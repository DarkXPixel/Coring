#include "coring5/proto/http2/Http2Conn.hpp"

#include <cerrno>

namespace Coring5 {

void Http2Conn::start(IoContext &ctx) {
  core.arm_timer(ctx, DeadlineKind::Idle, ctx.limits->idle_timeout);
  if (!core.counted(ctx.ring.recv_provided(core.fd(), ctx.buffers.bgid(),
                                           core.tag(OpKind::Recv)))) {
    core.close(ctx, Errc::Overloaded);
  }
}

void Http2Conn::on_cqe(IoContext &ctx, const Cqe &cqe, IoBufferRing::Lease &) {
  if (cqe.tag.kind() != OpKind::Recv) {
    return;
  }
  if (cqe.res == 0) {
    core.close(ctx, Errc::Eof);
  } else if (cqe.res == -ENOBUFS) {
    inc(ctx.metrics.nobufs);
    core.close(ctx, Errc::NoBuffers);
  } else if (cqe.res < 0) {
    core.close(ctx, Errc::Reset);
  } else {
    core.close(ctx, Errc::Unsupported); // TODO(M8): nghttp2 session
  }
}

void Http2Conn::on_timer(IoContext &ctx, DeadlineKind kind) {
  if (kind == DeadlineKind::Idle) {
    core.close(ctx, Errc::Timeout);
  }
}

void Http2Conn::on_drain(IoContext &ctx) { core.close(ctx, Errc::Shutdown); }

} // namespace Coring5
