#pragma once

// One completion, decoded from an io_uring CQE. Shared by Ring and FakeRing
// (io/RingSeam.hpp) so connection types never depend on which one they run on.

#include "coring5/io/OpTag.hpp"
#include <cstdint>
#include <liburing.h>

namespace Coring5 {

struct Cqe {
  OpTag tag;
  int32_t res;
  uint32_t flags;

  [[nodiscard]] bool more() const noexcept {
    return (flags & IORING_CQE_F_MORE) != 0U;
  }
  [[nodiscard]] bool has_buffer() const noexcept {
    return (flags & IORING_CQE_F_BUFFER) != 0U;
  }
  [[nodiscard]] bool is_notif() const noexcept {
    return (flags & IORING_CQE_F_NOTIF) != 0U;
  }
  [[nodiscard]] uint16_t buffer_id() const noexcept {
    return static_cast<uint16_t>(flags >> IORING_CQE_BUFFER_SHIFT);
  }
};

} // namespace Coring5
