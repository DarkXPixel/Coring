#pragma once

#include <cstddef>
#include <span>
namespace Coring3 {
struct BufferHandle {
  std::byte *data{nullptr};
  size_t capacity{0};
  bool is_provided{false};

  [[nodiscard]] std::span<std::byte> as_span() const noexcept {
    return {data, capacity};
  }
};
} // namespace Coring3