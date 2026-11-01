#pragma once

// Completion barrier for control-plane -> workers requests (config reload,
// listener changes). Workers call complete(); the control thread may wait.
//
// Retirement rule (invariant I8): the control thread drops the previous
// snapshot only after done() is true, so the LAST reference is destroyed on the
// control thread and never on a worker's hot path.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace Coring5 {

class ReloadAck {
public:
  explicit ReloadAck(uint32_t expected) noexcept : remaining_(expected) {}

  // Called once per worker. Release semantics: everything the worker did
  // (including dropping its reference to the old snapshot) happens-before the
  // waiter observing done().
  void complete(bool ok) noexcept {
    if (!ok) {
      failed_.fetch_add(1, std::memory_order_relaxed);
    }
    remaining_.fetch_sub(1, std::memory_order_acq_rel);
  }

  [[nodiscard]] bool done() const noexcept {
    return remaining_.load(std::memory_order_acquire) == 0;
  }

  [[nodiscard]] uint32_t failures() const noexcept {
    return failed_.load(std::memory_order_relaxed);
  }

  // Polls (control path only, never call from a worker).
  [[nodiscard]] bool wait_for(std::chrono::milliseconds timeout) const {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
      if (std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
  }

private:
  std::atomic<uint32_t> remaining_;
  std::atomic<uint32_t> failed_{0};
};

} // namespace Coring5
