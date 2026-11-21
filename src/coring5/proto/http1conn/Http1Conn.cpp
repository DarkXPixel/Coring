#include "coring5/proto/http1conn/Http1Conn.hpp"

#include <cerrno>

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
  s_.emplace(*ctx.exchanges, *ctx.stage, *ctx.limits, ctx.buffers.buf_size(),
             SideRef{core.self(), 0});
  sync(ctx);
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

  // Route a freshly accepted request (E2). No handlers yet: synthetic 501.
  if (const Handle h = s_->take_new_exchange(); h.valid()) {
    if (Exchange *ex = ctx.exchanges->get(h)) {
      (void)ex->respond_synthetic(501);
    }
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
  switch (cqe.tag.kind()) {
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
