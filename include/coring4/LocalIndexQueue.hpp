#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
namespace Coring4 {
template <typename T, std::size_t Capacity>
  requires(((Capacity & (Capacity - 1)) == 0) &&
           Capacity < std::numeric_limits<uint8_t>::max())
class LocalIndexQueue {
public:
  bool push(T index) noexcept {
    if (size_ == Capacity) {
      return false;
    }
    buffer_[tail_] = index;
    tail_ = (tail_ + 1) & MASK;
    ++size_;
    return true;
  }

  [[nodiscard]] T *pop() noexcept {
    if (size_ == 0) {
      return nullptr;
    }

    T &index = buffer_[head_];
    head_ = (head_ + 1) & MASK;
    --size_;
    return &index;
  }

  [[nodiscard]] T *peek() noexcept {
    if (size_ == 0) {
      return nullptr;
    }
    return &buffer_[head_];
  }

  [[nodiscard]] uint8_t size() const noexcept { return size_; }

  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

private:
  static constexpr std::size_t MASK = Capacity - 1;
  std::array<T, Capacity> buffer_;
  uint8_t head_{0};
  uint8_t tail_{0};
  uint8_t size_{};
};
}; // namespace Coring4