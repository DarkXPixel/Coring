#include "coring5/io/Connection.hpp"

#include <cassert>
#include <sys/socket.h>

namespace Coring5 {

void Connection::handle_cqe(IoContext &ctx, const Cqe &cqe,
                            BufferRing::Lease &lease) {
  assert(state_ != State::Closed && "CQE delivered to a Closed connection");
  assert(inflight_ > 0 && "CQE without a counted submission (I2 violated)");

  if (!cqe.more()) {
    --inflight_;
  }

  const bool is_cancel = cqe.tag.kind() == OpKind::Cancel;
  if (!is_cancel && !closing()) {
    on_cqe(ctx, cqe, lease);
  }
  // In Closing state the lease (if any) is simply recycled by its destructor.

  if (state_ == State::Closing && inflight_ == 0) {
    finish(ctx);
  }
}

void Connection::handle_timer(IoContext &ctx, DeadlineKind kind) {
  if (!closing()) {
    on_timer(ctx, kind);
  }
}

void Connection::begin_drain(IoContext &ctx) {
  if (state_ == State::Active) {
    state_ = State::Draining;
    on_drain(ctx);
  }
}

void Connection::close(IoContext &ctx, Errc why) {
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
  // The cancel has its own CQE and is counted like any other submission (I2).
  if (!counted(ctx.ring.cancel_fd(fd_.get(), tag(OpKind::Cancel)))) {
    // SQ exhausted: shutdown() makes pending recv/send complete anyway.
    ::shutdown(fd_.get(), SHUT_RDWR);
  }
}

void Connection::finish(IoContext &ctx) {
  fd_.reset(); // I3: only now, when no CQE can reference this slot anymore
  state_ = State::Closed;
  ctx.mark_closed(self_);
}

} // namespace Coring5
