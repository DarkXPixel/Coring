#pragma once
#include <cstddef>

namespace Coring::Memory {

    enum class MemoryType {
        Standard,
        Large,
    };

    class UnixMemory {
    public:
        UnixMemory() = delete;
        [[nodiscard]] static void* allocate(std::size_t size, MemoryType type = MemoryType::Standard) noexcept;
        void deallocate(void* ptr, size_t size) noexcept;
    };
}