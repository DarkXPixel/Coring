#pragma once

#include "coring4/PoolType.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
namespace Coring4 {

class BufferHandle16 {
  static constexpr uint32_t IS_PROVIDED_BUFFER = 1;

  union {
    std::byte *ptr_{nullptr};
    const char *t;
  };
  uint64_t size_ : 56 {0};
  PoolType pool_id_{-1};

  // uint32_t flags_ : 8;

public:
  BufferHandle16() = default;
  BufferHandle16(std::span<const std::byte> buf, PoolType pool_id)
      : pool_id_(pool_id), ptr_(const_cast<std::byte *>(buf.data())),
        size_(buf.size()) {}
  BufferHandle16(const BufferHandle16 &) = default;
  BufferHandle16 &operator=(const BufferHandle16 &) = default;

  [[nodiscard]] std::span<std::byte> as_span() const noexcept {
    return {ptr_, static_cast<std::size_t>(size_)};
  }

  [[nodiscard]] uint16_t size() const noexcept { return size_; }

  void set_size(uint32_t size) noexcept { size_ = size; }

  [[nodiscard]] std::span<std::byte> as_raw_span() const noexcept {
    return {ptr_, size_};
  }

  [[nodiscard]] std::byte *as_raw() const noexcept { return ptr_; }

  [[nodiscard]] std::byte *data() const noexcept { return ptr_; }

  [[nodiscard]] bool is_valid() const noexcept { return ptr_ != nullptr; }

  [[nodiscard]] PoolType get_pool_id() const noexcept { return pool_id_; }

  // void set_is_provided() noexcept { flags_ |= IS_PROVIDED_BUFFER; }
  // [[nodiscard]] bool is_provided() const noexcept {
  //   return (flags_ & IS_PROVIDED_BUFFER);
  // }
};
} // namespace Coring4