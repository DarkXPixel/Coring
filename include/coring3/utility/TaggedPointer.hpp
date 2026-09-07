#pragma once

#include <cassert>
#include <concepts>
#include <cstdint>
#include <type_traits>
namespace Coring3::Utility {

template <typename T>
concept IntegerOrEnum = std::integral<T> || std::is_enum_v<T>;
class TaggedPointer {
  static constexpr uintptr_t TAG_MASK = 0x0F; // 1111
  static constexpr uintptr_t PTR_MASK = ~TAG_MASK;

public:
  template <typename T, IntegerOrEnum U>
  static uint64_t pack(T *ptr, U tag) noexcept {
    static_assert(alignof(T) % 16 == 0, "Pointer must be aligned by 16 byte");
    auto addr = reinterpret_cast<uintptr_t>(ptr);
    assert((addr & TAG_MASK) == 0 && "Pointer must be aligned by 16 byte");
    return static_cast<uint64_t>(addr | static_cast<uintptr_t>(tag));
  }

  template <typename T> static T *unpack_ptr(uint64_t user_data) noexcept {
    return reinterpret_cast<T *>(static_cast<uintptr_t>(user_data) & PTR_MASK);
  }

  template <IntegerOrEnum U> static U unpack_tag(uint64_t user_data) noexcept {
    return static_cast<U>(static_cast<uintptr_t>(user_data) & TAG_MASK);
  }
};
} // namespace Coring3::Utility