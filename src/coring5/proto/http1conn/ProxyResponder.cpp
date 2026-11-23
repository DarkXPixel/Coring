#include "coring5/proto/http1conn/Responders.hpp"

#include "coring5/net/IdleUpstreams.hpp"
#include "coring5/proto/http1/ResponseWriter.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <cstring>

namespace Coring5 {
namespace {
void arm(IoContext &ctx, const ConnCore &core, TimerNode &timer,
         DeadlineKind kind, Millis after) {
  timer.owner = core.handle();
  timer.pool = core.self().pool;
  timer.kind = kind;
  ctx.timers.schedule(timer, ctx.now + after);
}

std::string_view peer_address(int fd, char (&out)[INET6_ADDRSTRLEN]) {
  sockaddr_storage addr{};
  socklen_t size = sizeof(addr);
  if (::getpeername(fd, reinterpret_cast<sockaddr *>(&addr), &size) != 0) {
    return {};
  }
  const void *ip;
  if (addr.ss_family == AF_INET) {
    ip = &reinterpret_cast<const sockaddr_in *>(&addr)->sin_addr;
  } else if (addr.ss_family == AF_INET6) {
    ip = &reinterpret_cast<const sockaddr_in6 *>(&addr)->sin6_addr;
  } else {
    return {};
  }
  return inet_ntop(addr.ss_family, ip, out, sizeof(out)) ? std::string_view(out)
                                                         : std::string_view{};
}

std::span<const ProxyHeader>
headers(const Http1::RequestHead &h,
        std::array<ProxyHeader, Http1::kMaxHeadersHard> &out) {
  for (size_t i = 0; i < h.header_count; ++i) {
    out[i] = {h.headers[i].name, h.headers[i].value};
  }
  return std::span(out).first(h.header_count);
}

bool websocket_upgrade(const Http1::RequestHead &h) {
  if (!h.upgrade || !h.keep_alive || h.minor_version != 1 || h.chunked ||
      h.has_content_length) {
    return false;
  }
  bool seen = false;
  for (const auto &field : std::span(h.headers).first(h.header_count)) {
    if (field.named("upgrade")) {
      if (seen || !Http1::Header{field.value, {}}.named("websocket")) {
        return false;
      }
      seen = true;
    }
  }
  return seen;
}

Result<size_t> request_head(std::span<char> buf, const Http1::RequestHead &h,
                            int fd, bool reusable, bool upgrade,
                            bool trailers) {
  char addr[INET6_ADDRSTRLEN];
  std::array<ProxyHeader, Http1::kMaxHeadersHard> input;
  std::array<ProxyHeader, Http1::kMaxHeadersHard + 3> output;
  const auto filtered = ReverseProxy::request_headers(
      headers(h, input), {peer_address(fd, addr), h.host}, output);
  if (!filtered) {
    return std::unexpected(filtered.error());
  }
  const bool have_host = std::ranges::any_of(*filtered, [](auto field) {
    return Http1::Header{field.name, field.value}.named("host");
  });
  if (h.minor_version == 1 && !have_host) {
    return unexpected_error(Errc::ProtocolViolation);
  }
  Http1::ResponseWriter writer(buf);
  (void)writer.request_line(h.method, h.target, h.minor_version);
  for (const auto &field : *filtered) {
    (void)writer.header(field.name, field.value);
  }
  if (h.has_content_length) {
    (void)writer.content_length(h.content_length);
  }
  if (h.chunked) {
    (void)writer.chunked();
  }
  if (upgrade) {
    (void)writer.header("Connection", "Upgrade");
    (void)writer.header("Upgrade", "websocket");
  } else if (trailers) {
    (void)writer.header("TE", "trailers");
    (void)writer.header("Connection", reusable ? "TE" : "close, TE");
  } else if (!reusable) {
    (void)writer.header("Connection", "close");
  }
  (void)writer.end_head();
  if (writer.error() != Errc::Ok) {
    return unexpected_error(writer.error());
  }
  return writer.size();
}
} // namespace

void ProxyResponder::start(IoContext &ctx, ConnCore &core, Exchange &ex,
                           const Http1::RequestHead &h) {
  if (!ctx.upstreams || tx_.bytes().empty() || head_.bytes().empty() ||
      body_.bytes().empty()) {
    (void)ex.respond_synthetic(503);
    finished_ = true;
    return;
  }
  upgrade_ = h.upgrade && h.method == "GET" &&
             h.find("expect").data() == nullptr && websocket_upgrade(h);
  if (h.upgrade && !upgrade_) {
    (void)ex.respond_synthetic(400);
    finished_ = true;
    return;
  }
  reuse_allowed_ = !upgrade_ && h.minor_version == 1 && ctx.idle_upstreams &&
                   limits_.keepalive_timeout > Millis::zero();
  forward_trailers_ = !upgrade_ && ex.request_meta().client_accepts_trailers;
  auto size = request_head(tx_.bytes(), h, core.fd(), reuse_allowed_, upgrade_,
                           forward_trailers_);
  if (!size || !proxy_.start(ex, {}, h.method)) {
    (void)ex.respond_synthetic(
        !size ? (size.error().code == Errc::TooLarge ? 431 : 400) : 501);
    finished_ = true;
    return;
  }
  unsent_ = tx_.bytes().first(*size);
  waiting_continue_ = !h.find("expect").empty();
  request_chunked_ = h.chunked;
  retry_deadline_ = ctx.now + ctx.upstreams->connect_timeout(
                                  proxy_.upstream(), limits_.connect_timeout);
  BackendRef first;
  if (reuse_allowed_) {
    if (auto picked = ctx.upstreams->pick(proxy_.upstream(), ctx.now)) {
      first = *picked;
      auto fd = ctx.idle_upstreams->take(first, ctx.now);
      if (fd) {
        backend_ = BackendLease(*ctx.upstreams, first);
        core.set_peer(std::move(fd));
        return;
      }
    }
  }
  dial(ctx, core, ex, first);
}

void ProxyResponder::dial(IoContext &ctx, ConnCore &core, Exchange &ex,
                          BackendRef first) {
  auto budget = std::chrono::ceil<Millis>(retry_deadline_ - ctx.now);
  dialing_ = dialer_.start(ctx, core, *ctx.upstreams, proxy_.upstream(), 1,
                           budget, first) == DialStatus::Pending;
  if (!dialing_) {
    fail(ctx, core, ex, dialer_.failure());
  }
}

void ProxyResponder::fail(IoContext &ctx, ConnCore &core, Exchange &ex,
                          Errc why) {
  read_timer_.cancel();
  write_timer_.cancel();
  reusable_ = false;
  if (why == Errc::Overloaded && !wrote_) {
    core.close(ctx, why);
    return;
  }
  const bool retry = !wrote_ && !pending() && ctx.now < retry_deadline_ &&
                     core.state() == ConnCore::State::Active;
  if (proxy_.on_failure(ex, why,
                        retry ? ProxyRetry::IfUnsent : ProxyRetry::Disallow)) {
    inc(ctx.metrics.upstream_retries);
    core.reset_peer(); // no peer operations in flight
    backend_.reset();
    dial(ctx, core, ex);
    return;
  }
  finished_ = true;
  if (pending() && core.has_peer()) {
    (void)::shutdown(core.peer_fd(), SHUT_RDWR);
  }
}

void ProxyResponder::upload(IoContext &ctx, ConnCore &core, Exchange &ex) {
  if (sending_ || (cursor_.head_done && !tunnel_)) {
    return;
  }
  if (unsent_.empty()) {
    if (upload_done_) {
      return;
    }
    auto buf = tx_.bytes();
    const auto prefix = request_chunked_ ? Http1::kChunkHeaderMax : 0;
    auto data =
        buf.subspan(prefix, buf.size() - prefix - (request_chunked_ ? 2 : 0));
    auto step = proxy_.prepare_request_body(ex, std::as_writable_bytes(data));
    if (!step) {
      fail(ctx, core, ex, step.error().code);
      return;
    }
    if (step->status == ProxyBodyStep::Status::Blocked) {
      return;
    }
    if (step->status == ProxyBodyStep::Status::Done) {
      upload_done_ = true;
      if (!request_chunked_) {
        return;
      }
      unsent_ = std::span("0\r\n\r\n", 5);
    } else {
      payload_ = step->size;
      if (request_chunked_) {
        std::array<char, Http1::kChunkHeaderMax> line;
        auto n = Http1::encode_chunk_header(line, payload_);
        std::memcpy(data.data() - n, line.data(), n);
        std::memcpy(data.data() + payload_, "\r\n", 2);
        unsent_ = buf.subspan(prefix - n, n + payload_ + 2);
      } else {
        unsent_ = data.first(payload_);
      }
    }
  }
  sending_ = core.counted(ctx.ring.send(core.peer_fd(), unsent_.data(),
                                        static_cast<uint32_t>(unsent_.size()),
                                        core.tag(OpKind::Send, kAuxPeer)));
  if (!sending_) {
    core.close(ctx, Errc::Overloaded);
  } else {
    arm(ctx, core, write_timer_, DeadlineKind::UpstreamWrite,
        limits_.idle_timeout);
  }
}

Result<> ProxyResponder::parse_heads(Exchange &ex, ChunkPool &stages) {
  auto buf = head_.bytes();
  while (head_used_ != 0) {
    Http1::RequestHead h;
    uint16_t status{};
    auto r =
        Http1::parse_response_head(buf.first(head_used_), limits_, h, status);
    if (r.status == Http1::ParseStatus::NeedMore) {
      return {};
    }
    const bool tunnel = status == 101;
    if (r.status == Http1::ParseStatus::Error ||
        (tunnel && (!upgrade_ || !websocket_upgrade(h))) ||
        ((status < 200 || status == 204) &&
         (h.chunked || h.has_content_length))) {
      return unexpected_error(Errc::ProtocolViolation);
    }
    auto tail = buf.subspan(r.consumed, head_used_ - r.consumed);
    if (status < 200 && !tunnel) {
      if (status == 100) {
        waiting_continue_ = false;
        if (!upload_done_) {
          read_timer_.cancel();
        }
        (void)proxy_.on_interim(ex, status);
      }
      std::memmove(buf.data(), tail.data(), tail.size());
      head_used_ = static_cast<uint32_t>(tail.size());
      continue;
    }
    ResponseMeta meta;
    meta.status = status;
    meta.declared_known = h.has_content_length;
    meta.declared = h.content_length;
    std::array<ProxyHeader, Http1::kMaxHeadersHard> input;
    auto published =
        proxy_.on_response_head(ex, cursor_, meta, headers(h, input), fields_, tunnel);
    if (!published) {
      return std::unexpected(published.error());
    }
    tunnel_ = tunnel;
    if (tunnel_) {
      upload_done_ = false;
    }
    // An early final may leave request bytes unsent. Recycle only a complete
    // request paired with a persistent, self-delimited response.
    reusable_ = reuse_allowed_ && h.keep_alive && !sending_ && unsent_.empty() &&
                (upload_done_ || !ex.request_meta().has_body) &&
                (h.chunked || h.has_content_length ||
                 content_forbidden(status, ex.request_meta().head_only));
    response_chunked_ = h.chunked;
    if (response_chunked_ && !content_forbidden(status, ex.request_meta().head_only)) {
      trailer_buffer_.emplace(stages);
      if (trailer_buffer_->bytes().empty()) {
        return unexpected_error(Errc::Overloaded);
      }
      decoder_.capture_trailers(trailer_buffer_->bytes());
      connection_token_count_ = h.connection_token_count;
      std::ranges::copy(std::span(h.connection_tokens).first(connection_token_count_),
                        connection_tokens_.begin());
    }
    length_known_ = h.has_content_length;
    remaining_ = h.content_length;
    staged_ = tail;
    return {};
  }
  return {};
}

Result<std::span<const ResponseField>> ProxyResponder::response_trailers() noexcept {
  // Done guarantees complete, syntax-checked lines and the blank terminator.
  auto wire = std::string_view(decoder_.trailer_block());
  std::size_t used = 0;
  while (wire.size() > 2) {
    const auto end = wire.find("\r\n");
    const auto line = wire.substr(0, end);
    const auto colon = line.find(':');
    const Http1::Header field{line.substr(0, colon), line.substr(colon + 1)};
    wire.remove_prefix(end + 2);

    const auto options = std::span(connection_tokens_).first(connection_token_count_);
    if (std::ranges::any_of(options, [&](auto option) {
          return field.named(option);
        })) {
      return unexpected_error(Errc::ProtocolViolation);
    }
    constexpr std::array<std::string_view, 15> dropped{
        "authorization", "proxy-authorization", "proxy-authenticate",
        "proxy-authentication-info", "www-authenticate", "cache-control",
        "content-encoding", "content-type", "content-range", "range",
        "max-forwards", "cookie", "set-cookie", "authentication-info", "location"};
    if (!forward_trailers_ ||
        (field.name.size() >= 3 && Http1::Header{field.name.substr(0, 3), {}}.named("if-")) ||
        std::ranges::any_of(dropped, [&](auto name) { return field.named(name); })) {
      continue;
    }
    if (used == trailer_fields_.size()) {
      return unexpected_error(Errc::TooLarge);
    }
    const auto first = field.value.find_first_not_of(" \t");
    const auto value = first == field.value.npos
                           ? std::string_view{}
                           : field.value.substr(first, field.value.find_last_not_of(" \t") - first + 1);
    trailer_fields_[used++] = {field.name, value};
  }
  return std::span(trailer_fields_).first(used);
}

Result<> ProxyResponder::consume_body(IoContext &ctx, Exchange &ex, uint32_t credit) {
  auto data = staged_.first(std::min<size_t>(credit, staged_.size()));
  bool end = false;
  if (response_chunked_) {
    auto step = decoder_.feed(staged_, credit);
    if (step.event == Http1::ChunkedEvent::Error) {
      if (Http1::is_trailer_error(step.reason)) {
        inc(ctx.metrics.trailers_rejected);
      }
      return unexpected_error(step.error);
    }
    data = step.data;
    staged_ = staged_.subspan(step.consumed);
    end = step.event == Http1::ChunkedEvent::Done;
  } else {
    if (length_known_) {
      data = data.first(std::min<uint64_t>(data.size(), remaining_));
      remaining_ -= data.size();
      end = remaining_ == 0;
    }
    staged_ = staged_.subspan(data.size());
  }
  std::span<const ResponseField> trailers;
  if (end && response_chunked_) {
    const auto fields = response_trailers();
    if (!fields) {
      inc(ctx.metrics.trailers_rejected);
      return std::unexpected(fields.error());
    }
    trailers = *fields;
    const auto dropped = decoder_.trailers_seen() - trailers.size();
    if (dropped != 0) {
      inc(ctx.metrics.trailers_dropped, dropped);
    }
  }
  auto result = proxy_.on_read(ex, cursor_, std::as_bytes(data), end, trailers);
  if (result) {
    finished_ = end;
  }
  return result;
}

void ProxyResponder::receive(IoContext &ctx, ConnCore &core, Exchange &ex) {
  if (!head_sent_ || receiving_) {
    return;
  }
  auto dest = head_.bytes().subspan(head_used_);
  if (cursor_.head_done) {
    auto body = body_.bytes();
    for (;;) {
      auto step = proxy_.apply(ex, cursor_, static_cast<uint32_t>(body.size()));
      if (!step) {
        fail(ctx, core, ex, step.error().code);
        return;
      }
      if (step->status == ProxyBodyStep::Status::Done) {
        finished_ = true;
        return;
      }
      if (step->status == ProxyBodyStep::Status::Blocked) {
        return;
      }
      if (staged_.empty()) {
        dest = body.first(step->size);
        break;
      }
      if (auto result = consume_body(ctx, ex, step->size); !result) {
        fail(ctx, core, ex, result.error().code);
        return;
      }
      if (finished_) {
        return;
      }
    }
  }
  auto len = static_cast<uint32_t>(
      std::min<size_t>(dest.size(), ctx.buffers.buf_size()));
  receiving_ =
      len && core.counted(ctx.ring.recv(core.peer_fd(), dest.data(), len,
                                        core.tag(OpKind::Recv, kAuxPeer)));
  if (!receiving_) {
    core.close(ctx, Errc::Overloaded);
  } else if ((cursor_.head_done || upload_done_ || waiting_continue_) &&
             !read_timer_.scheduled()) {
    arm(ctx, core, read_timer_, DeadlineKind::UpstreamRead,
        limits_.idle_timeout);
  }
}

void ProxyResponder::pump(IoContext &ctx, ConnCore &core, Exchange &ex) {
  if (!finished_ && !dialing_) {
    upload(ctx, core, ex);
    if (!finished_ && !core.closing()) {
      receive(ctx, core, ex);
    }
    if (receiving_ && upload_done_ && !sending_ && !cursor_.head_done &&
        !read_timer_.scheduled()) {
      arm(ctx, core, read_timer_, DeadlineKind::UpstreamRead,
          limits_.idle_timeout);
    }
  }
  if (finished_) {
    read_timer_.cancel();
    write_timer_.cancel();
  }
  if (finished_ && !pending() && core.has_peer()) {
    const auto ref = backend_.ref();
    backend_.reset();
    if (reusable_ && staged_.empty() && ctx.upstreams->endpoint(ref) &&
        ex.stream(StreamId::Res).state() == StreamState::Finished) {
      ctx.idle_upstreams->park(ref, ctx.now + limits_.keepalive_timeout,
                               core.take_peer());
    } else {
      core.reset_peer();
    }
  }
}

void ProxyResponder::complete(IoContext &ctx, ConnCore &core, Exchange &ex,
                              const Cqe &cqe) {
  if (dialing_) {
    const auto state = dialer_.on_cqe(ctx, core, cqe);
    if (state == DialStatus::Pending) {
      return;
    }
    dialing_ = false;
    if (state == DialStatus::Failed) {
      fail(ctx, core, ex, dialer_.failure());
    } else {
      auto connected = dialer_.take(core);
      backend_ = std::move(connected.lease);
      core.set_peer(std::move(connected.fd));
    }
    return;
  }
  if (cqe.tag.kind() == OpKind::Send) {
    write_timer_.cancel();
    sending_ = false;
    if (finished_ || (cursor_.head_done && !tunnel_)) {
      return;
    }
    if (cqe.res <= 0 || static_cast<size_t>(cqe.res) > unsent_.size()) {
      fail(ctx, core, ex, Errc::Reset);
      return;
    }
    wrote_ = true;
    unsent_ = unsent_.subspan(static_cast<size_t>(cqe.res));
    if (unsent_.empty()) {
      head_sent_ = true;
      if (payload_ && !proxy_.on_request_sent(ex, payload_)) {
        fail(ctx, core, ex, Errc::Canceled);
      }
      payload_ = 0;
    }
    return;
  }
  receiving_ = false;
  if (cursor_.head_done) {
    read_timer_.cancel();
  }
  if (finished_) {
    return;
  }
  if (cqe.res <= 0) {
    if (cqe.res == 0 && cursor_.head_done && !length_known_ &&
        !response_chunked_) {
      (void)proxy_.on_read(ex, cursor_, std::span<const std::byte>{}, true);
      finished_ = true;
    } else {
      fail(ctx, core, ex, Errc::Reset);
    }
    return;
  }
  auto n = static_cast<uint32_t>(cqe.res);
  if (!cursor_.head_done) {
    head_used_ += n;
    if (auto parsed = parse_heads(ex, *ctx.stage); !parsed) {
      fail(ctx, core, ex, parsed.error().code);
    }
    if (cursor_.head_done) {
      read_timer_.cancel();
    }
    if (cursor_.head_done && !tunnel_ && sending_) {
      (void)::shutdown(core.peer_fd(), SHUT_WR);
    }
  } else {
    staged_ = body_.bytes().first(n);
    if (auto result = consume_body(ctx, ex, ex.stream(StreamId::Res).claimed());
        !result) {
      fail(ctx, core, ex, result.error().code);
    }
  }
}
} // namespace Coring5
