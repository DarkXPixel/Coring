#pragma once

#include "coring4/BufferHandle.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <span>
#include <vector>
namespace Coring4 {
template <std::size_t BUF_SIZE = 16384> class ProvidedBufferPool {
public:
  static constexpr std::size_t NUM_BUFFERS = 1024;

private:
  io_uring_buf_ring *buf_ring_{nullptr};
  std::size_t ring_size_bytes_{0};
  uint16_t bgid_{0};

  std::vector<BufferHandle16> pool_bufs_; // temp
public:
  ProvidedBufferPool() noexcept = default;

  bool init(struct io_uring *ring, uint16_t bgid, uint32_t num_bufs) {
    ring_size_bytes_ = sizeof(struct io_uring_buf) * num_bufs;
    buf_ring_ = static_cast<struct io_uring_buf_ring *>(
        aligned_alloc(4096, ring_size_bytes_));

    if (buf_ring_ == nullptr) {
      return false;
    }

    struct io_uring_buf_reg reg{};
    reg.ring_addr = reinterpret_cast<uint64_t>(buf_ring_);
    reg.ring_entries = num_bufs;
    reg.bgid = bgid;

    bgid_ = bgid;

    int ret = io_uring_register_buf_ring(ring, &reg, 0);
    if (ret < 0) {
      return false;
    }

    io_uring_buf_ring_init(buf_ring_);
    pool_bufs_.resize(num_bufs);
    return true;
  }

  void recycle_buffer(BufferHandle16 handle, uint16_t bid) noexcept {
    pool_bufs_[bid] = handle;
    io_uring_buf_ring_add(buf_ring_, handle.as_raw(), BUF_SIZE, bid,
                          io_uring_buf_ring_mask(NUM_BUFFERS), bid);
  }

  BufferHandle16 get_by_bid(uint16_t bid) noexcept { return pool_bufs_[bid]; }

  auto get_bgid() const noexcept { return bgid_; }

  void advanace(int count) noexcept {
    io_uring_buf_ring_advance(buf_ring_, count);
  }
};
} // namespace Coring4