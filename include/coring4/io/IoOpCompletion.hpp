#pragma once

#include "IoOp.hpp"
#include <cstdint>
#include <liburing/io_uring.h>
namespace Coring4 {
struct IoCompletion {
  IoOp *op{};
  std::int32_t result{};
  std::uint32_t flags{};

  [[nodiscard]] bool more() const noexcept {
    return (flags & IORING_CQE_F_MORE) != 0U;
  }

  [[nodiscard]] bool buffer_selected() const noexcept {
    return (flags & IORING_CQE_F_BUFFER) != 0U;
  }

  [[nodiscard]] std::uint16_t buffer_id() const noexcept {
    return static_cast<std::uint16_t>(flags >> IORING_CQE_BUFFER_SHIFT);
  }
};
} // namespace Coring4
