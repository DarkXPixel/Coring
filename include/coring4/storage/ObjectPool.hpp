#pragma once

#include <cstdint>
#include <memory>
#include <vector>
namespace Coring4 {
template <typename T, std::size_t ChunkSize = 1024> class ObjectPool {
  struct Node {
    Node *next_free;
  };

  std::vector<T *> chunks_;
  Node *free_head_{nullptr};
  std::size_t available_buffers_{0};
  std::size_t capacity_{0};

public:
  ObjectPool() { grow(); }

  ~ObjectPool() {
    for (auto &i : chunks_) {
      ::operator delete[](i, std::align_val_t{alignof(T)});
    }
  }

  ObjectPool(const ObjectPool &) = delete;
  ObjectPool &operator=(const ObjectPool &) = delete;
  ObjectPool(ObjectPool &&) noexcept = default;
  ObjectPool &operator=(ObjectPool &&) noexcept = default;

  [[nodiscard]] T *allocate() {
    if (free_head_ == nullptr) {
      grow();
    }

    Node *node = free_head_;
    free_head_ = node->next_free;
    --available_buffers_;

    T *ptr = reinterpret_cast<T *>(node);
    std::construct_at(ptr, T());
    return ptr;
  }

  template <typename... Args>
  [[nodiscard]] T *allocate_and_construct(Args &&...args) {
    if (free_head_ == nullptr) {
      grow();
    }

    Node *node = free_head_;
    free_head_ = node->next_free;
    --available_buffers_;

    T *ptr = reinterpret_cast<T *>(node);
    std::construct_at(ptr, std::forward<Args>(args)...);
    return ptr;
  }

  void deallocate(T *ptr) noexcept {
    if (ptr == nullptr) [[unlikely]] {
      return;
    }

    std::destroy_at(ptr);

    Node *node = reinterpret_cast<Node *>(ptr);
    node->next_free = free_head_;
    free_head_ = node;
    ++available_buffers_;
  }

  [[nodiscard]] std::size_t available() const noexcept {
    return available_buffers_;
  }

  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
  void grow() {
    auto *raw_chunk = static_cast<T *>(
        ::operator new[](ChunkSize * sizeof(T), std::align_val_t{alignof(T)}));

    if (raw_chunk == nullptr) [[unlikely]] {
      std::terminate();
    }

    chunks_.push_back(raw_chunk);

    for (std::size_t i = ChunkSize; i > 0; --i) {
      std::size_t elem_idx = i - 1;
      T *ptr = raw_chunk + elem_idx;
      Node *node = reinterpret_cast<Node *>(ptr);

      node->next_free = free_head_;
      free_head_ = node;
      ++available_buffers_;
    }

    capacity_ += ChunkSize;
  }
};
} // namespace Coring4