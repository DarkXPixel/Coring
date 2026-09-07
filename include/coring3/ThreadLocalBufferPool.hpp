#pragma once

#include "coring3/BufferHandle.hpp"
#include <cstddef>
#include <memory>
#include <vector>

namespace Coring3 {
template <typename std::size_t BlockSize = 16384> class ThreadLocalBufferPool {
  struct Node {
    Node *pNext;
  };
  Node *free_list_{nullptr};
  std::vector<std::unique_ptr<std::byte[]>> chunks_;
  size_t capacity_{0};
  size_t allocated_count_{0};

public:
  explicit ThreadLocalBufferPool(size_t initial_blocks = 1024) {
    grow(initial_blocks);
  }

  BufferHandle acquire() {
    if (!free_list_) {
      grow(capacity_ == 0 ? 128 : capacity_);
    }

    Node *node = free_list_;
    free_list_ = free_list_->pNext;
    ++allocated_count_;

    return BufferHandle{.data = reinterpret_cast<std::byte *>(node),
                        .capacity = BlockSize};
  }

  void release(BufferHandle handle) noexcept {
    if (handle.data == nullptr) {
      return;
    }

    auto *node = reinterpret_cast<Node *>(handle.data);
    node->next = free_list_;
    free_list_ = node;
    --allocated_count_;
  }

private:
  void grow(size_t count) {
    auto chunk = std::make_unique<std::byte[]>(count * BlockSize);
    std::byte *raw_ptr = chunk.get();

    for (size_t i = 0; i < count; ++i) {
      auto *node = reinterpret_cast<Node *>(raw_ptr + (i * BlockSize));
      node->next = free_list_;
      free_list_ = node;
    }
    chunks_.push_back(std::move(chunk));
    capacity_ += count;
  }
};
} // namespace Coring3