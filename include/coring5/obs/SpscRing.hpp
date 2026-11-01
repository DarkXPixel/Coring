#pragma once

// Bounded wait-free single-producer / single-consumer ring.
// Intended use: worker (producer) -> logging/writer thread (consumer), so the
// data path never blocks on disk I/O. push() on a full ring DROPS the item and
// reports false (callers count the drop); it never blocks.

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace Coring5 {

template <class T, std::size_t N> class SpscRing {
  static_assert(std::has_single_bit(N), "capacity must be a power of two");

public:
  bool push(T v) noexcept {
    const auto h = head_.load(std::memory_order_relaxed);
    const auto t = tail_.load(std::memory_order_acquire);
    if (h - t == N) {
      return false;
    }
    buf_[h & (N - 1)] = std::move(v);
    head_.store(h + 1, std::memory_order_release);
    return true;
  }

  std::optional<T> pop() noexcept {
    const auto t = tail_.load(std::memory_order_relaxed);
    const auto h = head_.load(std::memory_order_acquire);
    if (t == h) {
      return std::nullopt;
    }
    T v = std::move(buf_[t & (N - 1)]);
    tail_.store(t + 1, std::memory_order_release);
    return v;
  }

  [[nodiscard]] std::size_t size_approx() const noexcept {
    return head_.load(std::memory_order_acquire) -
           tail_.load(std::memory_order_acquire);
  }

private:
  alignas(64) std::atomic<std::size_t> head_{0};
  alignas(64) std::atomic<std::size_t> tail_{0};
  alignas(64) std::array<T, N> buf_{};
};

} // namespace Coring5
