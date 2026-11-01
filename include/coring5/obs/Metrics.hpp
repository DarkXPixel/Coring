#pragma once

// Per-worker metric shards. Each worker thread is the ONLY writer of its shard
// (relaxed atomics, no read-modify-write contention); the control thread reads.
// Shards are cache-line aligned so workers never false-share.

#include "coring5/base/Result.hpp"
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <span>
#include <string>

namespace Coring5 {

using Counter = std::atomic<uint64_t>;

inline void inc(Counter &c, uint64_t n = 1) noexcept {
  c.fetch_add(n, std::memory_order_relaxed);
}
inline void set(Counter &c, uint64_t v) noexcept {
  c.store(v, std::memory_order_relaxed);
}
[[nodiscard]] inline uint64_t get(const Counter &c) noexcept {
  return c.load(std::memory_order_relaxed);
}

// Log2 histogram: bucket i holds values in [2^(i-1), 2^i). Good enough to spot
// loop stalls; use HDR histograms on the load-generator side for real latency.
class Log2Histogram {
public:
  static constexpr std::size_t kBuckets = 48; // up to ~2^47 ns = 39 hours

  void record(uint64_t value) noexcept {
    auto b = static_cast<std::size_t>(std::bit_width(value));
    if (b >= kBuckets) {
      b = kBuckets - 1;
    }
    inc(buckets_[b]);
  }

  [[nodiscard]] uint64_t count() const noexcept {
    uint64_t n = 0;
    for (const auto &b : buckets_) {
      n += get(b);
    }
    return n;
  }

  // Upper bound of the bucket containing quantile q in [0,1]; 0 if empty.
  [[nodiscard]] uint64_t quantile_upper_bound(double q) const noexcept {
    const uint64_t total = count();
    if (total == 0) {
      return 0;
    }
    const auto target = static_cast<uint64_t>(static_cast<double>(total) * q);
    uint64_t seen = 0;
    for (std::size_t i = 0; i < kBuckets; ++i) {
      seen += get(buckets_[i]);
      if (seen > target) {
        return i == 0 ? 0 : (uint64_t{1} << i);
      }
    }
    return uint64_t{1} << (kBuckets - 1);
  }

private:
  std::array<Counter, kBuckets> buckets_{};
};

struct alignas(64) WorkerMetrics {
  Counter accepts{0};
  Counter accept_errors{0};
  Counter conns_rejected{0}; // pool full, load shed
  Counter conns_live{0};     // gauge
  Counter stale_events{0};   // MUST stay 0 (invariant I4)
  Counter nobufs{0};
  Counter sq_full{0};
  Counter cq_overflow{0};
  Counter bytes_in{0};
  Counter bytes_out{0};
  Counter loop_iterations{0};
  Counter cqes{0};
  Counter buffers_free{0}; // gauge
  Counter config_generation{0};
  std::array<Counter, kErrcCount> closes{};
  Log2Histogram loop_ns;
};

// Prometheus-style text, aggregated over all shards.
[[nodiscard]] std::string render_metrics(std::span<const WorkerMetrics> shards);

} // namespace Coring5
