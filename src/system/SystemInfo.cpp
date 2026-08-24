#include "system/SystemInfo.hpp"
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <sys/mman.h>
#include <unistd.h>

namespace Coring::System {
    bool check_large_pages_support(size_t large_page_size = 2 * 1024 * 1024) {
        void* ptr = ::mmap(nullptr, large_page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);

        if(ptr != MAP_FAILED) {
            ::munmap(ptr, large_page_size);
            return true;
        }
        return false;
    }

    size_t detect_large_page_size() noexcept {
        FILE* f = std::fopen("/proc/meminfo", "r");
        if (!f) return 2 * 1024 * 1024;

        size_t hugepagesz_kb = 0;
        char line[128];
        while (std::fgets(line, sizeof(line), f)) {
            if (std::sscanf(line, "Hugepagesize: %zu kB", &hugepagesz_kb) == 1) {
                std::fclose(f);
                return hugepagesz_kb * 1024;
            }
        }
        std::fclose(f);
        return 2 * 1024 * 1024;
    }

    bool check_thp_enabled() noexcept {
        std::ifstream file("/sys/kernel/mm/transparent_hugepage/enabled");
        std::string content;
        if (std::getline(file, content)) {
            return content.find("[always]") != std::string::npos ||
                   content.find("[madvise]") != std::string::npos;
        }
        return false;
    }

    void SystemConfig::initialize() noexcept {
        long ps = ::sysconf(_SC_PAGE_SIZE);
        info_.page_size = (ps > 0) ? static_cast<size_t>(ps) : 4096;

        long cls = ::sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
        info_.cache_line_size = (cls > 0) ? static_cast<size_t>(cls) : 64;

        long cores = ::sysconf(_SC_NPROCESSORS_ONLN);
        info_.cpu_core_count = (cores > 0) ? static_cast<size_t>(cores) : 1;

        info_.large_pages_size = detect_large_page_size();

        info_.hugetlb_supported = check_large_pages_support(info_.large_pages_size);

        info_.thp_supported = check_thp_enabled();
    }
}