#pragma once

// Immutable configuration published to workers (docs/CONFIG_AND_RELOAD.md).
//
//   RuntimeConfig  - fixed for the process lifetime (threads, pool sizes).
//                    A reload that changes it is REJECTED ("restart required").
//   ConfigSnapshot - hot-reloadable. Built and validated on the control thread,
//                    then shared read-only (shared_ptr<const>) with all
//                    workers.
//
// Connections must never keep pointers into a snapshot (invariant I8): they
// keep ListenerId / ids and re-resolve per request.

#include "coring5/base/Endpoint.hpp"
#include "coring5/base/Types.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace Coring5 {

using Millis = std::chrono::milliseconds;

// Per-worker capacity of each connection pool, indexed by PoolId.
using PoolCapacities = std::array<uint32_t, kPoolCount>;

struct RuntimeConfig {
  uint32_t workers{1};
  std::vector<int> cpus; // optional pinning list; empty = no pinning
  uint32_t max_connections_per_worker{10000}; // budget shared by all pools
  uint32_t buffer_count{1024};                // power of two
  uint32_t buffer_size{16384};
  uint32_t ring_sq_entries{2048};
  uint32_t ring_cq_entries{8192};
  // Per-worker memory for L4 relay queues (docs/ARCHITECTURE.md section 7).
  // Mapped lazily (pages are committed on first use); only allocated when an
  // l4 pool exists. Chunk size equals buffer_size.
  uint32_t relay_memory_mib{256};
  // HTTP/1 stage chunk size (partial heads, blocked/pipelined tails). 0 = auto
  // (`limits.max_head_bytes + buffer_size` of the first config). Explicit values
  // must be >= that sum and <= 8 MiB. Restart-only (part of equality); every
  // load and reload is checked against the EFFECTIVE size below.
  uint32_t http1_stage_bytes{0};
  // Effective stage size workers allocate. Computed by parse_config(); NOT part
  // of equality (auto mode follows the limits); like pool_capacity, the first
  // configuration fixes it and reloads must fit.
  uint32_t http1_stage_effective{0};
  Millis tick{50};             // timer wheel resolution / idle wakeup period
  Millis drain_timeout{10000}; // graceful shutdown deadline
  Millis reload_ack_timeout{5000};

  // Explicit [runtime.pools] sizes, indexed by PoolId; 0 = not set. These are
  // fixed for the process lifetime (part of equality).
  PoolCapacities pools{};
  // EFFECTIVE per-worker pool capacities, computed by parse_config() from the
  // explicit sizes, the listeners' `max_connections` and the
  // max_connections_per_worker budget (see derive_pool_capacities). The first
  // configuration sizes the worker pools; later reloads must fit into them.
  // Deliberately NOT part of equality: a reload that only adds a listener
  // derives different capacities, which is fine as long as it fits.
  PoolCapacities pool_capacity{};

  [[nodiscard]] bool operator==(const RuntimeConfig &o) const {
    return workers == o.workers && cpus == o.cpus &&
           max_connections_per_worker == o.max_connections_per_worker &&
           buffer_count == o.buffer_count && buffer_size == o.buffer_size &&
           ring_sq_entries == o.ring_sq_entries &&
           ring_cq_entries == o.ring_cq_entries &&
           relay_memory_mib == o.relay_memory_mib &&
           http1_stage_bytes == o.http1_stage_bytes && tick == o.tick &&
           drain_timeout == o.drain_timeout &&
           reload_ack_timeout == o.reload_ack_timeout && pools == o.pools;
  }
};

// Defaults from docs/ARCHITECTURE.md section "Limits".
struct Limits {
  Millis idle_timeout{60000};
  Millis header_timeout{10000};
  Millis connect_timeout{3000};
  Millis keepalive_timeout{60000};
  uint32_t max_head_bytes{16 * 1024};
  uint32_t max_headers{100};
  uint32_t max_uri_bytes{8 * 1024};
  uint32_t queue_high_watermark{256 * 1024};
  uint32_t queue_low_watermark{64 * 1024};
  // HTTP/1 server session: requests served per connection (0 = unlimited) and
  // max request body size (0 = unlimited; over it -> 413 / abort TooLarge).
  uint32_t max_requests{1000};
  uint64_t max_request_body_bytes{0};

  bool operator==(const Limits &) const = default;
};

enum class LbPolicy : uint8_t { RoundRobin, LeastConnections };

// A named group of numeric TCP backends (M3). Built and validated on the
// control thread; each worker keeps its own runtime state (UpstreamPool).
struct UpstreamSpec {
  UpstreamId id{}; // assigned by ConfigManager; stable while the name matches
  std::string name;
  std::vector<Endpoint> backends;
  LbPolicy policy{LbPolicy::RoundRobin};
  Millis connect_timeout{0}; // 0 = use limits.connect_timeout
  uint32_t max_fails{
      3}; // consecutive connect failures before ejection; 0 = never eject
  Millis eject_time{10000};

  bool operator==(const UpstreamSpec &) const = default;
};

struct ListenerSpec {
  ListenerId id{}; // assigned by ConfigManager; stable while name+address match
  std::string name;
  std::string address; // as written, e.g. "0.0.0.0:8080"
  Endpoint endpoint;   // parsed
  ProtocolKind protocol{ProtocolKind::Echo};
  // Per-worker connection budget of this listener; 0 = take a share of the
  // max_connections_per_worker budget. The pool of a protocol is the sum over
  // its listeners unless [runtime.pools] overrides it.
  uint32_t max_connections{0};
  std::string route;        // route-set name (M4)
  std::string upstream;     // upstream name (M3)
  UpstreamId upstream_id{}; // resolved from `upstream` by ConfigManager
  std::string tls;          // certificate set name (M5)
};

struct ConfigSnapshot {
  uint64_t generation{0};
  Limits limits;
  std::vector<ListenerSpec> listeners;
  std::vector<UpstreamSpec> upstreams;

  [[nodiscard]] const ListenerSpec *find(ListenerId id) const noexcept {
    for (const auto &l : listeners) {
      if (l.id == id) {
        return &l;
      }
    }
    return nullptr;
  }
  [[nodiscard]] const UpstreamSpec *
  find_upstream(UpstreamId id) const noexcept {
    for (const auto &u : upstreams) {
      if (u.id == id) {
        return &u;
      }
    }
    return nullptr;
  }
};

// Raw result of parsing, before id assignment.
struct ParsedConfig {
  RuntimeConfig runtime;
  Limits limits;
  std::vector<ListenerSpec> listeners;
  std::vector<UpstreamSpec> upstreams;
};

} // namespace Coring5
