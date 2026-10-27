#pragma once

#include <cstdint>
namespace Coring4 {
struct SessionBase {
  int fd{-1};

  std::uint32_t outstandings_ops{0};

  bool closing{false};
  bool destroy_pending{false};
};
} // namespace Coring4