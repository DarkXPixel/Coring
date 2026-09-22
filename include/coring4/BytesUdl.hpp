#pragma once

#include <cstdint>
namespace Coring4 {
constexpr uint64_t operator""_B(unsigned long long bytes) { return bytes; }

constexpr uint64_t operator""_KB(unsigned long long kbytes) {
  return kbytes * 1024;
}

constexpr uint64_t operator""_MB(unsigned long long mbytes) {
  return mbytes * 1024 * 1024;
}

constexpr uint64_t operator""_GB(unsigned long long gbytes) {
  return gbytes * 1024 * 1024 * 1024;
}
} // namespace Coring4