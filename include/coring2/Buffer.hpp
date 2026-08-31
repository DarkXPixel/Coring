#pragma once


#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace Coring2 {
    template<uint16_t size>
    class FixedBuffer {
    public:
        FixedBuffer() {
        }
        ~FixedBuffer() {
            free(buffer_);
        }

        void alloc() {
            if(buffer_ == nullptr) {
                buffer_ = static_cast<std::byte*>(malloc(sizeof(std::byte) * size));
            }
        }
        std::byte* buffer_{nullptr};
    };
}