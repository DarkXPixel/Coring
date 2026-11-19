#pragma once

// Per-worker backend selection for l4 relays (M3). Pure data structure: no
// sockets, no ring, no allocation in pick()/acquire()/release()/report().
//
// DNS is resolved on the control thread; the ConfigSnapshot carries numeric
// endpoints, and apply_config() copies them, so a connection never points into
// a snapshot (invariant I8).
//
// Selection
//   * RoundRobin / LeastConnections over the backends that are not ejected and
//     not in the caller's `exclude` list (backends already tried by this
//     connection).
//   * Passive health: `max_fails` consecutive connect failures eject a backend
//     for `eject_time`. After the time elapses the backend is eligible again;
//     because its fail counter is kept, ONE more failure re-ejects it
//     (half-open probe). A success resets the counter.
//   * Panic mode: when every non-excluded backend is ejected, pick() ignores
//     ejection and still picks (same policy) instead of refusing all traffic;
//     BackendRef::panic tells the caller.
//
// Reload
//   * Every upstream entry carries an `epoch`. apply_config() keeps the entry
//     (state and epoch) when its spec is unchanged and gives a changed one a
//     fresh epoch with clean state; removed ones disappear. A BackendRef
//     obtained before a reload therefore never touches the state of an
//     unrelated backend: acquire/release/report with a stale epoch are no-ops.

#include "coring5/base/Endpoint.hpp"
#include "coring5/base/Result.hpp"
#include "coring5/base/Types.hpp"
#include "coring5/config/Snapshot.hpp"

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace Coring5 {

struct BackendRef {
  UpstreamId upstream{};
  uint32_t backend{0};
  uint32_t epoch{0}; // 0 = invalid
  bool panic{false}; // picked although every candidate was ejected

  [[nodiscard]] bool valid() const noexcept { return epoch != 0; }
};

class UpstreamPool {
public:
  // Worker thread, on ApplyConfig. May allocate (cold path).
  void apply_config(const ConfigSnapshot &snapshot);

  // `exclude`: backend indexes this connection already tried.
  // Errors: UpstreamUnavailable (unknown upstream / nothing left to try).
  [[nodiscard]] Result<BackendRef>
  pick(UpstreamId id, TimePoint now,
       std::span<const uint32_t> exclude = {}) noexcept;

  // Endpoint of a picked backend; nullptr if the ref is stale. The pointer is
  // valid only until the next apply_config(): copy it before suspending.
  [[nodiscard]] const Endpoint *endpoint(BackendRef ref) const noexcept;

  [[nodiscard]] uint32_t backend_count(UpstreamId id) const noexcept;
  // The upstream's own connect timeout, or `fallback` when it sets none.
  [[nodiscard]] Millis connect_timeout(UpstreamId id,
                                       Millis fallback) const noexcept;

  // Balanced pair: acquire() when a connection to the backend starts,
  // release() when it ends (least-connections accounting).
  void acquire(BackendRef ref) noexcept;
  void release(BackendRef ref) noexcept;

  // Connect outcome. Returns true if this report ejected the backend.
  bool report(BackendRef ref, bool ok, TimePoint now) noexcept;

  // Introspection (tests, metrics).
  [[nodiscard]] uint32_t active(BackendRef ref) const noexcept;
  [[nodiscard]] bool ejected(BackendRef ref, TimePoint now) const noexcept;

private:
  struct BackendState {
    uint32_t active{0};
    uint32_t consecutive_fails{0};
    TimePoint ejected_until{};
    bool ever_ejected{false};
  };
  struct Entry {
    UpstreamSpec spec;
    std::vector<BackendState> state;
    uint32_t epoch{0};
    uint32_t rr_cursor{0};
  };

  [[nodiscard]] Entry *find(UpstreamId id) noexcept;
  [[nodiscard]] const Entry *find(UpstreamId id) const noexcept;
  [[nodiscard]] Entry *resolve(BackendRef ref) noexcept;
  [[nodiscard]] const Entry *resolve(BackendRef ref) const noexcept;
  [[nodiscard]] static bool is_ejected(const BackendState &s,
                                       TimePoint now) noexcept {
    return s.ever_ejected && now < s.ejected_until;
  }
  // Index of the chosen backend, or kNone.
  [[nodiscard]] static uint32_t select(Entry &e, TimePoint now,
                                       std::span<const uint32_t> exclude,
                                       bool honour_ejection) noexcept;

  static constexpr uint32_t kNone = 0xFFFFFFFFU;

  uint32_t epoch_counter_{0};
  std::vector<Entry> entries_; // few upstreams: linear lookup beats hashing
};

// Move-only RAII over one acquire()/release() pair (least-connections
// accounting). Whoever holds the lease holds the right to report() health for
// that backend; destroying it releases the slot. It points at the worker's
// UpstreamPool, so it must not outlive it (the worker destroys connections
// before the pool) and must stay on that worker's thread.
class BackendLease {
public:
  BackendLease() = default;
  BackendLease(UpstreamPool &pool, BackendRef ref) noexcept
      : pool_(&pool), ref_(ref) {
    pool_->acquire(ref_);
  }
  BackendLease(const BackendLease &) = delete;
  BackendLease &operator=(const BackendLease &) = delete;
  BackendLease(BackendLease &&o) noexcept
      : pool_(std::exchange(o.pool_, nullptr)),
        ref_(std::exchange(o.ref_, BackendRef{})) {}
  BackendLease &operator=(BackendLease &&o) noexcept {
    if (this != &o) {
      reset();
      pool_ = std::exchange(o.pool_, nullptr);
      ref_ = std::exchange(o.ref_, BackendRef{});
    }
    return *this;
  }
  ~BackendLease() { reset(); }

  [[nodiscard]] bool valid() const noexcept { return pool_ != nullptr; }
  [[nodiscard]] const BackendRef &ref() const noexcept { return ref_; }

  void reset() noexcept {
    if (pool_ != nullptr) {
      pool_->release(ref_);
      pool_ = nullptr;
      ref_ = {};
    }
  }

private:
  UpstreamPool *pool_{nullptr};
  BackendRef ref_{};
};

} // namespace Coring5
