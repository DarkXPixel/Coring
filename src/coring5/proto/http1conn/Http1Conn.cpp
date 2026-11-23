#include "coring5/proto/http1conn/Http1Conn.hpp"

#include <algorithm>
#include <cerrno>
#include <limits>

namespace Coring5 {

namespace {

using Http1::SessionDeadline;

DeadlineKind to_kind(SessionDeadline k) noexcept {
  switch (k) {
  case SessionDeadline::KeepAlive:
    return DeadlineKind::KeepAlive;
  case SessionDeadline::HeaderRead:
    return DeadlineKind::HeaderRead;
  case SessionDeadline::BodyIdle:
    return DeadlineKind::BodyIdle;
  case SessionDeadline::WriteStall:
    return DeadlineKind::WriteStall;
  case SessionDeadline::UpstreamFirstByte:
    return DeadlineKind::UpstreamFirstByte;
  case SessionDeadline::MaxLifetime:
    return DeadlineKind::MaxLifetime;
  case SessionDeadline::None:
    break;
  }
  return DeadlineKind::Idle;
}

SessionDeadline from_kind(DeadlineKind k) noexcept {
  switch (k) {
  case DeadlineKind::KeepAlive:
    return SessionDeadline::KeepAlive;
  case DeadlineKind::HeaderRead:
    return SessionDeadline::HeaderRead;
  case DeadlineKind::BodyIdle:
    return SessionDeadline::BodyIdle;
  case DeadlineKind::WriteStall:
    return SessionDeadline::WriteStall;
  case DeadlineKind::UpstreamFirstByte:
    return SessionDeadline::UpstreamFirstByte;
  case DeadlineKind::MaxLifetime:
    return SessionDeadline::MaxLifetime;
  default:
    return SessionDeadline::None;
  }
}

} // namespace

void Http1Conn::start(IoContext &ctx) {
  context_ = &ctx;
  listener_ = ctx.accepted.listener;
  Http1::RequestObserver observer{
      this, [](void *self, const Http1::RequestHead &head) noexcept {
        static_cast<Http1Conn *>(self)->select_route(head);
      }};
  observer.on_trailers = [](void *self, uint32_t forwarded, uint32_t dropped,
                            bool rejected) noexcept {
    auto &metrics = static_cast<Http1Conn *>(self)->context_->metrics;
    if (forwarded != 0) {
      inc(metrics.trailers_forwarded, forwarded);
    }
    if (dropped != 0) {
      inc(metrics.trailers_dropped, dropped);
    }
    if (rejected) {
      inc(metrics.trailers_rejected);
    }
  };
  if (ctx.access_log) {
    observer.on_begin = [](void *self) noexcept {
      static_cast<Http1Conn *>(self)->begin_request();
    };
    observer.on_parsed = [](void *self, const Http1::RequestHead &head) noexcept {
      static_cast<Http1Conn *>(self)->capture_request_head(head);
    };
    observer.on_complete =
        [](void *self, const Http1::RequestOutcome &outcome) noexcept {
          static_cast<Http1Conn *>(self)->log_request(outcome);
        };
  }
  s_.emplace(*ctx.exchanges, *ctx.stage, *ctx.limits, ctx.buffers.buf_size(),
             SideRef{core.self(), 0}, observer);
  sync(ctx);
}

void Http1Conn::begin_request() noexcept {
  request_started_ = Clock::now();
  const auto wall = std::chrono::duration_cast<Millis>(
      std::chrono::system_clock::now().time_since_epoch());
  access_record_ = {};
  access_record_.unix_ms =
      static_cast<uint64_t>(std::max(wall, Millis{0}).count());
  access_record_.listener = listener_.value;
}

void Http1Conn::capture_request_head(const Http1::RequestHead &head) noexcept {
  const auto method = head.method.substr(0, access_record_.method.size());
  const auto target = head.target.substr(0, access_record_.target.size());
  std::ranges::copy(method, access_record_.method.begin());
  std::ranges::copy(target, access_record_.target.begin());
  access_record_.method_len = static_cast<uint8_t>(method.size());
  access_record_.target_len = static_cast<uint8_t>(target.size());
}

void Http1Conn::log_request(const Http1::RequestOutcome &outcome) noexcept {
  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      Clock::now() - request_started_);
  access_record_.duration_us = static_cast<uint32_t>(
      std::min<int64_t>(elapsed.count(), std::numeric_limits<uint32_t>::max()));
  access_record_.bytes_in = outcome.bytes_in;
  access_record_.bytes_out = outcome.bytes_out;
  access_record_.status = outcome.status;
  if (!context_->access_log->push(access_record_)) {
    inc(context_->metrics.access_log_dropped);
  }
}

void Http1Conn::select_route(const Http1::RequestHead &head) noexcept {
  adapter_.emplace<std::monostate>();
  response_text_.reset();
  location_ = {};
  cursor_ = {};
  response_kind_ = ResponseKind::Synthetic;
  response_status_ = 501;

  const auto *snapshot = context_->snapshot;
  const auto *listener = snapshot ? snapshot->find(listener_) : nullptr;
  if (!listener || listener->route.empty()) {
    return;
  }

  const auto match = snapshot->match(listener_, head.host, head.target,
                                     context_->route_scratch);
  if (!match.action) {
    response_status_ = 404;
  } else if (const auto *respond = std::get_if<RespondRoute>(match.action)) {
    response_kind_ = ResponseKind::Respond;
    response_status_ = respond->status;
    response_text_ = respond->text;
  } else if (const auto *redirect = std::get_if<RedirectRoute>(match.action)) {
    response_kind_ = ResponseKind::Redirect;
    response_status_ = redirect->status;
    response_text_ = redirect->text;
    location_ = {"Location", redirect->target()};
  } else {
    auto &ctx = *context_;
    auto &ex = *ctx.exchanges->get(s_->exchange());
    response_status_ = 0;
    if (!watching_disconnect_) {
      watching_disconnect_ = core.counted(
          ctx.ring.poll_disconnect(core.fd(), core.tag(OpKind::Disconnect)));
      if (!watching_disconnect_) {
        core.close(ctx, Errc::Overloaded);
        return;
      }
    }
    if (const auto *file = std::get_if<StaticRoute>(match.action)) {
      adapter_.emplace<FileResponder>(ctx).start(ctx, core, ex, head, *file);
    } else if (ctx.upstreams && ctx.chunks) {
      adapter_
          .emplace<ProxyResponder>(
              ctx, std::get<ProxyRoute>(*match.action).upstream_id)
          .start(ctx, core, ex, head);
    } else {
      (void)ex.respond_synthetic(503);
    }
  }
}

void Http1Conn::apply_response(Exchange &ex) noexcept {
  if (response_kind_ == ResponseKind::Synthetic) {
    (void)ex.respond_synthetic(response_status_);
    response_status_ = 0;
    return;
  }

  const auto result = response_kind_ == ResponseKind::Redirect
      ? Redirect::apply(ex, cursor_, response_status_, location_)
      : Respond::apply(ex, cursor_, response_status_,
                       response_text_ ? std::string_view(*response_text_)
                                      : std::string_view{});
  if (!result) {
    if (ex.head_published()) {
      ex.abort(StreamId::Res, result.error().code);
    } else {
      (void)ex.respond_synthetic(500);
    }
  }
  if (!result || *result == ApplyStatus::Done) {
    response_status_ = 0;
    if (response_kind_ == ResponseKind::Respond) {
      response_text_.reset();
    }
  }
}

void Http1Conn::arm_deadline(IoContext &ctx) {
  const auto &d = s_->deadline();
  if (d.epoch == armed_epoch_) {
    return;
  }
  armed_epoch_ = d.epoch;
  if (d.kind == SessionDeadline::None) {
    core.cancel_timer();
  } else {
    core.arm_timer(ctx, to_kind(d.kind), d.after);
  }
}

void Http1Conn::sync(IoContext &ctx) {
  if (core.closing()) {
    return;
  }
  s_->refresh_limits(*ctx.limits);

  const Handle fresh = s_->take_new_exchange();
  if (!s_->exchange().valid()) {
    response_text_.reset();
    response_status_ = 0;
    if (!s_->closing()) {
      adapter_.emplace<std::monostate>();
    }
  } else if (auto *file = std::get_if<FileResponder>(&adapter_)) {
    file->pump(ctx, core, *ctx.exchanges->get(s_->exchange()));
  } else if (auto *proxy = std::get_if<ProxyResponder>(&adapter_)) {
    proxy->pump(ctx, core, *ctx.exchanges->get(s_->exchange()));
    // A final response can overtake an upload. Let the downstream finish,
    // then drain the outstanding peer CQE before reusing its send storage.
    if (proxy->retiring()) {
      s_->on_drain();
    }
  } else if (!send_inflight_ && response_status_ != 0) {
    if (auto *ex = ctx.exchanges->get(s_->exchange())) {
      // Resume the local producer when downstream consumption returns credit.
      if (fresh.valid() ||
          (ex->take_events(Side::Responder) & ExEvent::kResWritable) != 0) {
        apply_response(*ex);
      }
    }
  }

  if (core.closing()) {
    return;
  }
  if (!send_inflight_) {
    const auto out = s_->next_send();
    if (!out.empty()) {
      if (!core.counted(ctx.ring.send(core.fd(), out.data(),
                                      static_cast<uint32_t>(out.size()),
                                      core.tag(OpKind::Send)))) {
        core.close(ctx, Errc::Overloaded); // SQ exhausted: shed
        return;
      }
      send_inflight_ = true;
    }
  }

  if (s_->should_close()) {
    core.close(ctx, s_->close_reason());
    return;
  }

  if (!recv_inflight_ && s_->prepare_recv() > 0) {
    if (!core.counted(ctx.ring.recv_provided(core.fd(), ctx.buffers.bgid(),
                                             core.tag(OpKind::Recv)))) {
      core.close(ctx, Errc::Overloaded);
      return;
    }
    recv_inflight_ = true;
  }

  arm_deadline(ctx);
}

void Http1Conn::on_cqe(IoContext &ctx, const Cqe &cqe,
                       IoBufferRing::Lease &lease) {
  const auto kind = cqe.tag.kind();
  if (kind == OpKind::FileOpen || kind == OpKind::FileStat ||
      kind == OpKind::Read || kind == OpKind::Connect ||
      kind == OpKind::LinkTimeout || cqe.tag.aux() == kAuxPeer) {
    auto *ex = ctx.exchanges->get(s_->exchange());
    if (ex) {
      if (auto *file = std::get_if<FileResponder>(&adapter_)) {
        file->complete(ctx, core, *ex, cqe);
      } else {
        std::get<ProxyResponder>(adapter_).complete(ctx, core, *ex, cqe);
      }
    } else if (kind == OpKind::FileOpen && cqe.res >= 0) {
      ::close(cqe.res);
    }
    sync(ctx);
    return;
  }
  switch (cqe.tag.kind()) {
  case OpKind::Disconnect:
    core.close(ctx, Errc::Reset);
    return;
  case OpKind::Recv: {
    recv_inflight_ = false;
    if (cqe.res == -ENOBUFS) {
      inc(ctx.metrics.nobufs);
      core.close(ctx, Errc::NoBuffers);
      return;
    }
    if (cqe.res == 0) {
      s_->on_eof(Errc::Eof);
    } else if (cqe.res < 0 || !lease.valid()) {
      s_->on_eof(Errc::Reset);
    } else {
      inc(ctx.metrics.bytes_in, static_cast<uint64_t>(cqe.res));
      const auto b = lease.bytes();
      s_->on_data({reinterpret_cast<const char *>(b.data()), b.size()});
    }
    // The lease recycles when the worker's frame unwinds (I10): nothing parked.
    sync(ctx);
    return;
  }
  case OpKind::Send: {
    send_inflight_ = false;
    if (cqe.res <= 0) {
      core.close(ctx, Errc::Reset);
      return;
    }
    inc(ctx.metrics.bytes_out, static_cast<uint64_t>(cqe.res));
    s_->on_sent(static_cast<uint32_t>(cqe.res));
    sync(ctx);
    return;
  }
  default:
    return;
  }
}

void Http1Conn::on_timer(IoContext &ctx, DeadlineKind kind) {
  if (kind == DeadlineKind::UpstreamRead ||
      kind == DeadlineKind::UpstreamWrite) {
    auto *ex = ctx.exchanges->get(s_->exchange());
    if (auto *proxy = std::get_if<ProxyResponder>(&adapter_); proxy && ex) {
      proxy->on_timeout(ctx, core, *ex);
      sync(ctx);
    }
    return;
  }
  const SessionDeadline k = from_kind(kind);
  if (k == SessionDeadline::None) {
    return;
  }
  s_->on_deadline(k);
  sync(ctx);
}

void Http1Conn::on_drain(IoContext &ctx) {
  s_->on_drain();
  sync(ctx);
}

void Http1Conn::on_exchange(IoContext &ctx) {
  if (core.closing()) {
    // Nothing may still read the exchange's buffers once no CQE is pending.
    if (core.inflight() == 0) {
      s_->release();
    }
    return;
  }
  s_->on_exchange_events();
  sync(ctx);
}

} // namespace Coring5
