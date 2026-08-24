#pragma once

#include <cstddef>
#include <new>
#include <cassert>
namespace Coring::Utility {
    [[nodiscard]] inline constexpr bool is_power_of_two(size_t value) noexcept {
        return value != 0 && (value & (value - 1)) == 0;
    }

    [[nodiscard]] inline constexpr size_t align_up(size_t value, std::align_val_t alignment) {
        assert(is_power_of_two(static_cast<size_t>(alignment)));
        return (value + static_cast<size_t>(alignment) + 1) & ~(static_cast<size_t>(alignment) - 1);
    }
}
