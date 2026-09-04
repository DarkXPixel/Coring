#pragma once

#include <cstddef>
#include <memory>
#include <new>
#include <span>
#include <vector>
namespace Coring2 {
class FixedBufferPool4096 {
public:
  static constexpr std::size_t BufferSize = 4096;

  FixedBufferPool4096(std::size_t inital_capacity) { grow(inital_capacity); }
  ~FixedBufferPool4096() {
    for (auto *chunk : chunks_) {
      ::operator delete[](chunk, std::align_val_t{alignof(std::align_val_t)});
    }
  }
  [[nodiscard]] std::span<std::byte> allocate() {
    if (free_list_ == nullptr) {
      grow(capacity_ == 0 ? 16 : capacity_);
    }
    Node *node = free_list_;
    free_list_ = free_list_->pNext;
    std::span<std::byte> result(reinterpret_cast<std::byte *>(node),
                                BufferSize);
    std::destroy_at(node);
    return result;
  }

  void deallocate(std::byte *ptr) noexcept {
    if (ptr == nullptr) [[unlikely]] {
      return;
    }

    auto *node = std::construct_at(reinterpret_cast<Node *>(ptr));
    node->pNext = free_list_;
    free_list_ = node;
  }

private:
  struct Node {
    Node *pNext{nullptr};
  };

  void grow(std::size_t buffer_count) {
    std::size_t bytes_to_alloc = buffer_count * BufferSize;
    auto *raw_chunk = static_cast<std::byte *>(::operator new[](
        bytes_to_alloc, std::align_val_t{alignof(std::max_align_t)}));

    chunks_.push_back(raw_chunk);

    for (std::size_t i = 0; i < buffer_count; ++i) {
      auto *buffer_ptr = raw_chunk + (i * BufferSize);
      deallocate(buffer_ptr);
    }

    capacity_ += buffer_count;
  }
  Node *free_list_{nullptr};
  std::vector<std::byte *> chunks_;
  std::size_t capacity_{0};
};
} // namespace Coring2