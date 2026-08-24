#pragma once

#include <cstddef>
namespace Coring::System {
    struct SystemInfo {
        size_t page_size{4096};
        size_t cache_line_size{64};
        size_t cpu_core_count{1};
        size_t large_pages_size{0};
        bool hugetlb_supported{false};
        bool thp_supported{false};
    };

    class SystemConfig {
    public:
        SystemConfig() = delete;

        static void initialize() noexcept;
        [[nodiscard]] static const SystemInfo& get() noexcept {
            return info_;
        }

        [[nodiscard]] static size_t page_size() noexcept {
            return info_.page_size;
        }

    private:
        static inline SystemInfo info_{};
    };
}