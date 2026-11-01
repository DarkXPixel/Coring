#include "coring5/proto/l4/EchoConnection.hpp"

#include <cerrno>

namespace Coring5 {

void EchoConnection::start(IoContext &ctx) {
  arm_timer(ctx, DeadlineKind::Idle, ctx.limits->idle_timeout);
  arm_recv(ctx);
}

void EchoConnection::on_drain(IoContext &ctx) {
  if (!pending_.valid()) {
    close(ctx, Errc::Shutdown);
  }
  // else: the Send completion closes the connection (state() == Draining).
}

void EchoConnection::arm_recv(IoContext &ctx) {
  if (!counted(ctx.ring.recv_provided(fd(), ctx.buffers.bgid(),
                                      tag(OpKind::Recv)))) {
    close(ctx, Errc::Overloaded); // SQ exhausted: shed this connection
  }
}

void EchoConnection::send_rest(IoContext &ctx) {
  const auto bytes = pending_.bytes();
  const bool queued = counted(ctx.ring.send(
      fd(), bytes.data() + sent_, static_cast<uint32_t>(bytes.size() - sent_),
      tag(OpKind::Send)));
  if (!queued) {
    close(ctx, Errc::Overloaded);
  }
}

void EchoConnection::on_cqe(IoContext &ctx, const Cqe &cqe,
                            BufferRing::Lease &lease) {
  switch (cqe.tag.kind()) {
  case OpKind::Recv: {
    if (cqe.res == -ENOBUFS) {
      inc(ctx.metrics.nobufs);
      close(ctx, Errc::NoBuffers);
      return;
    }
    if (cqe.res == 0) {
      close(ctx, Errc::Eof);
      return;
    }
    if (cqe.res < 0 || !lease.valid()) {
      close(ctx, Errc::Reset);
      return;
    }
    inc(ctx.metrics.bytes_in, static_cast<uint64_t>(cqe.res));
    arm_timer(ctx, DeadlineKind::Idle, ctx.limits->idle_timeout);
    pending_ = std::move(lease); // keep the buffer alive until fully sent
    sent_ = 0;
    send_rest(ctx);
    return;
  }
  case OpKind::Send: {
    if (cqe.res <= 0) {
      close(ctx, Errc::Reset);
      return;
    }
    sent_ += static_cast<uint32_t>(cqe.res);
    inc(ctx.metrics.bytes_out, static_cast<uint64_t>(cqe.res));
    if (sent_ < pending_.size()) {
      send_rest(ctx); // short write
      return;
    }
    pending_.release();
    if (state() == State::Draining) {
      close(ctx, Errc::Shutdown);
      return;
    }
    arm_recv(ctx);
    return;
  }
  default:
    return;
  }
}

void EchoConnection::on_timer(IoContext &ctx, DeadlineKind kind) {
  if (kind == DeadlineKind::Idle) {
    close(ctx, Errc::Timeout);
  }
}

} // namespace Coring5
