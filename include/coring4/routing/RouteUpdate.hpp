#pragma once

#include "coring4/routing/RouteTable.hpp"
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <string>

namespace Coring4 {

// Completion tracker for one table rollout. Every worker reports exactly once
// (success or failure); the control plane may wait for the rollout to finish.
class RouteUpdateAck {
public:
  explicit RouteUpdateAck(uint32_t workers) : pending_(workers) {}

  // Called from worker threads.
  void complete(const std::expected<void, std::string> &result) {
    {
      std::lock_guard lock(mtx_);
      if (!result) {
        ++failed_;
        if (first_error_.empty()) {
          first_error_ = result.error();
        }
      }
      if (pending_ > 0) {
        --pending_;
      }
    }
    cv_.notify_all();
  }

  // Returns true if every worker finished within `timeout` (successfully or
  // not). Inspect failed() / first_error() afterwards.
  template <class Rep, class Period>
  bool wait_for(std::chrono::duration<Rep, Period> timeout) {
    std::unique_lock lock(mtx_);
    return cv_.wait_for(lock, timeout, [this] { return pending_ == 0; });
  }

  [[nodiscard]] uint32_t pending() const {
    std::lock_guard lock(mtx_);
    return pending_;
  }
  [[nodiscard]] uint32_t failed() const {
    std::lock_guard lock(mtx_);
    return failed_;
  }
  [[nodiscard]] std::string first_error() const {
    std::lock_guard lock(mtx_);
    return first_error_;
  }

private:
  mutable std::mutex mtx_;
  std::condition_variable cv_;
  uint32_t pending_;
  uint32_t failed_{0};
  std::string first_error_;
};

} // namespace Coring4
