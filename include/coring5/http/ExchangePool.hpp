#pragma once

// ExchangePool: fixed-capacity pool of Exchanges + the ReadyList (one entry per
// live Exchange). docs/EXCHANGE.md s.4.4.

#include "coring5/http/Exchange.hpp"
#include "coring5/http/ReadyList.hpp"
#include "coring5/mem/ChunkPool.hpp"
#include "coring5/mem/HandlePool.hpp"
#include <algorithm>
#include <cstdint>

namespace Coring5 {

struct ExchangeStats {
  uint64_t opened{0};
  uint64_t shed{0};
  uint64_t reaped{0};
};

class ExchangePool {
public:
  ExchangePool(uint32_t capacity, ChunkPool &chunks, StreamLimits limits);

  // Applies to Exchanges opened AFTER the call; live ones keep theirs (a reload
  // never changes watermarks under an in-flight body).
  void set_limits(StreamLimits limits) noexcept { limits_ = limits; }
  // Full => invalid handle (the ingress answers 503 itself).
  [[nodiscard]] Handle open(SideRef ingress, RequestMeta meta) noexcept;
  // nullptr if stale/invalid.
  [[nodiscard]] Exchange *get(Handle h) noexcept { return pool_.get(h); }
  [[nodiscard]] bool has_ready() const noexcept { return !ready_.empty(); }
  [[nodiscard]] uint32_t live() const noexcept { return pool_.live(); }
  [[nodiscard]] const ExchangeStats &stats() const noexcept { return stats_; }

  // Delivers up to `budget` of the entries present at call time (stale ones
  // are skipped silently). `listed_` is cleared BEFORE deliver(h, Exchange&) so
  // events raised inside it re-list; an Exchange that is reapable and not
  // re-listed afterwards is destroyed (E11). Returns delivered count.
  template <class F> uint32_t drain(uint32_t budget, F &&deliver) {
    const uint32_t n = std::min(budget, ready_.size());
    uint32_t done = 0;
    for (uint32_t i = 0; i < n; ++i) {
      const auto h = ready_.pop();
      if (!h) {
        break;
      }
      Exchange *ex = pool_.get(*h);
      if (ex == nullptr) {
        continue;
      }
      ex->listed_ = false;
      deliver(*h, *ex);
      ++done;
      if (ex->reapable() && !ex->listed_) {
        (void)pool_.destroy(*h);
        ++stats_.reaped;
      }
    }
    return done;
  }

private:
  ChunkPool *chunks_;
  StreamLimits limits_;
  HandlePool<Exchange> pool_;
  ReadyList ready_;
  ExchangeStats stats_{};
};

} // namespace Coring5
