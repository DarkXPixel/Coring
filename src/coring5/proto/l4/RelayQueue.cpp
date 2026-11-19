#include "coring5/proto/l4/RelayQueue.hpp"

#include <algorithm>
#include <cstring>

namespace Coring5 {

bool RelayQueue::push(ChunkPool &pool,
                      std::span<const std::byte> data) noexcept {
  if (data.empty()) {
    return true;
  }
  const uint32_t cs = pool.chunk_size();
  // A default-constructed (capacity 0) pool has chunk_size 0: refuse instead of
  // dividing by zero below.
  if (cs == 0 || pool.capacity() == 0 || data.size() > UINT32_MAX - bytes_) {
    return false;
  }
  const std::size_t room = tail_ != ChunkPool::kNil ? cs - tail_fill_ : 0;
  const std::size_t rest = data.size() > room ? data.size() - room : 0;
  const std::size_t need = (rest + cs - 1) / cs;
  if (pool.free_count() < need) {
    return false;
  }

  std::size_t off = 0;
  if (room != 0) {
    const std::size_t n = std::min(room, data.size());
    std::memcpy(pool.data(tail_) + tail_fill_, data.data(), n);
    tail_fill_ += static_cast<uint32_t>(n);
    off = n;
  }
  while (off < data.size()) {
    const uint32_t c = pool.alloc(); // cannot fail: checked above
    assert(c != ChunkPool::kNil);
    const std::size_t n = std::min<std::size_t>(cs, data.size() - off);
    std::memcpy(pool.data(c), data.data() + off, n);
    if (tail_ == ChunkPool::kNil) {
      head_ = c;
      head_off_ = 0;
    } else {
      pool.set_next(tail_, c);
    }
    tail_ = c;
    tail_fill_ = static_cast<uint32_t>(n);
    off += n;
  }
  bytes_ += static_cast<uint32_t>(data.size());
  return true;
}

std::span<const std::byte>
RelayQueue::front(const ChunkPool &pool) const noexcept {
  if (head_ == ChunkPool::kNil) {
    return {};
  }
  const uint32_t end = head_ == tail_ ? tail_fill_ : pool.chunk_size();
  return {pool.data(head_) + head_off_, end - head_off_};
}

void RelayQueue::consume(ChunkPool &pool, uint32_t n) noexcept {
  assert(n <= bytes_);
  while (n != 0) {
    const uint32_t end = head_ == tail_ ? tail_fill_ : pool.chunk_size();
    const uint32_t avail = end - head_off_;
    if (n < avail) {
      head_off_ += n;
      bytes_ -= n;
      return;
    }
    // The whole head chunk is consumed: give it back.
    n -= avail;
    bytes_ -= avail;
    const uint32_t finished = head_;
    head_ = pool.next(finished);
    head_off_ = 0;
    pool.free(finished);
    if (head_ == ChunkPool::kNil) {
      tail_ = ChunkPool::kNil;
      tail_fill_ = 0;
      assert(n == 0 && bytes_ == 0);
      return;
    }
  }
}

void RelayQueue::clear(ChunkPool &pool) noexcept {
  while (head_ != ChunkPool::kNil) {
    const uint32_t next = pool.next(head_);
    pool.free(head_);
    head_ = next;
  }
  tail_ = ChunkPool::kNil;
  head_off_ = 0;
  tail_fill_ = 0;
  bytes_ = 0;
}

} // namespace Coring5
