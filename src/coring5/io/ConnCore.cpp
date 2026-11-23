#include "coring5/io/ConnCore.hpp"

#include <cassert>
#include <sys/socket.h>

namespace Coring5 {

bool ConnCore::begin_cqe(const Cqe &cqe) noexcept {
  assert(state_ != State::Closed && "CQE delivered to a Closed connection");
  assert(inflight_ > 0 && "CQE without a counted submission (I2 violated)");

  if (!cqe.more()) {
    --inflight_;
  }
  if (lookup_ == cqe.tag.kind()) {
    lookup_ = OpKind::Nop;
  }
  // An open can win its race with cancellation. No protocol callback runs in
  // Closing, but the successful CQE has transferred ownership of a real fd.
  if (closing() && cqe.tag.kind() == OpKind::FileOpen && cqe.res >= 0) {
    ::close(cqe.res);
  }
  // In Closing state the lease (if any) is simply recycled by its destructor.
  return cqe.tag.kind() != OpKind::Cancel && !closing();
}

void ConnCore::end_cqe(IoContext &ctx) {
  if (state_ == State::Closing && inflight_ == 0) {
    finish(ctx);
  }
}

bool ConnCore::begin_drain() noexcept {
  if (state_ == State::Active) {
    state_ = State::Draining;
    return true;
  }
  return false;
}

void ConnCore::close(IoContext &ctx, Errc why) {
  if (closing()) {
    return;
  }
  state_ = State::Closing;
  inc(ctx.metrics.closes[static_cast<std::size_t>(why)]);
  cancel_timer();

  if (inflight_ == 0) {
    finish(ctx);
    return;
  }
  // Each cancel has its own CQE and is counted like any other submission (I2).
  if (lookup_ != OpKind::Nop) {
    (void)counted(ctx.ring.cancel_tag(tag(lookup_), tag(OpKind::Cancel)));
  }
  cancel_side(ctx, fd_.get(), kAuxConnection);
  if (peer_.valid()) {
    cancel_side(ctx, peer_.get(), kAuxPeer);
  }
}

void ConnCore::cancel_side(IoContext &ctx, int fd, uint8_t aux) {
  if (!counted(ctx.ring.cancel_fd(fd, tag(OpKind::Cancel, aux)))) {
    // SQ exhausted: shutdown() makes pending recv/send/connect complete anyway.
    ::shutdown(fd, SHUT_RDWR);
  }
}

void ConnCore::finish(IoContext &ctx) {
  // I3: only now, when no CQE can reference this slot anymore.
  peer_.reset();
  fd_.reset();
  state_ = State::Closed;
  ctx.mark_closed(self_);
}

} // namespace Coring5
