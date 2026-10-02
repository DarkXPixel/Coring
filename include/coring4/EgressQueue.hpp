#pragma once
#include "BytesUdl.hpp"
#include "coring4/BufferHandle.hpp"
#include "coring4/StackRingBuffer.hpp"
#include "coring4/ThreadLocalSlabPool.hpp"

#include <boost/circular_buffer.hpp>
#include <boost/circular_buffer/base.hpp>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <sys/socket.h>
#include <sys/uio.h>
namespace Coring4 {

struct alignas(64) PendingChunk {
  std::array<BufferHandle16, 2> handles;
  uint32_t total_bytes{0};
  uint32_t bytes_sent{0};
  uint32_t current_handle_idx{0};
  uint8_t handle_count{0};

  void add_handle(BufferHandle16 handle) noexcept {
    assert(handle_count < 2);
    total_bytes += handle.size();
    handles[handle_count++] = handle;
  }
  [[nodiscard]] uint32_t remaining_bytes() const noexcept {
    return total_bytes - bytes_sent;
  }
};

static_assert(sizeof(PendingChunk) == 64, "PendingChunk must be 64B");

class EgressQueue {
public:
  constexpr static std::size_t DEFAULT_RING_CAPACITY = 256;
  constexpr static std::size_t MAX_IOV_BATCH = 16ULL;
  constexpr static std::size_t HIGH_WATERMARK = 2_MB;
  constexpr static std::size_t LOW_WATERMARK = 512_KB;
  constexpr static std::size_t ZC_THRESHOLD_BYTES = 4096;

  EgressQueue(int fd) : fd_(fd) {}

  bool push(PendingChunk &&chunk,
            ThreadLocalSlabPool<StackRingBuffer<PendingChunk, 32>, 16> &slab) {
    buffered_bytes_ += chunk.remaining_bytes();

    const bool use_overflow =
        overflow_ring_ != nullptr || inline_count_ >= inline_chunks_.size();

    if (!use_overflow) {
      inline_chunks_[inline_count_++] = std::move(chunk);
    } else {
      if (overflow_ring_ == nullptr) {
        overflow_ring_ = slab.allocate();
        if (overflow_ring_ == nullptr) {
          return false;
        }
      }

      if (!overflow_ring_->push(std::move(chunk))) {
        return false;
      }
    }
    return buffered_bytes_ >= HIGH_WATERMARK;
  }

  bool prepare_send_sqe(io_uring_sqe *sqe) {
    if (empty() || flush_pending_) {
      return false;
    }

    iov_count_ = 0;
    std::size_t total_payload_bytes = 0;
    std::size_t offset = get_front_chunk().bytes_sent;

    auto collect_iov = [&](PendingChunk &chunk) {
      for (uint8_t i = 0; i < chunk.handle_count; ++i) {
        const auto &handle = chunk.handles[i];
        if (offset >= handle.size()) {
          offset -= handle.size();
          continue;
        }
        auto &iov = iov_batch_[iov_count_++];
        iov.iov_base = reinterpret_cast<char *>(handle.data()) + offset;
        iov.iov_len = handle.size() - offset;
        total_payload_bytes += iov.iov_len;
        offset = 0;
        if (iov_count_ == MAX_IOV_BATCH) {
          return false;
        }
      }
      return true;
    };

    for (std::size_t i = inline_head_; i < inline_count_; ++i) {
      if (!collect_iov(inline_chunks_[i])) {
        break;
      }
      offset = 0;
    }

    if (iov_count_ < 16 && (overflow_ring_ != nullptr)) {
      overflow_ring_->for_each(
          [&](PendingChunk &chunk) { return collect_iov(chunk); });
    }

    if (iov_count_ == 0) {
      return false;
    }

    const bool use_zc = false; // (total_payload_bytes >= ZC_THRESHOLD_BYTES);

    if (iov_count_ == 1) {
      void *buf_addr = iov_batch_[0].iov_base;
      auto buf_len = static_cast<uint32_t>(iov_batch_[0].iov_len);

      if (use_zc) {
        io_uring_prep_send_zc(sqe, fd_, buf_addr, buf_len, 0, 0);
      } else {
        io_uring_prep_send(sqe, fd_, buf_addr, buf_len, 0);
      }
    } else {
      send_msg_ = {};
      send_msg_.msg_iov = iov_batch_.data();
      send_msg_.msg_iovlen = iov_count_;
      if (use_zc) {
        io_uring_prep_sendmsg_zc(sqe, fd_, &send_msg_, 0);
      } else {
        io_uring_prep_sendmsg(sqe, fd_, &send_msg_, 0);
      }
    }

    flush_pending_ = true;
    return true;
  }

  template <typename Func>
  bool on_send_complete(
      uint32_t bytes_transfered,
      ThreadLocalSlabPool<StackRingBuffer<PendingChunk, 32>, 16> &slab,
      Func &&callback_deallocate) {
    flush_pending_ = false;
    buffered_bytes_ -= std::min(buffered_bytes_, bytes_transfered);

    while (bytes_transfered > 0 && !empty()) {
      PendingChunk &front = get_front_chunk();
      uint32_t remaining = front.remaining_bytes();

      if (bytes_transfered >= remaining) {
        bytes_transfered -= remaining;
        for (int i = 0; i < front.handle_count; ++i) {
          callback_deallocate(front.handles[i]);
        }
        pop_front_chunk(slab);
      } else {
        front.bytes_sent += bytes_transfered;
        bytes_transfered = 0;
      }
    }
    return buffered_bytes_ <= LOW_WATERMARK;
  }
  template <typename Func>
  void clear(ThreadLocalSlabPool<StackRingBuffer<PendingChunk, 32>, 16> &slab,
             Func &&callback_deallocate) {
    while (!empty()) {
      auto &chunk = get_front_chunk();
      for (int i = 0; i < chunk.handle_count; ++i) {
        callback_deallocate(chunk.handles[i]);
      }
      pop_front_chunk(slab);
    }
  }

  bool empty() const noexcept {
    return inline_head_ == inline_count_ &&
           (overflow_ring_ == nullptr || overflow_ring_->empty());
  }

private:
  PendingChunk &get_front_chunk() noexcept {
    if (inline_head_ < inline_count_) {
      return inline_chunks_[inline_head_];
    }
    return overflow_ring_->front();
  }

  void pop_front_chunk(ThreadLocalSlabPool<StackRingBuffer<PendingChunk, 32>,
                                           16> &slab) noexcept {
    if (inline_head_ < inline_count_) {
      ++inline_head_;
      if (inline_head_ == inline_count_) {
        inline_head_ = 0;
        inline_count_ = 0;
      }
    } else if (overflow_ring_ != nullptr) {
      overflow_ring_->pop();
      if (overflow_ring_->empty()) {
        slab.deallocate(overflow_ring_);
        overflow_ring_ = nullptr;
      }
    }
  }

  int fd_{-1};
  uint32_t buffered_bytes_{0};

  std::array<PendingChunk, 2> inline_chunks_;
  uint8_t inline_head_{0};
  uint8_t inline_count_{0};
  bool flush_pending_{false};

  StackRingBuffer<PendingChunk, 32> *overflow_ring_{nullptr};
  std::array<struct iovec, MAX_IOV_BATCH> iov_batch_;
  std::size_t iov_count_{0};
  msghdr send_msg_;
};
} // namespace Coring4