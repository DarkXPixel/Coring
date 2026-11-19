#pragma once

// Fixed-size chunk allocator for relay queues (invariants I6, I7).
//
//  * One lazily-faulted mapping (MAP_NORESERVE, no pre-populate): resident
//    memory follows the high-water mark of queued bytes and can never exceed
//    `chunk_size * count`, which is what makes "slow reader keeps RSS bounded"
//    a property of the design instead of a hope.
//  * alloc()/free() are O(1) and never allocate. alloc() returns kNil when
//    the pool is exhausted; the caller sheds load.
//  * Each chunk has one `next` link, used first by the free list and then by
//    whichever queue owns the chunk (a chunk is in exactly one of the two).
//  * Single-threaded: owned by one worker.

#include "coring5/base/Result.hpp"
#include "coring5/mem/Region.hpp"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace Coring5 {

class ChunkPool {
public:
  static constexpr uint32_t kNil = 0xFFFFFFFFU;

  // Empty pool (capacity 0): alloc() always fails.
  ChunkPool() = default;
  ChunkPool(const ChunkPool &) = delete;
  ChunkPool &operator=(const ChunkPool &) = delete;
  ChunkPool(ChunkPool &&) noexcept = default;
  ChunkPool &operator=(ChunkPool &&) noexcept = default;
  ~ChunkPool() = default;

  [[nodiscard]] static Result<ChunkPool> create(uint32_t chunk_size,
                                                uint32_t count);

  [[nodiscard]] uint32_t alloc() noexcept {
    if (free_head_ == kNil) [[unlikely]] {
      return kNil;
    }
    const uint32_t i = free_head_;
    free_head_ = next_[i];
    next_[i] = kNil;
    --free_;
    if (count_ - free_ > peak_) {
      peak_ = count_ - free_;
    }
    return i;
  }

  void free(uint32_t i) noexcept {
    assert(i < count_ && free_ < count_);
    next_[i] = free_head_;
    free_head_ = i;
    ++free_;
  }

  [[nodiscard]] std::byte *data(uint32_t i) noexcept {
    assert(i < count_);
    return slab_.data() + static_cast<std::size_t>(i) * chunk_size_;
  }
  [[nodiscard]] const std::byte *data(uint32_t i) const noexcept {
    assert(i < count_);
    return slab_.data() + static_cast<std::size_t>(i) * chunk_size_;
  }

  // Queue link of an allocated chunk.
  [[nodiscard]] uint32_t next(uint32_t i) const noexcept { return next_[i]; }
  void set_next(uint32_t i, uint32_t n) noexcept { next_[i] = n; }

  [[nodiscard]] uint32_t chunk_size() const noexcept { return chunk_size_; }
  [[nodiscard]] uint32_t capacity() const noexcept { return count_; }
  [[nodiscard]] uint32_t free_count() const noexcept { return free_; }
  [[nodiscard]] uint32_t in_use() const noexcept { return count_ - free_; }
  // High-water mark of in_use() since creation: the resident-memory bound.
  [[nodiscard]] uint32_t peak_in_use() const noexcept { return peak_; }
  // Less than 1/8 free: connections that already hold queued data stop reading
  // so that the remaining chunks go to connections that can still make
  // progress.
  [[nodiscard]] bool low() const noexcept {
    return static_cast<uint64_t>(free_) * 8U < count_;
  }

private:
  Region slab_;
  std::unique_ptr<uint32_t[]> next_;
  uint32_t chunk_size_{0};
  uint32_t count_{0};
  uint32_t free_{0};
  uint32_t free_head_{kNil};
  uint32_t peak_{0};
};

} // namespace Coring5
