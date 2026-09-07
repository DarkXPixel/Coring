#pragma once
#include <cstdint>

namespace Coring3 {

// [32bit: generation] [24bit: index] [8bit: opcode]
struct UserData {
  uint32_t generation;
  uint32_t index : 24;
  uint8_t op_code;

  [[nodiscard]] uint64_t pack() const noexcept {
    return (static_cast<uint64_t>(generation) << 32) |
           (static_cast<uint64_t>(index) << 8) | static_cast<uint64_t>(op_code);
  }

  static UserData unpack(uint64_t val) noexcept {
    return UserData{.generation = static_cast<uint32_t>(val >> 32),
                    .index = static_cast<uint32_t>((val >> 8) & 0xFFFFFF),
                    .op_code = static_cast<uint8_t>(val & 0xFF)};
  }
};

static_assert(sizeof(UserData) == sizeof(uint64_t));
} // namespace Coring3