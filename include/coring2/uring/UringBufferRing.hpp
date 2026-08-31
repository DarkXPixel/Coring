#pragma once

#include <cstddef>
#include <cstdint>
#include <liburing/io_uring.h>
namespace Coring2 {
    struct BufferGroup {
        uint16_t bgid;
        uint32_t buf_count;
        uint32_t buf_size;

        io_uring_buf_ring* buf_ring{nullptr};
        std::byte* data_arena{nullptr};
    };


}