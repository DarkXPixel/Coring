#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
namespace Coring4 {
class BufferHandle16 {
  static constexpr uint32_t IS_PROVIDED_BUFFER = 1;

  std::byte *ptr_{nullptr};
  uint16_t size_{0};
  uint16_t offset_{0};
  int8_t pool_id_{-1};

  uint32_t flags_ : 24;

public:
  BufferHandle16() = default;
  BufferHandle16(std::span<std::byte> buf, int8_t pool_id)
      : pool_id_(pool_id), ptr_(buf.data()), size_(buf.size()) {}
  BufferHandle16(const BufferHandle16 &) = default;
  BufferHandle16 &operator=(const BufferHandle16 &) = default;

  [[nodiscard]] std::span<std::byte> as_span() const noexcept {
    return {ptr_ + offset_, static_cast<std::size_t>(size_ - offset_)};
  }

  [[nodiscard]] uint16_t size() const noexcept { return size_; }
  [[nodiscard]] uint16_t offset() const noexcept { return offset_; }

  void move_offset(int32_t offset) noexcept { offset_ += offset; }

  [[nodiscard]] std::span<std::byte> as_raw_span() const noexcept {
    return {ptr_, size_};
  }

  [[nodiscard]] std::byte *as_raw() const noexcept { return ptr_; }

  [[nodiscard]] bool is_valid() const noexcept { return ptr_ != nullptr; }

  void set_is_provided() noexcept { flags_ |= IS_PROVIDED_BUFFER; }
  [[nodiscard]] bool is_provided() const noexcept {
    return (flags_ & IS_PROVIDED_BUFFER);
  }
};
} // namespace Coring4