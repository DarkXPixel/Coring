#pragma once

#include <cstdint>

namespace Coring5 {

struct RingConfig {
  uint32_t sq_entries{2048};
  uint32_t cq_entries{8192};
  bool defer_taskrun{true};
  bool enforce_min_kernel{true}; // reject kernels older than 6.1
};

} // namespace Coring5
