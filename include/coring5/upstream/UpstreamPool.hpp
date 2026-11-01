#pragma once

// Upstream connection management (M3 for TCP, M4 for HTTP keep-alive reuse).
//
// STATUS: interface only. Per-worker; DNS is resolved on the control thread and
// delivered inside the ConfigSnapshot as numeric endpoints, so the data path
// never calls getaddrinfo().

#include "coring5/base/Endpoint.hpp"
#include "coring5/base/Result.hpp"
#include "coring5/base/Types.hpp"
#include <chrono>
#include <string>
#include <vector>

namespace Coring5 {

enum class LbPolicy : uint8_t { RoundRobin, LeastConnections };

struct BackendSpec {
  Endpoint endpoint;
  uint32_t weight{1};
};

struct UpstreamSpec {
  UpstreamId id{};
  std::string name;
  std::vector<BackendSpec> backends;
  LbPolicy policy{LbPolicy::RoundRobin};
  std::chrono::milliseconds connect_timeout{3000};
  uint32_t max_idle_keepalive{64}; // per worker
  uint32_t max_fails{3}; // passive health: eject after N consecutive fails
  std::chrono::milliseconds eject_time{10000};
};

// TODO(M3): class UpstreamPool {
//   Result<BackendRef> pick(UpstreamId);             // LB + passive health
//   void report(BackendRef, bool ok, Nanos latency); // feeds ejection
//   // M4: idle keep-alive fd cache with liveness probe (MSG_PEEK on reuse)
// };

} // namespace Coring5
