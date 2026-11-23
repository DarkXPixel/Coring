#include "coring5/proto/http1session/ServerSession.hpp"
#include "coring5/http/Handlers.hpp"
#include "coring5/proto/http1/Parser.hpp"
#include "coring5/proto/http1/ResponseWriter.hpp"
#include <algorithm>
#include <cassert>
#include <cstring>

namespace Coring5::Http1 {

namespace {

[[nodiscard]] bool ieq(std::string_view a, std::string_view lower) noexcept {
  if (a.size() != lower.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    char c = a[i];
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
    if (c != lower[i]) {
      return false;
    }
  }
  return true;
}

bool accepts_trailers(const RequestHead &head) noexcept {
  const auto options = std::span(head.connection_tokens).first(head.connection_token_count);
  if (head.minor_version != 1 ||
      std::ranges::none_of(options, [](auto name) { return ieq(name, "te"); })) {
    return false;
  }
  for (const auto &field : std::span(head.headers).first(head.header_count)) {
    if (!field.named("te")) {
      continue;
    }
    auto list = field.value;
    while (!list.empty()) {
      const auto comma = list.find(',');
      const auto item = list.substr(0, comma);
      const auto first = item.find_first_not_of(" \t");
      if (first != item.npos &&
          ieq(item.substr(first, item.find_last_not_of(" \t") - first + 1), "trailers")) {
        return true;
      }
      list = comma == list.npos ? std::string_view{} : list.substr(comma + 1);
    }
  }
  return false;
}

[[nodiscard]] uint16_t status_of(ParseError e) noexcept {
  switch (e) {
  case ParseError::HeadTooLarge:
  case ParseError::TooManyHeaders:
    return 431;
  case ParseError::UriTooLong:
    return 414;
  default:
    return 400;
  }
}

[[nodiscard]] char *as_chars(std::byte *p) noexcept {
  return static_cast<char *>(static_cast<void *>(p));
}

} // namespace

ServerSession::ServerSession(ExchangePool &exchanges, ChunkPool &stage,
                             const Limits &limits, uint32_t buffer_size,
                             SideRef self, RequestObserver observer) noexcept
    : pool_(&exchanges), chunks_(&stage), limits_(limits),
      buffer_size_(buffer_size), cap_(stage.chunk_size()), self_(self),
      observer_(observer) {
  assert(cap_ >= limits.max_head_bytes + buffer_size);
  refresh();
}

ServerSession::~ServerSession() { release(); }

char *ServerSession::stage_ptr() const noexcept {
  return as_chars(chunks_->data(stage_idx_));
}
char *ServerSession::out_ptr() const noexcept {
  return as_chars(chunks_->data(out_idx_));
}

// ---- stage -----------------------------------------------------------------

bool ServerSession::stash(std::span<const char> d) noexcept {
  if (d.empty()) {
    return true;
  }
  if (static_cast<uint64_t>(stage_len_) + d.size() > cap_) {
    return false;
  }
  if (stage_idx_ == ChunkPool::kNil) {
    stage_idx_ = chunks_->alloc();
    if (stage_idx_ == ChunkPool::kNil) {
      fail_hard(Errc::Overloaded);
      return true; // handled
    }
    stage_len_ = 0;
  }
  std::memcpy(stage_ptr() + stage_len_, d.data(), d.size());
  stage_len_ += static_cast<uint32_t>(d.size());
  return true;
}

void ServerSession::clear_stage() noexcept {
  if (stage_idx_ != ChunkPool::kNil) {
    chunks_->free(stage_idx_);
    stage_idx_ = ChunkPool::kNil;
  }
  stage_len_ = 0;
}

void ServerSession::stage_erase(std::size_t n) noexcept {
  if (n == 0) {
    return;
  }
  if (n >= stage_len_) {
    clear_stage();
    return;
  }
  std::memmove(stage_ptr(), stage_ptr() + n, stage_len_ - n);
  stage_len_ -= static_cast<uint32_t>(n);
}

void ServerSession::overflow() noexcept {
  if (hard_) {
    return;
  }
  if (rx_ == RxState::Idle || rx_ == RxState::Head) {
    static_error(431, Errc::TooLarge);
  } else {
    fail_hard(Errc::TooLarge);
  }
}

// ---- credit / body
// -----------------------------------------------------------

ClaimStatus ServerSession::claim_credit(Exchange &ex, uint32_t want) noexcept {
  const ClaimResult r = ex.claim(StreamId::Req, want);
  if (r.status == ClaimStatus::Granted) {
    held_ = r.granted;
  }
  return r.status;
}

void ServerSession::release_credit() noexcept {
  if (held_ == 0) {
    return;
  }
  held_ = 0;
  if (Exchange *ex = cur()) {
    (void)ex->push(StreamId::Req, {});
  }
}

void ServerSession::stop_rx() noexcept {
  release_credit();
  if (!req_done_) {
    error_close_ = true;
  }
  expect_ = false;
  rx_ = RxState::Stopped;
}

bool ServerSession::push_ok(WriteStatus st) noexcept {
  switch (st) {
  case WriteStatus::Ok:
    return true;
  case WriteStatus::Exhausted:
    body_failed(Errc::Overloaded, 503);
    return false;
  case WriteStatus::Violation:
    body_failed(Errc::ProtocolViolation, 400);
    return false;
  case WriteStatus::Closed:
    stop_rx();
    return false;
  }
  return false;
}

void ServerSession::body_complete(Exchange &ex) noexcept {
  release_credit();
  (void)ex.finish(StreamId::Req, false);
  req_done_ = true;
  expect_ = false;
  rx_ = RxState::Held;
}

void ServerSession::body_failed(Errc why, uint16_t status) noexcept {
  release_credit();
  Exchange *ex = cur();
  if (ex != nullptr) {
    ex->abort(StreamId::Req, why);
  }
  if (ex != nullptr && tx_ == TxState::Idle && !ex->head_published()) {
    static_error(status, why);
  } else {
    error_close_ = true;
    stop_rx();
  }
  clear_stage();
}

std::size_t ServerSession::run_body(std::span<const char> buf) noexcept {
  std::size_t pos = 0;
  while (pos < buf.size()) {
    Exchange *ex = cur();
    if (ex == nullptr) {
      stop_rx();
      return buf.size();
    }
    if (rx_ == RxState::ExpectWait) { // body bytes arrived: no 100 any more
      rx_ = RxState::Body;
      expect_ = false;
    }
    const auto rest = buf.subspan(pos);
    if (mode_ == Mode::Tunnel) {
      if (held_ == 0) {
        const auto want = static_cast<uint32_t>(std::min<std::size_t>(rest.size(), buffer_size_));
        const auto credit = claim_credit(*ex, want);
        if (credit == ClaimStatus::Blocked) {
          rx_ = RxState::BodyBlocked;
          return pos;
        }
        if (credit == ClaimStatus::Closed) {
          stop_rx();
          return buf.size();
        }
      }
      const auto n = std::min<std::size_t>(rest.size(), held_);
      held_ = 0;
      if (ex->push(StreamId::Req, std::as_bytes(rest.first(n))) != WriteStatus::Ok) {
        fail_hard(ex->stream(StreamId::Req).aborted_with());
        return buf.size();
      }
      pos += n;
      outcome_.bytes_in += n;
      continue;
    }
    if (body_kind_ == BodyKind::ContentLength) {
      if (held_ == 0) {
        const auto want = static_cast<uint32_t>(
            std::min<uint64_t>({rest.size(), cl_.remaining(), buffer_size_}));
        const ClaimStatus st = claim_credit(*ex, want);
        if (st == ClaimStatus::Blocked) {
          rx_ = RxState::BodyBlocked;
          return pos;
        }
        if (st == ClaimStatus::Closed) {
          stop_rx();
          return buf.size();
        }
      }
      const std::size_t n = cl_.take(std::min<std::size_t>(rest.size(), held_));
      pos += n;
      outcome_.bytes_in += n;
      held_ = 0; // push settles the claim
      if (!push_ok(ex->push(StreamId::Req, std::as_bytes(rest.first(n))))) {
        return buf.size();
      }
      if (cl_.done()) {
        body_complete(*ex);
        return pos;
      }
      continue;
    }
    // chunked
    const ChunkedStep step = dec_.feed(rest, held_);
    pos += step.consumed;
    outcome_.bytes_in += step.consumed;
    switch (step.event) {
    case ChunkedEvent::NeedMore:
      break;
    case ChunkedEvent::NeedCredit: {
      if (pos >= buf.size()) {
        break;
      }
      const auto want = static_cast<uint32_t>(
          std::min<std::size_t>(buf.size() - pos, buffer_size_));
      const ClaimStatus st = claim_credit(*ex, want);
      if (st == ClaimStatus::Blocked) {
        rx_ = RxState::BodyBlocked;
        return pos;
      }
      if (st == ClaimStatus::Closed) {
        stop_rx();
        return buf.size();
      }
      break;
    }
    case ChunkedEvent::Data:
      if (limits_.max_request_body_bytes != 0 &&
          dec_.body_bytes() > limits_.max_request_body_bytes) {
        body_failed(Errc::TooLarge, 413);
        return buf.size();
      }
      held_ = 0;
      if (!push_ok(ex->push(StreamId::Req, std::as_bytes(step.data)))) {
        return buf.size();
      }
      break;
    case ChunkedEvent::Done:
      if (observer_.on_trailers && dec_.trailers_seen() != 0) {
        observer_.on_trailers(observer_.context, 0, dec_.trailers_seen(), false);
      }
      body_complete(*ex);
      return pos;
    case ChunkedEvent::Error:
      if (observer_.on_trailers && is_trailer_error(step.reason)) {
        observer_.on_trailers(observer_.context, 0, 0, true);
      }
      body_failed(step.error == Errc::TooLarge ? Errc::TooLarge
                                               : Errc::ProtocolViolation,
                  step.error == Errc::TooLarge ? 413 : 400);
      return buf.size();
    }
  }
  return pos;
}

// ---- head ------------------------------------------------------------------

void ServerSession::handle_head(const RequestHead &h,
                                std::size_t tail) noexcept {
  ++served_;
  req_minor_ = h.minor_version;
  req_ka_ = h.keep_alive;
  head_only_ = h.method == "HEAD";
  req_done_ = false;
  expect_ = false;

  uint32_t expects = 0;
  bool bad_expect = false;
  if (req_minor_ >= 1) {
    for (std::size_t i = 0; i < h.header_count; ++i) {
      if (ieq(h.headers[i].name, "expect")) {
        ++expects;
        if (!ieq(h.headers[i].value, "100-continue")) {
          bad_expect = true;
        }
      }
    }
  }
  if (expects > 1 || bad_expect) {
    static_error(417);
    return;
  }
  const BodyPlan plan = begin_body(h);
  if (plan.error != Errc::Ok) {
    static_error(400);
    return;
  }
  if (plan.kind == BodyKind::ContentLength &&
      limits_.max_request_body_bytes != 0 &&
      plan.length > limits_.max_request_body_bytes) {
    static_error(413, Errc::TooLarge);
    return;
  }
  RequestMeta meta;
  meta.head_only = head_only_;
  meta.has_body = plan.kind != BodyKind::None;
  meta.declared_known = plan.kind == BodyKind::ContentLength;
  meta.declared = plan.length;
  meta.accepts_interim = req_minor_ >= 1;
  meta.client_accepts_trailers = accepts_trailers(h);
  const Handle ex = pool_->open(self_, meta);
  if (!ex.valid()) {
    static_error(503, Errc::Overloaded);
    return;
  }
  ex_ = ex;
  fresh_ = ex;
  if (observer_.on_head) {
    observer_.on_head(observer_.context, h);
  }
  body_kind_ = plan.kind;
  if (plan.kind == BodyKind::ContentLength) {
    cl_.start(plan.length);
  } else if (plan.kind == BodyKind::Chunked) {
    dec_ = ChunkedDecoder(limits_);
  }
  if (!meta.has_body) {
    rx_ = RxState::Held;
    req_done_ = true;
  } else if (expects == 1 && tail == 0) {
    rx_ = RxState::ExpectWait;
    expect_ = true;
  } else {
    rx_ = RxState::Body;
  }
}

std::size_t ServerSession::pump(std::span<const char> buf) noexcept {
  std::size_t pos = 0;
  while (pos < buf.size()) {
    switch (rx_) {
    case RxState::Idle:
      rx_ = RxState::Head;
      if (observer_.on_complete) {
        outcome_ = {};
        observing_request_ = true;
        if (observer_.on_begin) {
          observer_.on_begin(observer_.context);
        }
      }
      [[fallthrough]];
    case RxState::Head: {
      RequestHead head;
      const HeadResult r = parse_head(buf.subspan(pos), limits_, head);
      if (r.status == ParseStatus::NeedMore) {
        return pos;
      }
      if (r.status == ParseStatus::Error) {
        static_error(status_of(r.error),
                     r.error == ParseError::HeadTooLarge ||
                             r.error == ParseError::UriTooLong ||
                             r.error == ParseError::TooManyHeaders
                         ? Errc::TooLarge
                         : Errc::ProtocolViolation);
        return buf.size();
      }
      const std::size_t tail = buf.size() - pos - r.consumed;
      pos += r.consumed;
      outcome_.bytes_in = r.consumed;
      if (observer_.on_parsed) {
        observer_.on_parsed(observer_.context, head);
      }
      handle_head(head, tail);
      if (rx_ == RxState::Stopped) {
        return buf.size();
      }
      break;
    }
    case RxState::ExpectWait:
    case RxState::Body: {
      pos += run_body(buf.subspan(pos));
      if (rx_ == RxState::Stopped) {
        return buf.size();
      }
      if (rx_ == RxState::BodyBlocked) {
        return pos;
      }
      break;
    }
    case RxState::BodyBlocked:
    case RxState::Held:
      return pos;
    case RxState::Stopped:
      return buf.size();
    }
  }
  return pos;
}

void ServerSession::run_stage() noexcept {
  if (hard_ || stage_len_ == 0) {
    return;
  }
  if (rx_ == RxState::Stopped) {
    clear_stage();
    return;
  }
  const std::size_t c = pump({stage_ptr(), stage_len_});
  if (rx_ == RxState::Stopped) {
    clear_stage();
    return;
  }
  stage_erase(c);
}

// ---- public rx
// -----------------------------------------------------------------

uint32_t ServerSession::prepare_recv() noexcept {
  uint32_t r = 0;
  if (!hard_ && !want_close_ && !peer_eof_) {
    switch (rx_) {
    case RxState::Idle:
    case RxState::Head:
      r = std::min(buffer_size_, cap_ - stage_len_);
      break;
    case RxState::ExpectWait:
    case RxState::Body: {
      if (stage_len_ > 0) {
        break;
      }
      if (held_ > 0) {
        r = held_;
        break;
      }
      Exchange *ex = cur();
      if (ex == nullptr) {
        break;
      }
      const auto want = body_kind_ == BodyKind::ContentLength
                            ? static_cast<uint32_t>(std::min<uint64_t>(
                                  buffer_size_, cl_.remaining()))
                            : buffer_size_;
      const ClaimStatus st = claim_credit(*ex, want);
      if (st == ClaimStatus::Granted) {
        r = held_;
      } else if (st == ClaimStatus::Blocked) {
        rx_ = RxState::BodyBlocked;
      } else {
        stop_rx();
      }
      break;
    }
    case RxState::BodyBlocked:
    case RxState::Held:
    case RxState::Stopped:
      break;
    }
  }
  refresh();
  return r;
}

void ServerSession::on_data(std::span<const char> in) noexcept {
  progress_ = true;
  if (hard_ || rx_ == RxState::Stopped) {
    release_credit();
    refresh();
    return;
  }
  if (stage_len_ == 0) {
    const std::size_t c = pump(in);
    if (rx_ != RxState::Stopped && !hard_ && c < in.size() &&
        !stash(in.subspan(c))) {
      overflow();
    }
  } else if (!stash(in)) {
    overflow();
  } else {
    run_stage();
  }
  release_credit();
  refresh();
}

void ServerSession::on_eof(Errc why) noexcept {
  progress_ = true;
  peer_eof_ = true;
  release_credit();
  switch (rx_) {
  case RxState::Idle:
  case RxState::Head:
    clear_stage();
    want_close_ = true;
    break;
  case RxState::ExpectWait:
  case RxState::Body:
  case RxState::BodyBlocked:
    fail_hard(why == Errc::Eof && mode_ != Mode::Tunnel
                  ? Errc::ProtocolViolation
                  : why);
    break;
  case RxState::Held:
  case RxState::Stopped:
    if (why != Errc::Eof) {
      fail_hard(why);
    } else if (!ex_.valid() && tx_ != TxState::Idle) {
      want_close_ = true;
    } else if (!ex_.valid()) {
      want_close_ = true;
    }
    break;
  }
  refresh();
}

// ---- exchange events
// -----------------------------------------------------------

void ServerSession::on_exchange_events() noexcept {
  Exchange *ex = cur();
  if (hard_ || ex == nullptr) {
    return;
  }
  progress_ = true;
  const uint16_t ev = ex->take_events(Side::Ingress);

  if ((ev & ExEvent::kReqWritable) != 0 && rx_ == RxState::BodyBlocked) {
    rx_ = RxState::Body;
    run_stage();
    ex = cur();
    if (ex == nullptr) {
      refresh();
      return;
    }
  }
  if ((ev & ExEvent::kReqAborted) != 0 &&
      ex->stream(StreamId::Req).state() == StreamState::Aborted &&
      (rx_ == RxState::Body || rx_ == RxState::BodyBlocked ||
       rx_ == RxState::ExpectWait)) {
    stop_rx();
    clear_stage();
  }
  if ((ev & ExEvent::kInterimReady) != 0) {
    const uint16_t st = ex->take_interim();
    if (st == 100 && expect_ && rx_ == RxState::ExpectWait &&
        tx_ == TxState::Idle && !output_pending() && !ex->head_published() &&
        ex->stream(StreamId::Res).state() == StreamState::Open &&
        begin_out() && kContinue100.size() <= cap_) {
      std::memcpy(out_ptr(), kContinue100.data(), kContinue100.size());
      out_len_ = static_cast<uint32_t>(kContinue100.size());
      out_off_ = 0;
      tx_ = TxState::Interim;
      expect_ = false;
    }
  }
  tx_pump();
  refresh();
}

// ---- tx
// ----------------------------------------------------------------------

bool ServerSession::begin_out() noexcept {
  if (out_idx_ == ChunkPool::kNil) {
    out_idx_ = chunks_->alloc();
    if (out_idx_ == ChunkPool::kNil) {
      fail_hard(Errc::Overloaded);
      return false;
    }
    out_off_ = out_len_ = 0;
  } else if (!output_pending()) {
    out_off_ = out_len_ = 0;
  }
  return true;
}

void ServerSession::static_error(uint16_t status, Errc why) noexcept {
  if (hard_) {
    return;
  }
  if (tx_ != TxState::Idle && tx_ != TxState::Interim) {
    fail_hard(why);
    return;
  }
  if (!begin_out()) {
    return;
  }
  ResponseWriter w(std::span<char>(out_ptr() + out_len_, cap_ - out_len_));
  (void)w.status_line(status, 1);
  (void)w.content_length(0);
  (void)w.header("Connection", "close");
  (void)w.end_head();
  if (w.error() != Errc::Ok) {
    fail_hard(Errc::Internal);
    return;
  }
  out_len_ += static_cast<uint32_t>(w.size());
  outcome_.status = status;
  close_why_ = why;
  error_close_ = true;
  want_close_ = true;
  expect_ = false;
  rx_ = RxState::Stopped;
  tx_ = TxState::Done;
  drop_exchange(why);
  clear_stage();
}

void ServerSession::build_head(Exchange &ex) noexcept {
  const ResponseMeta m = ex.response_meta();
  const auto fields = ex.take_response_fields();
  const BodyStream &rs = ex.stream(StreamId::Res);
  if (rs.state() == StreamState::Aborted && !m.synthesized) {
    const Errc e = rs.aborted_with();
    static_error(status_for(e), e);
    return;
  }
  const bool tunnel = ex.tunneling();
  const bool forbidden = content_forbidden(m.status, head_only_);
  bool send_cl = false;
  uint64_t cl = 0;
  if (tunnel) {
    mode_ = Mode::Tunnel;
  } else if (forbidden) {
    mode_ = Mode::None;
    if (head_only_ && m.declared_known && m.status >= 200 && m.status != 204 &&
        m.status != 304) {
      send_cl = true;
      cl = m.declared;
    }
  } else if (m.no_content || m.declared_known) {
    send_cl = true;
    cl = m.no_content ? 0 : m.declared;
    mode_ = cl > 0 ? Mode::Length : Mode::None;
    tx_left_ = cl;
  } else if (req_minor_ >= 1) {
    mode_ = Mode::Chunked;
  } else {
    mode_ = Mode::CloseDelimited;
    tx_left_ = UINT64_MAX;
  }

  const bool incomplete = !req_done_;
  KeepAliveInput k;
  k.request_keep_alive = req_ka_;
  k.close_delimited = mode_ == Mode::CloseDelimited;
  k.request_incomplete = incomplete;
  k.draining = drain_;
  k.limit_reached =
      limits_.max_requests != 0 && served_ >= limits_.max_requests;
  k.errored = error_close_ || peer_eof_;
  keep_ = !tunnel && keep_alive_after(k);
  if (incomplete) {
    // Early final response: stop reading the body and close afterwards.
    release_credit();
    (void)ex.cancel_request_body();
    stop_rx();
    clear_stage();
  }

  ResponseWriter w(std::span<char>(out_ptr(), cap_));
  (void)w.status_line(m.status, 1);
  for (const ResponseField &field : fields) {
    if (forbidden_static_header(field.name)) {
      static_error(500, Errc::Internal);
      return;
    }
    (void)w.header(field.name, field.value);
  }
  if (mode_ == Mode::Chunked) {
    (void)w.chunked();
  } else if (send_cl) {
    (void)w.content_length(cl);
  }
  const std::string_view tok = connection_token(keep_, req_minor_);
  if (tunnel) {
    (void)w.header("Connection", "Upgrade");
    (void)w.header("Upgrade", "websocket");
  } else if (!tok.empty()) {
    (void)w.header("Connection", tok);
  }
  (void)w.end_head();
  if (w.error() != Errc::Ok) {
    static_error(500, w.error());
    return;
  }
  out_off_ = 0;
  out_len_ = static_cast<uint32_t>(w.size());
  outcome_.status = m.status;

  if (mode_ == Mode::Length) { // coalesce a small, already complete body
    const auto f = ex.front(StreamId::Res);
    if (f.size() >= tx_left_ && tx_left_ <= cap_ - out_len_) {
      const auto n = static_cast<uint32_t>(tx_left_);
      std::memcpy(out_ptr() + out_len_, f.data(), n);
      ex.consume(StreamId::Res, n);
      out_len_ += n;
      tx_left_ = 0;
    }
  }
  tx_ = TxState::Head;
  if (tunnel) {
    rx_ = RxState::Body;
    run_stage();
  }
}

void ServerSession::res_failed() noexcept {
  // Response broke after its head: close without a terminating chunk.
  keep_ = false;
  error_close_ = true;
  want_close_ = true;
  close_why_ = Errc::Canceled;
  tx_ = TxState::Done;
  drop_exchange(Errc::Canceled);
}

void ServerSession::tx_pump() noexcept {
  for (;;) {
    Exchange *ex = cur();
    if (ex == nullptr || hard_ || payload_out_ || output_pending()) {
      return;
    }
    out_off_ = out_len_ = 0;
    switch (tx_) {
    case TxState::Idle:
      if (!ex->head_published()) {
        // The failure event may have arrived while an interim send was pending.
        const auto &res = ex->stream(StreamId::Res);
        if (res.state() == StreamState::Aborted ||
            ex->phase() == Phase::Closing) {
          const Errc e = res.aborted_with();
          const Errc why = e == Errc::Ok ? Errc::Internal : e;
          static_error(status_for(why), why);
        }
        return;
      }
      if (!begin_out()) {
        return;
      }
      build_head(*ex);
      if (hard_ || cur() == nullptr) {
        return;
      }
      continue;
    case TxState::Interim:
      tx_ = TxState::Idle;
      continue;
    case TxState::Head:
      switch (mode_) {
      case Mode::None:
        tx_ = TxState::End;
        break;
      case Mode::Length:
        tx_ = tx_left_ > 0 ? TxState::Payload : TxState::End;
        break;
      case Mode::Chunked:
        tx_ = TxState::ChunkHdr;
        break;
      case Mode::CloseDelimited:
      case Mode::Tunnel:
        tx_ = TxState::Payload;
        break;
      }
      continue;
    case TxState::ChunkHdr: {
      const auto f = ex->front(StreamId::Res);
      if (f.empty()) {
        const BodyStream &rs = ex->stream(StreamId::Res);
        if (rs.end_seen()) {
          if (rs.aborted_with() != Errc::Ok) {
            res_failed();
            return;
          }
          const auto trailers = ex->response_trailers();
          std::array<FieldView, kMaxHeadersHard> fields;
          if (trailers.size() > fields.size() ||
              std::ranges::any_of(trailers, [](auto field) {
                return forbidden_static_header(field.name);
              })) {
            if (observer_.on_trailers) {
              observer_.on_trailers(observer_.context, 0, 0, true);
            }
            res_failed();
            return;
          }
          std::ranges::transform(trailers, fields.begin(), [](auto field) {
            return FieldView{field.name, field.value};
          });
          auto n = write_last_chunk({out_ptr(), cap_},
                                    std::span(fields).first(trailers.size()));
          if (!n) {
            fail_hard(Errc::Internal);
            return;
          }
          if (observer_.on_trailers && !trailers.empty()) {
            observer_.on_trailers(observer_.context,
                                  static_cast<uint32_t>(trailers.size()), 0, false);
          }
          out_len_ = static_cast<uint32_t>(*n);
          tx_ = TxState::End;
          continue;
        }
        if (ex->wait_readable(StreamId::Res)) {
          return;
        }
        continue;
      }
      pl_ = static_cast<uint32_t>(f.size());
      const std::size_t n = encode_chunk_header({out_ptr(), cap_}, pl_);
      out_len_ = static_cast<uint32_t>(n);
      tx_ = TxState::Payload;
      return;
    }
    case TxState::Payload: {
      if (!ex->front(StreamId::Res).empty()) {
        return;
      }
      const BodyStream &rs = ex->stream(StreamId::Res);
      if (rs.end_seen()) {
        if (rs.aborted_with() != Errc::Ok ||
            (mode_ != Mode::CloseDelimited && mode_ != Mode::Tunnel)) {
          res_failed();
          return;
        }
        want_close_ = true; // close-delimited body ends with the connection
        keep_ = false;
        tx_ = TxState::End;
        continue;
      }
      if (ex->wait_readable(StreamId::Res)) {
        return;
      }
      continue;
    }
    case TxState::End:
      finish_cycle();
      return;
    case TxState::Done:
      return;
    }
  }
}

void ServerSession::finish_cycle() noexcept {
  complete_request();
  const bool keep = keep_ && !error_close_ && !drain_ && !peer_eof_;
  drop_exchange(Errc::Ok);
  if (out_idx_ != ChunkPool::kNil) {
    chunks_->free(out_idx_);
    out_idx_ = ChunkPool::kNil;
  }
  out_off_ = out_len_ = 0;
  mode_ = Mode::None;
  if (!keep) {
    tx_ = TxState::Done;
    want_close_ = true;
    rx_ = RxState::Stopped;
    clear_stage();
    return;
  }
  tx_ = TxState::Idle;
  rx_ = RxState::Idle;
  req_done_ = false;
  expect_ = false;
  run_stage(); // pipelined next request, if already buffered
}

std::span<const std::byte> ServerSession::next_send() noexcept {
  std::span<const std::byte> r{};
  if (!hard_) {
    if (output_pending()) {
      send_src_ = 0;
      r = {chunks_->data(out_idx_) + out_off_, out_len_ - out_off_};
    } else if (Exchange *ex = cur(); ex != nullptr && tx_ == TxState::Payload) {
      auto f = ex->front(StreamId::Res);
      std::size_t n = f.size();
      if (mode_ == Mode::Chunked) {
        n = std::min<std::size_t>(n, pl_);
      } else if (mode_ == Mode::Length) {
        n = std::min<uint64_t>(n, tx_left_);
      }
      if (n > 0) {
        send_src_ = 1;
        payload_out_ = true;
        // An asynchronous producer can abort the stream and recycle its chunks
        // while this send is pending. The session output slot lives to the CQE.
        n = std::min<std::size_t>(n, cap_);
        std::memcpy(out_ptr(), f.data(), n);
        r = {chunks_->data(out_idx_), n};
      }
    }
  }
  refresh();
  return r;
}

void ServerSession::on_sent(uint32_t n) noexcept {
  progress_ = true;
  outcome_.bytes_out += n;
  if (hard_) {
    payload_out_ = false;
    return;
  }
  if (send_src_ == 0) {
    out_off_ = std::min(out_len_, out_off_ + n);
    if (out_off_ == out_len_) {
      out_off_ = out_len_ = 0;
    }
  } else {
    payload_out_ = false;
    if (Exchange *ex = cur()) {
      if (ex->stream(StreamId::Res).state() == StreamState::Aborted) {
        res_failed();
        refresh();
        return;
      }
      ex->consume(StreamId::Res, n);
      if (mode_ == Mode::Chunked) {
        pl_ -= std::min(pl_, n);
        if (pl_ == 0) {
          std::memcpy(out_ptr(), kChunkCrlf.data(), kChunkCrlf.size());
          out_off_ = 0;
          out_len_ = static_cast<uint32_t>(kChunkCrlf.size());
          tx_ = TxState::ChunkHdr;
        }
      } else {
        tx_left_ -= std::min<uint64_t>(tx_left_, n);
        if (tx_left_ == 0 && mode_ == Mode::Length) {
          tx_ = TxState::End;
        }
      }
    }
  }
  if (tx_ == TxState::Done && !output_pending()) {
    complete_request(); // static error responses have no exchange to pump
  }
  tx_pump();
  refresh();
}

// ---- lifecycle
// -----------------------------------------------------------------

void ServerSession::drop_exchange(Errc why) noexcept {
  Exchange *ex = cur();
  release_credit();
  if (ex != nullptr) {
    ex->detach(Side::Ingress, why);
    // Anonymous responders are local producers owned by this session.
    if (ex->responder_state() == Exchange::ResponderState::Attached &&
        !ex->responder().conn.handle.valid()) {
      ex->detach(Side::Responder, why);
    }
  }
  ex_ = {};
}

void ServerSession::fail_hard(Errc why) noexcept {
  if (hard_) {
    return;
  }
  hard_ = true;
  want_close_ = true;
  close_why_ = why;
  rx_ = RxState::Stopped;
  tx_ = TxState::Done;
  expect_ = false;
  clear_stage();
  // A pending payload CQE still belongs to this exchange. release() settles
  // its lifetime when the connection has drained its operations.
  if (!payload_out_) {
    drop_exchange(why);
  }
}

void ServerSession::release() noexcept {
  complete_request();
  payload_out_ = false;
  drop_exchange(Errc::Shutdown);
  clear_stage();
  if (out_idx_ != ChunkPool::kNil) {
    chunks_->free(out_idx_);
    out_idx_ = ChunkPool::kNil;
  }
  out_off_ = out_len_ = 0;
  held_ = 0;
}

void ServerSession::complete_request() noexcept {
  if (observing_request_) {
    observing_request_ = false;
    observer_.on_complete(observer_.context, outcome_);
  }
}

void ServerSession::on_drain() noexcept {
  drain_ = true;
  progress_ = true;
  if (!ex_.valid() && tx_ == TxState::Idle && stage_len_ == 0 &&
      rx_ == RxState::Idle) {
    want_close_ = true;
    close_why_ = Errc::Shutdown;
  }
  refresh();
}

void ServerSession::refresh_limits(const Limits &l) noexcept {
  if (ex_.valid() || stage_len_ != 0 || rx_ != RxState::Idle || hard_ ||
      want_close_ || l == limits_) {
    return;
  }
  if (static_cast<uint64_t>(l.max_head_bytes) + buffer_size_ > cap_) {
    return;
  }
  limits_ = l;
  progress_ = true; // re-arm the idle timer with the new value
  refresh();
}

void ServerSession::on_deadline(SessionDeadline kind) noexcept {
  progress_ = true;
  switch (kind) {
  case SessionDeadline::None:
    break;
  case SessionDeadline::KeepAlive:
    want_close_ = true;
    close_why_ = Errc::Timeout;
    break;
  case SessionDeadline::HeaderRead:
    if (tx_ == TxState::Idle && !ex_.valid()) {
      static_error(408, Errc::Timeout);
    } else {
      fail_hard(Errc::Timeout);
    }
    break;
  case SessionDeadline::BodyIdle:
  case SessionDeadline::UpstreamFirstByte: {
    Exchange *ex = cur();
    if (ex != nullptr && tx_ == TxState::Idle && !ex->head_published()) {
      static_error(kind == SessionDeadline::BodyIdle ? 408 : 504,
                   Errc::Timeout);
    } else {
      fail_hard(Errc::Timeout);
    }
    break;
  }
  case SessionDeadline::WriteStall:
  case SessionDeadline::MaxLifetime:
    fail_hard(Errc::Timeout);
    break;
  }
  refresh();
}

SessionDeadline ServerSession::compute_kind() const noexcept {
  if (hard_) {
    return SessionDeadline::None;
  }
  if (output_pending() || payload_out_) {
    return SessionDeadline::WriteStall;
  }
  if (want_close_) {
    return SessionDeadline::None;
  }
  if (!ex_.valid()) {
    if (stage_len_ > 0 || rx_ == RxState::Head || served_ == 0) {
      return SessionDeadline::HeaderRead;
    }
    return SessionDeadline::KeepAlive;
  }
  if (rx_ == RxState::Body || rx_ == RxState::BodyBlocked) {
    return SessionDeadline::BodyIdle;
  }
  if (rx_ == RxState::ExpectWait) {
    return SessionDeadline::UpstreamFirstByte;
  }
  const Exchange *ex = pool_->get(ex_);
  return ex != nullptr && ex->head_published()
             ? SessionDeadline::BodyIdle
             : SessionDeadline::UpstreamFirstByte;
}

void ServerSession::refresh() noexcept {
  const SessionDeadline k = compute_kind();
  if (k != dl_.kind || progress_) {
    dl_.kind = k;
    switch (k) {
    case SessionDeadline::HeaderRead:
      dl_.after = limits_.header_timeout;
      break;
    case SessionDeadline::KeepAlive:
      dl_.after = limits_.keepalive_timeout;
      break;
    case SessionDeadline::None:
    case SessionDeadline::MaxLifetime:
      dl_.after = Millis{0};
      break;
    default:
      dl_.after = limits_.idle_timeout;
      break;
    }
    ++dl_.epoch;
  }
  progress_ = false;
}

} // namespace Coring5::Http1
