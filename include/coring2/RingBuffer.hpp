#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
namespace Coring2 {
template <std::size_t Capacity> class alignas(64) RingBuffer {
private:
  alignas(64) std::array<std::byte, Capacity> data_;
  std::size_t head_;
  std::size_t tail_;

public:
  std::span<std::byte> prepare_write() noexcept {
    std::size_t mask = Capacity - 1;
    std::size_t write_pos = tail_ & mask;
    std::size_t available = Capacity - (tail_ - head_);
    std::size_t contiguous = std::min(available, Capacity - write_pos);
    return {data_.data() + write_pos, contiguous};
  }

  void commit_write(std::size_t bytes) noexcept { tail_ += bytes; }

  std::span<const std::byte> prepare_read() const noexcept {
    size_t mask = Capacity - 1;
    size_t read_pos = head_ & mask;
    size_t available = tail_ - head_;
    size_t contiguous = std::min(available, Capacity - read_pos);
    return {data_.data() + read_pos, contiguous};
  }

  void consume_read(std::size_t bytes) noexcept { head_ += bytes; }

  [[nodiscard]] bool empty() const noexcept { return head_ == tail_; }

  [[nodiscard]] std::size_t size() const noexcept { return tail_ - head_; }

  void clear() noexcept { head_ = tail_ = 0; }
};
} // namespace Coring2