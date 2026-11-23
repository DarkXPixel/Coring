#pragma once

#include "coring5/base/Result.hpp"
#include "coring5/obs/AccessLog.hpp"
#include <atomic>
#include <span>
#include <string>
#include <thread>

namespace Coring5 {

struct AccessLogRotation {
  uint64_t bytes{0};
  uint8_t files{0}; // archives, excluding the active file
};

// One consumer for a stable array of per-worker rings. Lifecycle calls belong
// to one control thread. Rings must outlive stop(), including destructor stop.
// The sink must be a regular file opened for append. Each line is
// tab-separated: unix_ms, duration_us, listener, bytes_in, bytes_out, status,
// method, target. String fields encode non-printable bytes and '%' as %HH.
// Rotation reserves path.1 through path.N, newest first. It runs before
// appending a line that would exceed the size limit. A single oversized line
// may occupy a file by itself. The writer needs exclusive use of these paths.
class AccessLogWriter {
public:
  AccessLogWriter() = default;
  AccessLogWriter(const AccessLogWriter &) = delete;
  AccessLogWriter &operator=(const AccessLogWriter &) = delete;
  ~AccessLogWriter();

  [[nodiscard]] Result<> start(const char *path,
                               std::span<AccessLogRing> rings,
                               AccessLogRotation rotation = {});
  // Quiesce producers first. Success means every accepted record was written
  // and the file closed (no fsync/durability guarantee). An I/O error stops
  // consumption and is returned here, including on repeated stop calls.
  [[nodiscard]] Result<> stop();
  [[nodiscard]] int error() const noexcept {
    return error_.load(std::memory_order_relaxed);
  }

private:
  void run(int fd, uint64_t size, std::span<AccessLogRing> rings,
           AccessLogRotation rotation) noexcept;

  std::string path_;
  std::thread thread_;
  std::atomic<bool> stopping_{false};
  std::atomic<int> error_{0};
};

} // namespace Coring5
