#pragma once

#include "BufferHandle.hpp"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <limits>
#include <memory>
#include <new>
#include <vector>
namespace Coring4 {
constexpr std::size_t DefaultBlockSize16Kb = 16384;
template <std::size_t BlockSize = DefaultBlockSize16Kb, bool auto_grow = false,
          std::align_val_t alignment =
              std::align_val_t{alignof(std::max_align_t)}>
class LocalBufferPool {
  struct Node {
    Node *next{nullptr};
  };

  Node *free_list_{nullptr};
  std::vector<std::byte *> chunks_; // temp
  std::size_t capacity{0};
  std::size_t available_buffers_{0};
  int8_t pool_id_{-1};

public:
  LocalBufferPool(int8_t pool_id, std::size_t initial_capacity = 1024)
      : pool_id_(pool_id) {
    grow(initial_capacity);
  }

  [[nodiscard]] auto allocate() {
    if (free_list_ == nullptr) {
      if constexpr (BlockSize <= std::numeric_limits<uint16_t>::max()) {
        return BufferHandle16{std::span<std::byte>(), pool_id_};
      } else {
        static_assert(false);
      }
      grow(capacity == 0 ? 16 : capacity);
    }

    Node *node = free_list_;
    free_list_ = free_list_->next;
    std::span<std::byte> result(reinterpret_cast<std::byte *>(node), BlockSize);
    std::destroy_at(node);
    if constexpr (BlockSize <= std::numeric_limits<uint16_t>::max()) {
      --available_buffers_;
      return BufferHandle16{result, pool_id_};
    } else {
      static_assert(false);
    }
  }

  void deallocate(BufferHandle16 buf) noexcept {
    if (!buf.is_valid()) [[unlikely]] {
      return;
    }
    deallocate_impl(buf.as_raw());
  }

private:
  void deallocate_impl(std::byte *ptr) noexcept {
    auto *node = std::construct_at(reinterpret_cast<Node *>(ptr));
    node->next = free_list_;
    free_list_ = node;
    ++available_buffers_;
  }

  void grow(std::size_t count) {
    auto *raw_chunk = static_cast<std::byte *>(::operator new[](
        count * BlockSize, std::align_val_t{alignof(std::max_align_t)}));

    if (raw_chunk == nullptr) [[unlikely]] {
      std::terminate(); //
    }
    chunks_.push_back(raw_chunk);

    for (std::size_t i = 0; i < count; ++i) {
      auto *buffer_ptr = raw_chunk + (i * BlockSize);
      deallocate_impl(buffer_ptr);
    }
    capacity += count;
  }
};
} // namespace Coring4