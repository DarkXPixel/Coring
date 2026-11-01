#pragma once

// Connection lifecycle base class. Implements invariants I2 and I3
// (docs/INVARIANTS.md); protocol code derives from it and only implements the
// on_* hooks.
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
// Rules for derived classes:
//   * wrap EVERY ring submission in counted(...);
//   * never submit new work when closing() is true;
//   * never `delete this` / destroy the slot: the Worker reaps Closed
//     connections after the callback returns;
//   * store Handles (not raw pointers) to other connections.

#include "coring5/io/FdOwner.hpp"
#include "coring5/io/IoContext.hpp"
#include <cstdint>

namespace Coring5 {

// Fixed size of a connection slot in the worker's HandlePool. A derived class
// that does not fit fails a static_assert in HandlePool::create().
inline constexpr std::size_t kConnectionSlotBytes = 512;

class Connection {
public:
  enum class State : uint8_t { Active, Draining, Closing, Closed };

  explicit Connection(FdOwner fd) noexcept : fd_(std::move(fd)) {}
  Connection(const Connection &) = delete;
  Connection &operator=(const Connection &) = delete;
  virtual ~Connection() = default;

  // Called by the Worker right after creation.
  void bind(Handle self) noexcept { self_ = self; }
  virtual void start(IoContext &ctx) = 0;

  // ---- called by the Worker only ----
  void handle_cqe(IoContext &ctx, const Cqe &cqe, BufferRing::Lease &lease);
  void handle_timer(IoContext &ctx, DeadlineKind kind);
  void begin_drain(IoContext &ctx);

  [[nodiscard]] State state() const noexcept { return state_; }
  [[nodiscard]] bool closing() const noexcept {
    return state_ >= State::Closing;
  }
  [[nodiscard]] uint16_t inflight() const noexcept { return inflight_; }
  [[nodiscard]] Handle handle() const noexcept { return self_; }
  [[nodiscard]] int fd() const noexcept { return fd_.get(); }

  // Idempotent. Starts the cancel/close sequence; completes asynchronously.
  void close(IoContext &ctx, Errc why);

protected:
  // CQE for a non-cancel operation of an Active/Draining connection. `lease`
  // owns the provided buffer, if any; move from it to keep the buffer, else it
  // is recycled when the callback returns.
  virtual void on_cqe(IoContext &ctx, const Cqe &cqe,
                      BufferRing::Lease &lease) = 0;
  virtual void on_timer(IoContext &ctx, DeadlineKind kind) = 0;
  // Graceful drain request (reload/shutdown). Default: close right away.
  virtual void on_drain(IoContext &ctx) { close(ctx, Errc::Shutdown); }

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
    timer_.owner = self_;
    ctx.timers.schedule(timer_, ctx.now + after);
  }
  void cancel_timer() noexcept { timer_.cancel(); }

private:
  void finish(IoContext &ctx);

  FdOwner fd_;
  TimerNode timer_;
  Handle self_{};
  State state_{State::Active};
  uint16_t inflight_{0};
};

} // namespace Coring5
