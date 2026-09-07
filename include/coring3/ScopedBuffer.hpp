#pragma once

#include "coring3/BufferHandle.hpp"
#include "coring3/ThreadLocalBufferPool.hpp"
namespace Coring3 {
class ScopedBuffer {
  BufferHandle handle_;
  ThreadLocalBufferPool<> *pool_{nullptr};

public:
  ScopedBuffer() = default;
  ScopedBuffer(ThreadLocalBufferPool<> &pool)
      : pool_(&pool), handle_(pool.acquire()) {}

  ~ScopedBuffer() {
    if (pool_ && handle_.data) {
      pool_->release(handle_);
    }
  }

  ScopedBuffer(ScopedBuffer &&rhs) noexcept
      : handle_(rhs.handle_), pool_(rhs.pool_) {
    rhs.handle_.data = nullptr;
  }

  ScopedBuffer &operator=(ScopedBuffer &&rhs) noexcept {
    if (this != &rhs) {
      if (pool_ && handle_.data) {
        pool_->release(handle_);
      }
      handle_ = rhs.handle_;
      pool_ = rhs.pool_;
      rhs.handle_.data = nullptr;
    }
    return *this;
  }

  ScopedBuffer(const ScopedBuffer &) = delete;
  ScopedBuffer &operator=(const ScopedBuffer &) = delete;

  [[nodiscard]] std::byte *data() const noexcept { return handle_.data; }
  [[nodiscard]] size_t size() const noexcept { return handle_.capacity; }
};
} // namespace Coring3