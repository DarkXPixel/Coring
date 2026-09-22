#pragma once

#include <array>
#include <cassert>
#include <cstddef>
template <typename T, std::size_t Capacity>
  requires((Capacity & (Capacity - 1)) == 0)
class StackRingBuffer {
public:
  static constexpr std::size_t MASK = Capacity - 1;

  bool push(T &&item) noexcept {
    if (full()) {
      return false;
    }
    data_[tail_] = std::move(item);
    tail_ = (tail_ + 1) & MASK;
    ++size_;
    return true;
  }

  void pop() noexcept {
    assert(!empty());
    data_[head_].~T();
    head_ = (head_ + 1) & MASK;
    --size_;
  }

  [[nodiscard]] T &front() noexcept {
    assert(!empty());
    return data_[head_];
  }

  [[nodiscard]] const T &front() const noexcept {
    assert(!empty());
    return data_[head_];
  }

  [[nodiscard]] bool empty() const noexcept { return head_ == tail_; }
  [[nodiscard]] bool full() const noexcept { return size() == Capacity; }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }

  template <typename Func> void for_each(Func &&func) {
    for (std::size_t i = head_; i != tail_; ++i) {
      if (!func(data_[i & MASK])) {
        break;
      }
    }
  }

private:
  std::size_t head_{0};
  std::size_t tail_{0};
  std::size_t size_{0};
  alignas(64) std::array<T, Capacity> data_;
};