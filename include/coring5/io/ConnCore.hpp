#pragma once

// Connection lifecycle core. Implements invariants I2 and I3
// (docs/INVARIANTS.md). It is a plain, NON-polymorphic class: every connection
// type (EchoConn, L4Conn, Http1Conn, ...) embeds one as the public member
// `core` and the worker reaches the type through a typed pool
// (io/ConnPool.hpp), so there are no vtables, no virtual calls and no
// per-connection vptr.
//
//   Active --begin_drain--> Draining --+
//     |                                 |
//     +-- close(why) ------------------+--> Closing --(inflight==0)--> Closed
//                                            (cancel_fd issued,
//                                             CQEs still arriving)
//
//  I2  every submitted SQE is counted at prep time through counted(); a CQE
//      without IORING_CQE_F_MORE decrements. This covers multishot ops,
//      SEND_ZC (second F_NOTIF CQE), linked timeouts and cancel SQEs.
//  I3  the fd is closed, and the slot becomes reapable, only when
//      state == Closing && inflight == 0.
//
// A connection may own a second socket (the `peer`, e.g. the upstream of an l4
// relay). close() cancels the primary fd and the peer fd SEPARATELY: each
// cancel is its own counted SQE with its own CQE, and whichever cancel cannot
// be queued falls back to shutdown(2) on that fd only. Both fds are closed
// together in finish(), never earlier, so neither number can be reused while a
// cancel for it is pending.
//
// Contract of a connection type `T` (checked by the ConnectionType concept):
//   static constexpr PoolId kPool;          which pool it lives in
//   explicit T(FdOwner);                    constructed in place in its slot
//   ConnCore core;                          public
//   void start(IoContext&);
//   void on_cqe(IoContext&, const Cqe&, IoBufferRing::Lease&);
//   void on_timer(IoContext&, DeadlineKind);
//   void on_drain(IoContext&);              finish current work, then close
//
// Rules for connection types:
//   * wrap EVERY ring submission in core.counted(...);
//   * never submit new work when core.closing() is true;
//   * never destroy the slot: the Worker reaps Closed connections after the
//     callback returns;
//   * store ConnRef/Handle (not raw pointers) to other connections.

#include "coring5/io/FdOwner.hpp"
#include "coring5/io/IoContext.hpp"
#include <concepts>
#include <cstdint>

namespace Coring5 {

class ConnCore {
public:
  enum class State : uint8_t { Active, Draining, Closing, Closed };

  explicit ConnCore(FdOwner fd) noexcept : fd_(std::move(fd)) {}
  ConnCore(const ConnCore &) = delete;
  ConnCore &operator=(const ConnCore &) = delete;
  ~ConnCore() = default;

  // Called by the pool right after construction.
  void bind(ConnRef self) noexcept { self_ = self; }

  // ---- called by ConnPool only ------------------------------------------
  // CQE prologue: accounts the completion (I2). Returns true when the
  // protocol's on_cqe() must run (not a cancel CQE, not closing).
  [[nodiscard]] bool begin_cqe(const Cqe &cqe) noexcept;
  // CQE epilogue: completes the close sequence when nothing is in flight (I3).
  void end_cqe(IoContext &ctx);
  // Active -> Draining. Returns true if the transition happened, i.e. the
  // protocol's on_drain() must run.
  [[nodiscard]] bool begin_drain() noexcept;

  // ---- for the protocol implementation ----------------------------------
  [[nodiscard]] State state() const noexcept { return state_; }
  [[nodiscard]] bool closing() const noexcept {
    return state_ >= State::Closing;
  }
  [[nodiscard]] uint16_t inflight() const noexcept { return inflight_; }
  [[nodiscard]] ConnRef self() const noexcept { return self_; }
  [[nodiscard]] Handle handle() const noexcept { return self_.handle; }
  [[nodiscard]] int fd() const noexcept { return fd_.get(); }

  // ---- optional second socket ------------------------------------------
  [[nodiscard]] bool has_peer() const noexcept { return peer_.valid(); }
  [[nodiscard]] int peer_fd() const noexcept { return peer_.get(); }
  // Takes ownership of the peer socket. Replacing/clearing it is only legal
  // while NO submission on the old peer is in flight (the caller tracks this);
  // otherwise the fd number could be reused under a pending operation (I3).
  void set_peer(FdOwner fd) noexcept { peer_ = std::move(fd); }
  void reset_peer() noexcept { peer_.reset(); }
  // Moves the peer socket out (e.g. a freshly dialed upstream handed to its
  // real owner). Same precondition as set_peer(): nothing in flight on it.
  [[nodiscard]] FdOwner take_peer() noexcept { return std::move(peer_); }

  // Idempotent. Starts the cancel/close sequence; completes asynchronously.
  void close(IoContext &ctx, Errc why);

  // I2: count a submission only if it was actually queued.
  [[nodiscard]] bool counted(bool queued, unsigned n = 1) noexcept {
    if (queued) {
      inflight_ = static_cast<uint16_t>(inflight_ + n);
    }
    return queued;
  }

  [[nodiscard]] OpTag tag(OpKind k, uint8_t aux = 0) const noexcept {
    return OpTag::make(k, self_, aux);
  }

  void arm_timer(IoContext &ctx, DeadlineKind kind, Millis after) noexcept {
    timer_.kind = kind;
    timer_.owner = self_.handle;
    timer_.pool = self_.pool;
    ctx.timers.schedule(timer_, ctx.now + after);
  }
  void cancel_timer() noexcept { timer_.cancel(); }

private:
  void finish(IoContext &ctx);
  // Counted cancel of every request on `fd`; shutdown(2) if the SQ is full.
  void cancel_side(IoContext &ctx, int fd, uint8_t aux);

  FdOwner fd_;
  FdOwner peer_;
  TimerNode timer_;
  ConnRef self_{};
  State state_{State::Active};
  uint16_t inflight_{0};
};

template <class T>
concept ConnectionType = requires(T &c, IoContext &ctx, const Cqe &cqe,
                                  IoBufferRing::Lease &lease, DeadlineKind kind) {
  { T::kPool } -> std::convertible_to<PoolId>;
  { c.core } -> std::same_as<ConnCore &>;
  c.start(ctx);
  c.on_cqe(ctx, cqe, lease);
  c.on_timer(ctx, kind);
  c.on_drain(ctx);
} && std::constructible_from<T, FdOwner>;

} // namespace Coring5
