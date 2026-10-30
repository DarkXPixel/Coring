#pragma once

#include <cstdint>
namespace Coring4 {
struct IoUringBackendConfig {
  std::uint32_t sq_entries{4096};
  std::uint32_t cq_entries{8192};
  bool single_issuer{true};
  bool multishot_accpet{true};
  bool multishot_recv{true};
  std::uint32_t buffer_size{16384};
  std::uint32_t buffer_pool_size{8192};
};
} // namespace Coring4
