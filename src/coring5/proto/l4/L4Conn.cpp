#include "coring5/proto/l4/L4Conn.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>

namespace Coring5 {

namespace {

// Interrupted/transient results: retry the operation, do not fail the stream.
bool is_transient(int err) noexcept { return err == EINTR || err == EAGAIN; }

Millis until(const IoContext &ctx, TimePoint t) noexcept {
  auto d = std::chrono::ceil<Millis>(t - ctx.now);
  return d < Millis{1} ? Millis{1} : d;
}

Millis tick_ms(const IoContext &ctx) noexcept {
  auto d = std::chrono::duration_cast<Millis>(ctx.timers.tick());
  return d < Millis{1} ? Millis{1} : d;
}

} // namespace

L4Conn::~L4Conn() {
  // Runs only after inflight == 0 (or at worker teardown, when the pools
  // outlive their connections): nothing can still read the queued chunks.
  backend_.reset();
  if (chunks_ != nullptr) {
    dir_[0].q.clear(*chunks_);
    dir_[1].q.clear(*chunks_);
  }
}

// ------------------------------------------------------------------ connect

void L4Conn::start(IoContext &ctx) {
  chunks_ = ctx.chunks;
  last_activity_ = ctx.now;

  if (ctx.upstreams == nullptr || chunks_ == nullptr ||
      chunks_->capacity() == 0) {
    core.close(ctx, Errc::Overloaded); // no relay memory sized for this worker
    return;
  }
  const DialStatus st =
      dialer_.start(ctx, core, *ctx.upstreams, ctx.accepted.upstream);
  // Backstop only: the LINK_TIMEOUT of each attempt is the real deadline.
  core.arm_timer(ctx, DeadlineKind::UpstreamConnect,
                 dialer_.budget() + tick_ms(ctx) * 2);
  on_dial(ctx, st);
}

void L4Conn::on_dial(IoContext &ctx, DialStatus st) {
  switch (st) {
  case DialStatus::Pending:
    return;
  case DialStatus::Failed:
    core.close(ctx, dialer_.failure());
    return;
  case DialStatus::Connected: {
    // Keep the socket as the peer and the lease for the life of the relay.
    DialedUpstream up = dialer_.take(core);
    core.set_peer(std::move(up.fd));
    backend_ = std::move(up.lease);
    start_relay(ctx);
    return;
  }
  }
}

void L4Conn::start_relay(IoContext &ctx) {
  phase_ = Phase::Relaying;
  last_activity_ = ctx.now;
  for (unsigned d = 0; d < 2; ++d) {
    dir_[d].progress_at = ctx.now;
  }
  rearm_timer(ctx);
  arm_recv(ctx, 0);
  arm_recv(ctx, 1);
}

// -------------------------------------------------------------------- relay

void L4Conn::arm_recv(IoContext &ctx, unsigned d) {
  Dir &D = dir_[d];
  if (core.closing() || core.state() == ConnCore::State::Draining ||
      D.recv_inflight || D.src_eof || D.paused || D.retry_recv) {
    return;
  }
  if (!core.counted(ctx.ring.recv_provided(
          src_fd(d), ctx.buffers.bgid(),
          core.tag(OpKind::Recv, static_cast<uint8_t>(d))))) {
    core.close(ctx, Errc::Overloaded);
    return;
  }
  D.recv_inflight = true;
}

void L4Conn::kick_send(IoContext &ctx, unsigned d) {
  Dir &D = dir_[d];
  if (core.closing() || D.send_inflight || D.q.empty()) {
    return;
  }
  const auto head = D.q.front(*chunks_);
  if (!core.counted(ctx.ring.send(
          dst_fd(d), head.data(), static_cast<uint32_t>(head.size()),
          core.tag(OpKind::Send, static_cast<uint8_t>(d))))) {
    core.close(ctx, Errc::Overloaded);
    return;
  }
  D.send_inflight = true;
}

void L4Conn::on_recv(IoContext &ctx, const Cqe &cqe, BufferRing::Lease &lease,
                     unsigned d) {
  Dir &D = dir_[d];
  D.recv_inflight = false;

  if (cqe.res == -ENOBUFS) {
    // The shared buffer ring is empty right now: transient. Try again on the
    // next tick instead of failing the connection.
    inc(ctx.metrics.nobufs);
    D.retry_recv = true;
    rearm_timer(ctx);
    return;
  }
  if (cqe.res < 0) {
    if (is_transient(-cqe.res)) {
      arm_recv(ctx, d);
      return;
    }
    core.close(ctx, cqe.res == -ECANCELED ? Errc::Canceled : Errc::Reset);
    return;
  }
  if (cqe.res == 0) {
    // Orderly EOF: flush what is queued, then half-close the other socket.
    D.src_eof = true;
    maybe_shutdown_sink(ctx, d);
    check_drain(ctx);
    return;
  }
  if (!lease.valid()) { // data without a buffer: kernel/ring bug, never relay
    core.close(ctx, Errc::Internal);
    return;
  }

  inc(ctx.metrics.bytes_in, static_cast<uint64_t>(cqe.res));
  last_activity_ = ctx.now;
  const bool was_empty = D.q.empty();
  // I10: copy out and give the buffer back immediately.
  const bool queued = D.q.push(*chunks_, lease.bytes());
  lease.release();
  if (!queued) {
    inc(ctx.metrics.relay_shed);
    core.close(ctx, Errc::Overloaded);
    return;
  }
  if (was_empty) {
    D.progress_at = ctx.now;
  }
  kick_send(ctx, d);
  if (core.closing()) {
    return;
  }
  if (core.state() == ConnCore::State::Draining) {
    check_drain(ctx);
    return;
  }
  // Backpressure (I6): stop reading this source until its queue drains.
  if (D.q.bytes() >= ctx.limits->queue_high_watermark || chunks_->low()) {
    D.paused = true;
    inc(ctx.metrics.relay_pauses);
  } else {
    arm_recv(ctx, d);
  }
  rearm_timer(ctx);
}

void L4Conn::on_send(IoContext &ctx, const Cqe &cqe, unsigned d) {
  Dir &D = dir_[d];
  D.send_inflight = false;

  if (cqe.res < 0) {
    if (is_transient(-cqe.res)) {
      kick_send(ctx, d); // nothing was written; resend the same head
      return;
    }
    // EPIPE / ECONNRESET / ETIMEDOUT / ...: the sink is gone.
    core.close(ctx, cqe.res == -ECANCELED ? Errc::Canceled : Errc::Reset);
    return;
  }
  if (cqe.res == 0) { // a zero-byte send of a non-empty span: treat as dead
    core.close(ctx, Errc::Reset);
    return;
  }
  // Short sends are normal: consume only what was written and send the rest.
  const auto n = static_cast<uint32_t>(cqe.res);
  D.q.consume(*chunks_, std::min(n, D.q.bytes()));
  inc(ctx.metrics.bytes_out, static_cast<uint64_t>(n));
  D.progress_at = ctx.now;
  last_activity_ = ctx.now;

  kick_send(ctx, d);
  maybe_shutdown_sink(ctx, d);
  maybe_resume(ctx, d);
  check_drain(ctx);
  if (!core.closing()) {
    rearm_timer(ctx);
  }
}

void L4Conn::maybe_resume(IoContext &ctx, unsigned d) {
  Dir &D = dir_[d];
  if (!D.paused || core.closing() || core.state() != ConnCore::State::Active) {
    return;
  }
  if (D.q.bytes() <= ctx.limits->queue_low_watermark &&
      (D.q.empty() || !chunks_->low())) {
    D.paused = false;
    arm_recv(ctx, d);
  }
}

void L4Conn::maybe_shutdown_sink(IoContext &ctx, unsigned d) {
  Dir &D = dir_[d];
  if (core.closing() || !D.src_eof || !D.q.empty() || D.send_inflight ||
      D.shut_queued) {
    return;
  }
  D.shut_queued = true;
  if (core.counted(ctx.ring.shutdown(
          dst_fd(d), SHUT_WR,
          core.tag(OpKind::Shutdown, static_cast<uint8_t>(d))))) {
    return;
  }
  // SQ full: the syscall is just as good for a half-close, so do not shed.
  (void)::shutdown(dst_fd(d), SHUT_WR);
  D.done = true;
  check_both_done(ctx);
}

void L4Conn::on_shutdown(IoContext &ctx, unsigned d) {
  // Any result is final: 0, or -ENOTCONN when the sink already went away
  // (which is exactly the state half-close wanted to reach).
  dir_[d].done = true;
  check_both_done(ctx);
}

void L4Conn::check_both_done(IoContext &ctx) {
  if (dir_[0].done && dir_[1].done) {
    core.close(ctx, Errc::Eof);
  }
}

void L4Conn::check_drain(IoContext &ctx) {
  if (core.state() != ConnCore::State::Draining) {
    return;
  }
  // Graceful drain: bytes already accepted from a peer are still delivered.
  for (const Dir &D : dir_) {
    if (!D.q.empty() || D.send_inflight) {
      return;
    }
  }
  core.close(ctx, Errc::Shutdown);
}

TimePoint L4Conn::deadline(const IoContext &ctx) const noexcept {
  const Millis idle = ctx.limits->idle_timeout;
  TimePoint dl = last_activity_ + idle;
  for (const Dir &D : dir_) {
    // A direction with queued bytes whose sink makes no progress is a write
    // stall even while the other direction keeps the connection "active".
    if (!D.q.empty()) {
      dl = std::min(dl, D.progress_at + idle);
    }
  }
  return dl;
}

void L4Conn::rearm_timer(IoContext &ctx) {
  TimePoint dl = deadline(ctx);
  if (dir_[0].retry_recv || dir_[1].retry_recv) {
    dl = std::min(dl, ctx.now + tick_ms(ctx));
  }
  core.arm_timer(ctx, DeadlineKind::Idle, until(ctx, dl));
}

// -------------------------------------------------------------- CQE / timer

void L4Conn::on_cqe(IoContext &ctx, const Cqe &cqe, BufferRing::Lease &lease) {
  const unsigned d = cqe.tag.aux() & 1U;
  switch (cqe.tag.kind()) {
  case OpKind::Connect:
  case OpKind::LinkTimeout:
    if (phase_ == Phase::Connecting) {
      on_dial(ctx, dialer_.on_cqe(ctx, core, cqe));
    }
    return;
  case OpKind::Recv:
    on_recv(ctx, cqe, lease, d);
    return;
  case OpKind::Send:
    on_send(ctx, cqe, d);
    return;
  case OpKind::Shutdown:
    on_shutdown(ctx, d);
    return;
  default:
    return;
  }
}

void L4Conn::on_timer(IoContext &ctx, DeadlineKind kind) {
  if (phase_ == Phase::Connecting) {
    if (kind == DeadlineKind::UpstreamConnect) {
      core.close(ctx, Errc::Timeout); // LINK_TIMEOUT should have fired first
    }
    return;
  }
  // Re-arm recvs that hit -ENOBUFS (the tick is their back-off).
  for (unsigned d = 0; d < 2; ++d) {
    if (dir_[d].retry_recv) {
      dir_[d].retry_recv = false;
      arm_recv(ctx, d);
    }
  }
  if (core.closing()) {
    return;
  }
  if (ctx.now >= deadline(ctx)) {
    core.close(ctx, Errc::Timeout);
    return;
  }
  rearm_timer(ctx);
}

void L4Conn::on_drain(IoContext &ctx) {
  if (phase_ == Phase::Connecting) {
    core.close(ctx, Errc::Shutdown); // no byte relayed yet: nothing to flush
    return;
  }
  check_drain(ctx); // closes now if idle, else the last send completion does
}

} // namespace Coring5
