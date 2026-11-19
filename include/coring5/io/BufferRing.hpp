#pragma once

// Provided-buffer ring (IORING_REGISTER_PBUF_RING) over one contiguous slab.
//
//   buffer id -> address is `base + bid * buf_size` (no side table).
//   recycle() passes the real buffer SIZE as the length. The legacy
//   ProvidedBufferPool passed the buffer COUNT here by mistake.
//
// Invariant I10: never hold a buffer across an idle period. A Lease is meant to
// live for one recv->parse->(send|copy) step; partial data is copied into a
// bounded staging buffer.

#include "coring5/base/Result.hpp"
#include "coring5/io/Ring.hpp"
#include "coring5/mem/Region.hpp"
#include <cassert>
#include <cstdint>
#include <span>
#include <utility>

namespace Coring5 {

class BufferRing {
public:
  // `count` must be a power of two in [2, 32768].
  [[nodiscard]] static Result<BufferRing>
  create(Ring &ring, uint16_t bgid, uint32_t count, uint32_t buf_size);

  BufferRing(const BufferRing &) = delete;
  BufferRing &operator=(const BufferRing &) = delete;
  BufferRing(BufferRing &&o) noexcept;
  BufferRing &operator=(BufferRing &&o) noexcept;
  ~BufferRing();

  // Move-only RAII over one kernel-selected buffer. Recycles on destruction.
  class Lease {
  public:
    Lease() = default;
    Lease(const Lease &) = delete;
    Lease &operator=(const Lease &) = delete;
    Lease(Lease &&o) noexcept
        : ring_(std::exchange(o.ring_, nullptr)), bid_(o.bid_), len_(o.len_) {}
    Lease &operator=(Lease &&o) noexcept {
      if (this != &o) {
        release();
        ring_ = std::exchange(o.ring_, nullptr);
        bid_ = o.bid_;
        len_ = o.len_;
      }
      return *this;
    }
    ~Lease() { release(); }

    [[nodiscard]] bool valid() const noexcept { return ring_ != nullptr; }
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
      return ring_ ? ring_->view(bid_, len_) : std::span<const std::byte>{};
    }
    [[nodiscard]] uint32_t size() const noexcept { return len_; }
    [[nodiscard]] uint16_t id() const noexcept { return bid_; }
    [[nodiscard]] uint32_t buf_size() const noexcept {
      assert(ring_);
      return ring_->buf_size();
    }

    void release() noexcept {
      if (ring_ != nullptr) {
        ring_->recycle(bid_);
        ring_ = nullptr;
      }
    }

  private:
    friend class BufferRing;
    Lease(BufferRing *r, uint16_t bid, uint32_t len) noexcept
        : ring_(r), bid_(bid), len_(len) {}
    BufferRing *ring_{nullptr};
    uint16_t bid_{0};
    uint32_t len_{0};
  };

  // Called when a CQE carries IORING_CQE_F_BUFFER.
  [[nodiscard]] Lease take(uint16_t bid, uint32_t len) noexcept {
    --available_;
    return Lease(this, bid, len);
  }

  [[nodiscard]] std::span<const std::byte> view(uint16_t bid,
                                                uint32_t len) const noexcept {
    return {slab_.data() + static_cast<std::size_t>(bid) * buf_size_, len};
  }

  // Cheap (local); the kernel sees the new tail only after publish().
  void recycle(uint16_t bid) noexcept;
  // One store per loop iteration.
  void publish() noexcept;

  [[nodiscard]] uint16_t bgid() const noexcept { return bgid_; }
  [[nodiscard]] uint32_t count() const noexcept { return count_; }
  [[nodiscard]] uint32_t buf_size() const noexcept { return buf_size_; }
  // Buffers currently owned by the kernel/ring (drives load shedding).
  [[nodiscard]] uint32_t free_count() const noexcept { return available_; }

private:
  BufferRing() = default;
  void destroy() noexcept;

  Ring *ring_{nullptr};
  io_uring_buf_ring *br_{nullptr};
  Region slab_;
  uint32_t count_{0};
  uint32_t buf_size_{0};
  uint32_t available_{0};
  int mask_{0};
  uint16_t bgid_{0};
  unsigned pending_{0};
};

} // namespace Coring5
