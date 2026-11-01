#pragma once

#include <cstdint>

namespace Coring4 {

// Handle to a route set inside one concrete RouteTable.
//
// `generation` ties the handle to the table it was resolved against. A stale
// handle (resolved against a previous table and not re-bound after a swap)
// is rejected by RouteTable::match() instead of silently pointing at an
// unrelated route set. generation == 0 means "invalid / unbound".
struct RouteSetId {
  uint32_t index{0};
  uint32_t generation{0};

  [[nodiscard]] constexpr bool is_valid() const noexcept {
    return generation != 0;
  }
  constexpr bool operator==(const RouteSetId &) const noexcept = default;
};

} // namespace Coring4
