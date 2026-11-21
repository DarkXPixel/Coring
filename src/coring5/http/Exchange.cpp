#include "coring5/http/Exchange.hpp"
#include <cassert>

namespace Coring5 {

Exchange::Exchange(ChunkPool &pool, StreamLimits limits, SideRef ingress,
                   RequestMeta meta) noexcept
    : req_(pool, limits), res_(pool, limits), ingress_(ingress),
      req_meta_(meta) {
  if (!meta.has_body) {
    (void)req_.finish();
  } else if (meta.declared_known) {
    (void)req_.set_declared(meta.declared);
  }
}

void Exchange::bind(Handle self, ReadyList &ready) noexcept {
  self_ = self;
  ready_ = &ready;
  raise(ExEvent::kHeadReady);
  harvest();
}

void Exchange::raise(uint16_t e) noexcept {
  if (e == 0) {
    return;
  }
  ing_events_ |= static_cast<uint16_t>(e & ExEvent::kIngressMask);
  resp_events_ |= static_cast<uint16_t>(e & ExEvent::kResponderMask);
  if (!listed_ && ready_ != nullptr && ready_->push(self_)) {
    listed_ = true;
  }
}

void Exchange::harvest() noexcept {
  uint16_t e = 0;
  const uint8_t r = req_.take_events();
  const uint8_t s = res_.take_events();
  if ((r & StreamEvent::kReadable) != 0) {
    e |= ExEvent::kReqReadable;
  }
  if ((r & StreamEvent::kWritable) != 0) {
    e |= ExEvent::kReqWritable;
  }
  if ((s & StreamEvent::kReadable) != 0) {
    e |= ExEvent::kResReadable;
  }
  if ((s & StreamEvent::kWritable) != 0) {
    e |= ExEvent::kResWritable;
  }
  if (!req_abort_noted_ && req_.state() == StreamState::Aborted) {
    req_abort_noted_ = true;
    e |= ExEvent::kReqAborted;
  }
  if (!res_abort_noted_ && res_.state() == StreamState::Aborted) {
    res_abort_noted_ = true;
    e |= ExEvent::kResAborted;
  }
  raise(e);
}

bool Exchange::attach_responder(SideRef responder) noexcept {
  if (phase_ != Phase::Routing || ingress_detached_ ||
      responder_state_ != ResponderState::None) {
    return false;
  }
  responder_ = responder;
  responder_state_ = ResponderState::Attached;
  phase_ = Phase::Active;
  raise(ExEvent::kResponderStart);
  return true;
}

HeadStatus Exchange::publish(ResponseMeta meta) noexcept {
  if (meta.status < 200 || meta.status > 599) {
    return HeadStatus::BadStatus;
  }
  if (ingress_detached_) {
    return HeadStatus::Gone;
  }
  meta.no_content =
      meta.no_content || content_forbidden(meta.status, req_meta_.head_only);
  res_meta_ = meta;
  head_published_ = true;
  interim_ = 0;
  phase_ = Phase::Responding;
  if (meta.no_content || meta.synthesized) {
    (void)res_.finish();
  } else if (meta.declared_known) {
    (void)res_.set_declared(meta.declared);
  }
  raise(ExEvent::kResponseHeadReady);
  harvest();
  return HeadStatus::Ok;
}

HeadStatus Exchange::respond_synthetic(uint16_t status) noexcept {
  if (phase_ != Phase::Routing && phase_ != Phase::Active) {
    return HeadStatus::WrongPhase;
  }
  ResponseMeta m;
  m.status = status;
  m.synthesized = true;
  m.declared_known = true;
  return publish(m);
}

HeadStatus Exchange::publish_final(const ResponseMeta &meta) noexcept {
  if (phase_ != Phase::Active) {
    return HeadStatus::WrongPhase;
  }
  return publish(meta);
}

HeadStatus Exchange::publish_interim(uint16_t status) noexcept {
  if (phase_ != Phase::Active) {
    return HeadStatus::WrongPhase;
  }
  if (status < 100 || status > 199 || status == 101) {
    return HeadStatus::BadStatus;
  }
  if (ingress_detached_) {
    return HeadStatus::Gone;
  }
  if (!req_meta_.accepts_interim) {
    return HeadStatus::Dropped;
  }
  interim_ = status; // 1-slot: coalesces with a pending one
  raise(ExEvent::kInterimReady);
  return HeadStatus::Ok;
}

bool Exchange::cancel_request_body() noexcept {
  if (phase_ != Phase::Responding || !head_published_) {
    return false;
  }
  req_.abort(Errc::Canceled);
  harvest();
  return true;
}

void Exchange::detach(Side who, Errc why) noexcept {
  if (who == Side::Ingress) {
    if (!ingress_detached_) {
      ingress_detached_ = true;
      if (req_.state() == StreamState::Open) {
        req_.abort(why);
      }
      res_.abort(Errc::Canceled);
    }
  } else if (responder_state_ != ResponderState::Detached) {
    responder_state_ = ResponderState::Detached;
    if (res_.state() == StreamState::Open) {
      res_.abort(why);
    }
    req_.abort(Errc::Canceled);
  }
  if (ingress_detached_ || phase_ == Phase::Responding) {
    phase_ = Phase::Closing;
  }
  harvest();
  raise(ExEvent::kPeerDetached); // always, so the reap check runs
}

bool Exchange::gate(StreamId s) noexcept {
  if (s == StreamId::Res && !head_published_) {
    assert(false && "R1: Res write before publish_final");
    res_.abort(Errc::Internal);
    harvest();
    return false;
  }
  return true;
}

ClaimResult Exchange::claim(StreamId s, uint32_t n) noexcept {
  if (!gate(s)) {
    return {};
  }
  const ClaimResult r = mut(s).claim(n);
  harvest();
  return r;
}

WriteStatus Exchange::push(StreamId s, std::span<const std::byte> d) noexcept {
  if (!gate(s)) {
    return WriteStatus::Closed;
  }
  const WriteStatus r = mut(s).push(d);
  harvest();
  return r;
}

WriteStatus Exchange::finish(StreamId s, bool has_trailers) noexcept {
  if (!gate(s)) {
    return WriteStatus::Closed;
  }
  const WriteStatus r = mut(s).finish(has_trailers);
  harvest();
  return r;
}

void Exchange::consume(StreamId s, uint32_t n) noexcept {
  mut(s).consume(n);
  harvest();
}

bool Exchange::wait_readable(StreamId s) noexcept {
  const bool r = mut(s).wait_readable();
  harvest();
  return r;
}

void Exchange::abort(StreamId s, Errc why) noexcept {
  mut(s).abort(why);
  harvest();
}

} // namespace Coring5
