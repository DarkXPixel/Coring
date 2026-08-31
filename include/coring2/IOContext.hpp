#pragma once

#include <cstdint>
#include <netinet/in.h>
#include <sys/socket.h>
namespace Coring2 {
    enum class IOType : uint8_t {
        Accept = 0,
        Read = 1,
        Write = 2,
        SpliceL4 = 3,
        Timer = 4,
        Close = 5,
        Cancel = 6,
    };

    constexpr uint64_t TYPE_MASK = 0b111; // 7
    inline uint64_t encode_user_data(void* ptr, IOType type) {
        return reinterpret_cast<uint64_t>(ptr) | static_cast<uint64_t>(type);
    }

    inline uint64_t encode_user_data_bgid(void* ptr, IOType type, uint16_t bgid = 0) noexcept {
        uint64_t uptr = reinterpret_cast<uint64_t>(ptr);
        uint64_t packed_ptr_type = uptr | static_cast<uint8_t>(type);
        return (static_cast<uint64_t>(bgid) << 48) | (packed_ptr_type & 0x0000FFFFFFFFFFFFULL);
    }

    inline uint16_t decode_bgid(uint64_t user_data) noexcept {
        return static_cast<uint16_t>(user_data >> 48);
    }

    inline IOType decode_type(uint64_t user_data) noexcept {
        return static_cast<IOType>(user_data & 0b111);
    }

    inline void* decode_ptr(uint64_t user_data) noexcept {
        constexpr uint64_t PTR_MASK = 0x0000FFFFFFFFFFF8ULL;
        return reinterpret_cast<void*>(user_data & PTR_MASK);
    }


    struct IOContext {
        IOType type;
        int fd{-1};
        void* data{nullptr};
    };

    struct IOAcceptContext {
        void* data{nullptr};
        int fd{-1};
        sockaddr_storage client_addr{};
        socklen_t addr_len{sizeof(client_addr)};

        void reset_len() noexcept {
            addr_len = sizeof(client_addr);
        }
    };
}