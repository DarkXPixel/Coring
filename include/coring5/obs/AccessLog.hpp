#pragma once

// Access-log record + ring (M7). Workers push, a dedicated writer thread
// formats and writes. Nothing here allocates on the worker.

#include "coring5/base/Types.hpp"
#include "coring5/obs/SpscRing.hpp"
#include <array>
#include <cstdint>

namespace Coring5 {

struct AccessRecord {
  uint64_t unix_ms{0};
  uint32_t duration_us{0};
  uint32_t listener{0};
  uint64_t bytes_in{0};
  uint64_t bytes_out{0};
  uint16_t status{0};
  uint8_t method_len{0};
  uint8_t target_len{0};
  std::array<char, 8> method{};
  std::array<char, 96> target{}; // truncated copy: no heap
};

// TODO(M7): AccessLogWriter thread (batching, rotation on SIGUSR1/reopen).
using AccessLogRing = SpscRing<AccessRecord, 4096>;

} // namespace Coring5
