#pragma once

// Single-level hashed timer wheel with INTRUSIVE nodes (invariants I7, I9).
//
//  * Nodes live inside their owner (Connection), so scheduling never
//    allocates and a destroyed owner cannot leave a stale entry behind: the
//    node's destructor unlinks it.
//  * Time is injected (advance(now)) so tests are deterministic.
//  * Deadlines further than slots*tick away simply stay in their slot and are
//    skipped (expiry check) until their lap comes around: correct, slightly
//    wasteful, no cascading logic.
//  * Callbacks may schedule or cancel any node, including other nodes that are
//    due in the same tick.

#include "coring5/base/Types.hpp"
#include <cstdint>
#include <vector>

namespace Coring5 {

enum class DeadlineKind : uint8_t {
  Idle,
  HeaderRead,
  BodyIdle,
  UpstreamConnect,
  UpstreamFirstByte,
  WriteStall,
  KeepAlive,
  MaxLifetime,
  Drain,
};

class TimerNode {
public:
  TimerNode() = default;
  TimerNode(const TimerNode &) = delete;
  TimerNode &operator=(const TimerNode &) = delete;
  ~TimerNode() { cancel(); }

  [[nodiscard]] bool scheduled() const noexcept { return next_ != this; }
  void cancel() noexcept {
    if (next_ != this) {
      prev_->next_ = next_;
      next_->prev_ = prev_;
      next_ = prev_ = this;
    }
  }

  Handle owner{};
  DeadlineKind kind{DeadlineKind::Idle};

private:
  friend class TimerWheel;
  TimerNode *prev_{this};
  TimerNode *next_{this};
  uint64_t expiry_tick_{0};
};

class TimerWheel {
public:
  TimerWheel(TimePoint start, Nanos tick, uint32_t slots = 1024);

  void schedule(TimerNode &n, TimePoint deadline) noexcept;
  void cancel(TimerNode &n) noexcept { n.cancel(); }

  // Fires f(TimerNode&) for every node whose deadline has passed.
  template <class F> void advance(TimePoint now, F &&fire) {
    const uint64_t target = tick_of(now);
    if (target <= cur_) {
      return;
    }
    if (target - cur_ > slots_.size()) {
      cur_ = target - slots_.size(); // long stall: visit every slot once
    }
    while (cur_ < target) {
      ++cur_;
      process_slot(slots_[cur_ % slots_.size()], fire);
    }
  }

  [[nodiscard]] uint64_t current_tick() const noexcept { return cur_; }
  [[nodiscard]] Nanos tick() const noexcept { return tick_; }

private:
  [[nodiscard]] uint64_t tick_of(TimePoint t) const noexcept {
    if (t <= start_) {
      return 0;
    }
    return static_cast<uint64_t>((t - start_) / tick_);
  }

  static void link_before(TimerNode &head, TimerNode &n) noexcept {
    n.prev_ = head.prev_;
    n.next_ = &head;
    head.prev_->next_ = &n;
    head.prev_ = &n;
  }

  template <class F> void process_slot(TimerNode &head, F &fire) {
    if (head.next_ == &head) {
      return;
    }
    // Detach the whole bucket so callbacks can freely (re)schedule/cancel.
    TimerNode local;
    local.next_ = head.next_;
    local.prev_ = head.prev_;
    local.next_->prev_ = &local;
    local.prev_->next_ = &local;
    head.next_ = head.prev_ = &head;

    while (local.next_ != &local) {
      TimerNode &n = *local.next_;
      n.cancel(); // unlink from `local`
      if (n.expiry_tick_ <= cur_) {
        fire(n);
      } else {
        link_before(head, n); // not due yet (a later lap)
      }
    }
  }

  TimePoint start_;
  Nanos tick_;
  uint64_t cur_{0};
  std::vector<TimerNode> slots_;
};

} // namespace Coring5
