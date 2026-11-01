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
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace Coring5 {

using Millis = std::chrono::milliseconds;

struct RuntimeConfig {
  uint32_t workers{1};
  std::vector<int> cpus; // optional pinning list; empty = no pinning
  uint32_t max_connections_per_worker{10000};
  uint32_t buffer_count{1024}; // power of two
  uint32_t buffer_size{16384};
  uint32_t ring_sq_entries{2048};
  uint32_t ring_cq_entries{8192};
  Millis tick{50};             // timer wheel resolution / idle wakeup period
  Millis drain_timeout{10000}; // graceful shutdown deadline
  Millis reload_ack_timeout{5000};

  bool operator==(const RuntimeConfig &) const = default;
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

  bool operator==(const Limits &) const = default;
};

struct ListenerSpec {
  ListenerId id{}; // assigned by ConfigManager; stable while name+address match
  std::string name;
  std::string address; // as written, e.g. "0.0.0.0:8080"
  Endpoint endpoint;   // parsed
  ProtocolKind protocol{ProtocolKind::Echo};
  std::string route;    // route-set name (M4)
  std::string upstream; // upstream pool name (M3)
  std::string tls;      // certificate set name (M5)
};

struct ConfigSnapshot {
  uint64_t generation{0};
  Limits limits;
  std::vector<ListenerSpec> listeners;

  [[nodiscard]] const ListenerSpec *find(ListenerId id) const noexcept {
    for (const auto &l : listeners) {
      if (l.id == id) {
        return &l;
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
};

} // namespace Coring5
