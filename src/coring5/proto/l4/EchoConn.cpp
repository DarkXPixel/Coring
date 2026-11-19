#include "coring5/proto/l4/EchoConn.hpp"

#include <cerrno>

namespace Coring5 {

void EchoConn::start(IoContext &ctx) {
  core.arm_timer(ctx, DeadlineKind::Idle, ctx.limits->idle_timeout);
  arm_recv(ctx);
}

void EchoConn::on_drain(IoContext &ctx) {
  if (!pending_.valid()) {
    core.close(ctx, Errc::Shutdown);
  }
  // else: the Send completion closes the connection (state == Draining).
}

void EchoConn::arm_recv(IoContext &ctx) {
  if (!core.counted(ctx.ring.recv_provided(core.fd(), ctx.buffers.bgid(),
                                           core.tag(OpKind::Recv)))) {
    core.close(ctx, Errc::Overloaded); // SQ exhausted: shed this connection
  }
}

void EchoConn::send_rest(IoContext &ctx) {
  const auto bytes = pending_.bytes();
  const bool queued = core.counted(ctx.ring.send(
      core.fd(), bytes.data() + sent_,
      static_cast<uint32_t>(bytes.size() - sent_), core.tag(OpKind::Send)));
  if (!queued) {
    core.close(ctx, Errc::Overloaded);
  }
}

void EchoConn::on_cqe(IoContext &ctx, const Cqe &cqe,
                      BufferRing::Lease &lease) {
  switch (cqe.tag.kind()) {
  case OpKind::Recv: {
    if (cqe.res == -ENOBUFS) {
      inc(ctx.metrics.nobufs);
      core.close(ctx, Errc::NoBuffers);
      return;
    }
    if (cqe.res == 0) {
      core.close(ctx, Errc::Eof);
      return;
    }
    if (cqe.res < 0 || !lease.valid()) {
      core.close(ctx, Errc::Reset);
      return;
    }
    inc(ctx.metrics.bytes_in, static_cast<uint64_t>(cqe.res));
    core.arm_timer(ctx, DeadlineKind::Idle, ctx.limits->idle_timeout);
    pending_ = std::move(lease); // keep the buffer alive until fully sent
    sent_ = 0;
    send_rest(ctx);
    return;
  }
  case OpKind::Send: {
    if (cqe.res <= 0) {
      core.close(ctx, Errc::Reset);
      return;
    }
    sent_ += static_cast<uint32_t>(cqe.res);
    inc(ctx.metrics.bytes_out, static_cast<uint64_t>(cqe.res));
    if (sent_ < pending_.size()) {
      send_rest(ctx); // short write
      return;
    }
    pending_.release();
    if (core.state() == ConnCore::State::Draining) {
      core.close(ctx, Errc::Shutdown);
      return;
    }
    arm_recv(ctx);
    return;
  }
  default:
    return;
  }
}

void EchoConn::on_timer(IoContext &ctx, DeadlineKind kind) {
  if (kind == DeadlineKind::Idle) {
    core.close(ctx, Errc::Timeout);
  }
}

} // namespace Coring5
