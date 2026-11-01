#pragma once

// Owns the worker threads, their mailboxes and metric shards.
//
// start() returns only after EVERY worker finished init() (pin, ring, buffers,
// pools). If any worker fails, the others are stopped and the error from the
// failing worker is returned: no half-started process.

#include "coring5/config/Snapshot.hpp"
#include "coring5/obs/Metrics.hpp"
#include "coring5/runtime/Mailbox.hpp"
#include "coring5/runtime/Worker.hpp"
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace Coring5 {

class WorkerGroup {
public:
  WorkerGroup() = default;
  WorkerGroup(const WorkerGroup &) = delete;
  WorkerGroup &operator=(const WorkerGroup &) = delete;
  ~WorkerGroup();

  [[nodiscard]] std::expected<void, std::string> start(const RuntimeConfig &rt);

  // Graceful: stop accepting, drain connections up to `drain_timeout`, join.
  void shutdown(Millis drain_timeout);

  [[nodiscard]] std::size_t size() const noexcept { return mailboxes_.size(); }
  [[nodiscard]] Mailbox &mailbox(std::size_t i) { return *mailboxes_[i]; }
  [[nodiscard]] std::span<const WorkerMetrics> metrics() const noexcept {
    return {metrics_.get(), mailboxes_.size()};
  }
  [[nodiscard]] bool running() const noexcept { return running_; }

private:
  std::vector<std::unique_ptr<Mailbox>> mailboxes_;
  std::unique_ptr<WorkerMetrics[]> metrics_;
  std::vector<std::thread> threads_;
  bool running_{false};
};

} // namespace Coring5
