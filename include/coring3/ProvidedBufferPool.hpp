#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <liburing.h>
#include <liburing/io_uring.h>
namespace Coring3 {
class ProvidedBufferPool {
  static constexpr uint16_t BGID_CLIENT_RX = 1;
  static constexpr std::size_t BUF_SIZE = 4096;
  static constexpr std::size_t NUM_BUFFERS = 1024;

private:
  io_uring_buf_ring *buf_ring_{nullptr};
  std::byte *buffer_base{nullptr};
  std::size_t ring_size_bytes_{0};

public:
  ProvidedBufferPool() noexcept = default;

  bool init(struct io_uring *ring, uint16_t bgid, uint32_t num_bufs) {
    auto total_buf_bytes = num_bufs * BUF_SIZE;
    buffer_base =
        static_cast<std::byte *>(aligned_alloc(4096, total_buf_bytes));

    if (buffer_base == nullptr) {
      return false;
    }

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

    int ret = io_uring_register_buf_ring(ring, &reg, 0);
    if (ret < 0) {
      return false;
    }

    io_uring_buf_ring_init(buf_ring_);

    for (uint32_t i = 0; i < num_bufs; ++i) {
      io_uring_buf_ring_add(buf_ring_, buffer_base + (i * BUF_SIZE), BUF_SIZE,
                            i, io_uring_buf_ring_mask(num_bufs),
                            static_cast<int>(i));
    }

    io_uring_buf_ring_advance(buf_ring_, static_cast<int>(num_bufs));
    return true;
  }
};
} // namespace Coring3