#pragma once

// Byte queue of ONE relay direction (invariant I6): a singly linked list of
// pool chunks. Data is appended into the free tail of the last chunk, so every
// chunk except the tail is full and a stalled direction wastes at most one
// partial chunk.
//
//  * push() is all-or-nothing: it checks the pool first and leaves the queue
//    untouched when the data does not fit.
//  * front() is the contiguous run at the head. Its address stays valid until
//    consume() passes it, even while push() appends behind it, which is what
//    allows an in-flight send to read straight from the queue.
//  * An empty queue owns no chunk (nothing is parked on an idle connection).
//
// The queue stores no pool pointer (it is 20 bytes); every call that needs the
// pool takes it explicitly.

#include "coring5/mem/ChunkPool.hpp"
#include <cstdint>
#include <span>

namespace Coring5 {

class ChunkQueue {
public:
  ChunkQueue() = default;
  ChunkQueue(const ChunkQueue &) = delete;
  ChunkQueue &operator=(const ChunkQueue &) = delete;
  // The queue must be clear()ed before destruction (the owner does that);
  // destroying a non-empty queue would leak chunks, so it is asserted.
  ~ChunkQueue() { assert(bytes_ == 0 && head_ == ChunkPool::kNil); }

  [[nodiscard]] bool empty() const noexcept { return bytes_ == 0; }
  [[nodiscard]] uint32_t bytes() const noexcept { return bytes_; }

  // Appends a copy of `data`. False (and no change) if the pool cannot supply
  // the chunks or the byte counter would overflow.
  [[nodiscard]] bool push(ChunkPool &pool,
                          std::span<const std::byte> data) noexcept;

  // Contiguous readable bytes at the head (empty span when the queue is empty).
  [[nodiscard]] std::span<const std::byte>
  front(const ChunkPool &pool) const noexcept;

  // Drops `n` bytes from the head (n <= bytes()), freeing finished chunks.
  void consume(ChunkPool &pool, uint32_t n) noexcept;

  // Returns every chunk to the pool.
  void clear(ChunkPool &pool) noexcept;

private:
  uint32_t head_{ChunkPool::kNil};
  uint32_t tail_{ChunkPool::kNil};
  uint32_t head_off_{0};  // bytes of the head chunk already consumed
  uint32_t tail_fill_{0}; // bytes of the tail chunk that hold data
  uint32_t bytes_{0};
};

} // namespace Coring5
