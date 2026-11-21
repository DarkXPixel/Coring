#pragma once

// ReadyList: preallocated FIFO ring of Exchange handles with pending events.
// Capacity = pool capacity and an Exchange is listed at most once (its
// `listed_` flag), so push() can never overflow on the data path (I7).
// Single-threaded (owned by the worker).

#include "coring5/base/Types.hpp"
#include <cstdint>
#include <memory>
#include <optional>

namespace Coring5 {

class ReadyList {
public:
  explicit ReadyList(uint32_t capacity)
      : cap_(capacity), buf_(std::make_unique<Handle[]>(capacity)) {}
  ReadyList(const ReadyList &) = delete;
  ReadyList &operator=(const ReadyList &) = delete;

  // false if full (cannot happen while each Exchange is listed once).
  [[nodiscard]] bool push(Handle h) noexcept {
    if (size_ == cap_) {
      return false;
    }
    buf_[(head_ + size_) % cap_] = h;
    ++size_;
    return true;
  }
  [[nodiscard]] std::optional<Handle> pop() noexcept {
    if (size_ == 0) {
      return std::nullopt;
    }
    const Handle h = buf_[head_];
    head_ = (head_ + 1) % cap_;
    --size_;
    return h;
  }
  [[nodiscard]] uint32_t size() const noexcept { return size_; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
  [[nodiscard]] uint32_t capacity() const noexcept { return cap_; }

private:
  uint32_t cap_;
  uint32_t head_{0};
  uint32_t size_{0};
  std::unique_ptr<Handle[]> buf_;
};

} // namespace Coring5
