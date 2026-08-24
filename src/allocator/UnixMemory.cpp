#include "allocator/UnixMemory.hpp"
#include "system/SystemInfo.hpp"
#include "utility/MemoryUtility.hpp"
#include <cassert>
#include <new>
#include <sys/mman.h>

namespace Coring::Memory {
    void* UnixMemory::allocate(std::size_t size, MemoryType type) noexcept {
        assert(size);
        if(size == 0) [[unlikely]] {
            return nullptr;
        }

        const auto& cfg = System::SystemConfig::get();
        constexpr int flags = MAP_PRIVATE | MAP_ANONYMOUS;
        constexpr int prot = PROT_READ | PROT_WRITE;

        if(type == MemoryType::Large) {
            const size_t huge_size = Utility::align_up(size, static_cast<std::align_val_t>(cfg.large_pages_size)); 

            if(cfg.hugetlb_supported) {
                void* ptr = ::mmap(nullptr, huge_size, prot, flags | MAP_HUGETLB, -1, 0);
                if(ptr != MAP_FAILED) [[likely]] {
                    return ptr;
                }
            }

            if(cfg.thp_supported) {
                return nullptr;
            }
        }

        const size_t page_size = Utility::align_up(size, std::align_val_t(cfg.page_size));
        void* ptr = ::mmap(nullptr, page_size, prot, flags, -1, 0);
        return (ptr == MAP_FAILED) ? nullptr : ptr;
    }

    void UnixMemory::deallocate(void *ptr, size_t size) noexcept {
        const size_t ps = System::SystemConfig::page_size();
        const size_t page_aligned_size = (size + ps - 1) & ~(ps -1);
        ::munmap(ptr, page_aligned_size);   
    }
}