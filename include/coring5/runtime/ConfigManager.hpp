#pragma once

// Control-thread side of hot reload (docs/CONFIG_AND_RELOAD.md).
//
//   reload(parsed):
//     1. validate                                   -- nothing touched on error
//     2. reject runtime/access-log changes           -- "restart required"
//     3. assign ListenerIds (same name+address => same id)
//     4. create per-worker SO_REUSEPORT sockets for NEW listeners (can fail)
//     5. build the immutable snapshot
//     6. post ApplyConfig to every worker           -- infallible on the worker
//     7. wait for all acks (timeout => report, do NOT roll back silently)
//     8. retire the old snapshot HERE (last reference dies on this thread)
//
// Thread-safety: reload()/current() may be called from any control thread;
// they serialise on an internal mutex.

#include "coring5/config/Diff.hpp"
#include "coring5/config/Loader.hpp"
#include "coring5/config/Snapshot.hpp"
#include "coring5/runtime/WorkerGroup.hpp"
#include <expected>
#include <memory>
#include <mutex>
#include <set>
#include <string>

namespace Coring5 {

struct ReloadReport {
  uint64_t generation{0};
  ConfigDiff diff;
  bool workers_acked{false};
};

class ConfigManager {
public:
  explicit ConfigManager(WorkerGroup &workers) : workers_(workers) {}

  // First configuration: same pipeline as reload().
  [[nodiscard]] std::expected<ReloadReport, std::string>
  apply(ParsedConfig parsed);

  [[nodiscard]] std::shared_ptr<const ConfigSnapshot> current() const;
  [[nodiscard]] const RuntimeConfig &runtime() const noexcept {
    return runtime_;
  }

private:
  void retire_route_text(const std::shared_ptr<const ConfigSnapshot> &old);

  WorkerGroup &workers_;
  mutable std::mutex mtx_;
  std::shared_ptr<const ConfigSnapshot> current_;
  // Keep published root fds pinned here through worker acknowledgements.
  std::shared_ptr<const StaticRoots> current_roots_;
  // Keep the control thread responsible for freeing previous response bytes.
  std::set<std::shared_ptr<const std::string>, std::owner_less<>> retired_text_;
  RuntimeConfig runtime_;
  AccessLogConfig access_log_;
  bool have_runtime_{false};
  uint32_t next_listener_id_{1};
  uint32_t next_upstream_id_{1};
  uint64_t next_generation_{1};
};

} // namespace Coring5
