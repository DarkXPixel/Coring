#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <utility>
#include <vector>
namespace Coring4 {
// template <typename T, std::size_t Capacity> class ThreadLocalSlabPool {
// public:
//   template<typename ...Args>
//   T* alloc(Args&&... args) {
//   }
// };

template <typename T, std::size_t capacity = 1024> class ThreadLocalSlabPool {
  constexpr static auto SIZE_TYPE = sizeof(T);
  struct Node {
    Node *next{nullptr};
  };

  Node *free_list_{nullptr};
  std::vector<std::byte *> chunks_; // temp
  std::size_t available_buffers_{0};

public:
  ThreadLocalSlabPool() { grow(capacity); }

  ~ThreadLocalSlabPool() {
    for (auto &i : chunks_) {
      ::operator delete[](i, std::align_val_t{alignof(std::max_align_t)});
    }
  }

  template <typename... Args> T *allocate(Args &&...args) {
    if (free_list_ == nullptr) {
      grow(capacity);
    }

    Node *node = free_list_;
    free_list_ = free_list_->next;
    --available_buffers_;
    return std::construct_at(reinterpret_cast<T *>(node),
                             std::forward<Args>(args)...);
  }

  void deallocate(T *ptr) noexcept {
    if (ptr == nullptr) {
      return;
    }
    std::destroy_at(ptr);
    deallocate_impl(reinterpret_cast<std::byte *>(ptr));
  }

private:
  void deallocate_impl(std::byte *ptr) noexcept {
    auto *node = std::start_lifetime_as<Node>(static_cast<void *>(ptr));
    node->next = free_list_;
    free_list_ = node;
    ++available_buffers_;
  }
  void grow(std::size_t count) {
    auto *raw_chunk = static_cast<std::byte *>(::operator new[](
        count * SIZE_TYPE, std::align_val_t{alignof(std::max_align_t)}));

    if (raw_chunk == nullptr) [[unlikely]] {
      std::terminate(); //
    }
    chunks_.push_back(raw_chunk);

    for (std::size_t i = 0; i < count; ++i) {
      auto *buffer_ptr = raw_chunk + (i * SIZE_TYPE);
      deallocate_impl(buffer_ptr);
    }
  }
};
} // namespace Coring4