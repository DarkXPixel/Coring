#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <vector>
namespace Coring4 {

template <typename T, std::size_t ChunkSize = 1024> class IndexObjectPool {
  struct Node {
    uint32_t next_free;
  };

  std::vector<T *> chunks_;
  uint32_t free_head_{UINT32_MAX};
  std::size_t available_buffers_{0};
  std::size_t capacity_{0};

public:
  IndexObjectPool() { grow(); }

  ~IndexObjectPool() {
    for (auto &i : chunks_) {
      ::operator delete[](i, std::align_val_t{alignof(T)});
    }
  }

  [[nodiscard]] uint32_t allocate() {
    if (free_head_ == UINT32_MAX) {
      grow();
    }

    uint32_t index = free_head_;
    T *ptr = get_pointer(index);
    Node *node = reinterpret_cast<Node *>(ptr);

    free_head_ = node->next_free;
    --available_buffers_;

    std::construct_at(ptr, T());
    return index;
  }
  struct allocate_result {
    uint32_t index;
    T *ptr;
  };

  template <typename... Args>
  [[nodiscard]] allocate_result allocate_and_get_and_construct(Args &&...args) {
    if (free_head_ == UINT32_MAX) {
      grow();
    }

    uint32_t index = free_head_;
    T *ptr = get_pointer(index);
    Node *node = reinterpret_cast<Node *>(ptr);

    free_head_ = node->next_free;
    --available_buffers_;
    std::construct_at(ptr, std::forward<Args>(args)...);

    return {index, ptr};
  }

  template <typename... Args>
  [[nodiscard]] uint32_t allocate_and_construct(Args &&...args) {
    if (free_head_ == UINT32_MAX) {
      grow();
    }

    uint32_t index = free_head_;
    T *ptr = get_pointer(index);
    Node *node = reinterpret_cast<Node *>(ptr);

    free_head_ = node->next_free;
    --available_buffers_;
    std::construct_at(ptr, std::forward<Args>(args)...);

    return index;
  }

  [[nodiscard]] allocate_result allocate_and_get() {
    if (free_head_ == UINT32_MAX) {
      grow();
    }

    uint32_t index = free_head_;
    T *ptr = get_pointer(index);
    Node *node = reinterpret_cast<Node *>(ptr);

    free_head_ = node->next_free;
    --available_buffers_;

    return {index, std::construct_at(ptr, T())};
  }

  [[nodiscard]] T *get(uint32_t index) { return get_pointer(index); }

  void deallocate(uint32_t index) noexcept {
    if (index == UINT32_MAX) [[unlikely]] {
      return;
    }
    deallocate_impl(index);
  }

  [[nodiscard]] std::size_t available() const noexcept {
    return available_buffers_;
  }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
  T *get_pointer(uint32_t index) const noexcept {
    uint32_t chunk_idx = index / ChunkSize;
    uint32_t elem_idx = index % ChunkSize;
    return chunks_[chunk_idx] + elem_idx;
  }

  void deallocate_impl(uint32_t index) noexcept {
    T *ptr = get_pointer(index);
    Node *node = reinterpret_cast<Node *>(ptr);
    node->next_free = free_head_;
    free_head_ = index;
    ++available_buffers_;
  }

  void grow() {
    auto *raw_chunk = static_cast<T *>(
        ::operator new[](ChunkSize * sizeof(T), std::align_val_t{alignof(T)}));

    if (raw_chunk == nullptr) [[unlikely]] {
      std::terminate();
    }

    auto new_chunk_idx = static_cast<uint32_t>(chunks_.size());
    chunks_.push_back(raw_chunk);

    for (std::size_t i = ChunkSize; i > 0; --i) {
      std::size_t elem_idx = i - 1;
      uint32_t current_idx =
          (new_chunk_idx * ChunkSize) + static_cast<uint32_t>(elem_idx);

      T *ptr = raw_chunk + elem_idx;
      Node *node = reinterpret_cast<Node *>(ptr);
      node->next_free = free_head_;
      free_head_ = current_idx;
      ++available_buffers_;
    }

    capacity_ += ChunkSize;
  }
};
} // namespace Coring4