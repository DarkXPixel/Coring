#pragma once

// Process-level orchestration: config file -> workers -> control socket.
// This is the only place that knows about all modules (apps/coringd uses it).

#include "coring5/runtime/ConfigManager.hpp"
#include "coring5/control/ControlServer.hpp"
#include "coring5/obs/AccessLogWriter.hpp"
#include "coring5/runtime/WorkerGroup.hpp"
#include <atomic>
#include <expected>
#include <memory>
#include <optional>
#include <string>

namespace Coring5 {

struct DaemonOptions {
  std::string config_path;
  std::string control_socket{"/run/coring/control.sock"};
  bool check_only{false}; // --check-config: parse+validate then exit
};

class Daemon {
public:
  explicit Daemon(DaemonOptions opts) : opts_(std::move(opts)) {}

  [[nodiscard]] std::expected<void, std::string> start();
  // SIGHUP / `coringctl reload`.
  [[nodiscard]] std::string reload();
  [[nodiscard]] std::string stats() const;
  [[nodiscard]] std::string status() const;
  void request_stop() noexcept { stop_requested_.store(true); }
  [[nodiscard]] bool stop_requested() const noexcept {
    return stop_requested_.load();
  }
  [[nodiscard]] Result<> stop();

private:
  DaemonOptions opts_;
  std::atomic<bool> stop_requested_{false};
  // Workers must quiesce before the writer stops, and both borrow the rings.
  std::unique_ptr<AccessLogRing[]> access_logs_;
  std::optional<AccessLogWriter> access_writer_;
  WorkerGroup workers_;
  std::unique_ptr<ConfigManager> config_;
  std::unique_ptr<ControlServer> control_;
  Millis drain_timeout_{10000};
};

} // namespace Coring5
