#pragma once

// Access-log record + ring. Workers push, a dedicated writer thread
// formats and writes. Nothing here allocates on the worker.

#include "coring5/base/Types.hpp"
#include "coring5/obs/SpscRing.hpp"
#include <array>
#include <cstdint>

namespace Coring5 {

struct AccessRecord {
  uint64_t unix_ms{0}; // when the session began parsing this request
  uint32_t duration_us{0};
  uint32_t listener{0};
  uint64_t bytes_in{0};  // parsed head and consumed wire body bytes
  uint64_t bytes_out{0}; // bytes confirmed by downstream send completions
  uint16_t status{0};    // serialized final status, or 0 if none was built
  uint8_t method_len{0};
  uint8_t target_len{0};
  std::array<char, 8> method{};
  std::array<char, 96> target{}; // truncated copy: no heap
};

// One producer per ring. push() returns false on overflow.
using AccessLogRing = SpscRing<AccessRecord, 4096>;

} // namespace Coring5
