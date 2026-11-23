#pragma once

// Everything a connection may touch on its worker. Owned by the Worker and
// passed by reference into every callback. This is a plain struct of references
// (not an interface): no virtual dispatch on the hot path, and protocol modules
// depend on `io/` instead of on `runtime/` (see docs/ARCHITECTURE.md layering).

#include "coring5/base/Types.hpp"
#include "coring5/config/Snapshot.hpp"
#include "coring5/io/RingSeam.hpp"
#include "coring5/io/TimerWheel.hpp"
#include "coring5/mem/ChunkPool.hpp"
#include "coring5/obs/AccessLog.hpp"
#include "coring5/obs/Metrics.hpp"
#include <span>
#include <vector>

namespace Coring5 {

struct StaticRoots;
class UpstreamPool;  // upstream/UpstreamPool.hpp (kept out of io's include graph)
class IdleUpstreams;
class ExchangePool;  // http/ExchangePool.hpp (io never includes http)

struct IoContext {
  IoRing &ring;
  IoBufferRing &buffers;
  TimerWheel &timers;
  WorkerMetrics &metrics;
  const Limits *limits{nullptr}; // swapped by the worker on config apply
  TimePoint now{};               // refreshed once per loop iteration

  // Connections that reached Closed. The worker destroys them at a safe point
  // (end of the loop iteration), never from inside their own callback.
  std::vector<ConnRef> closed;

  void mark_closed(ConnRef c) { closed.push_back(c); }

  // Per-worker services for l4 relays. Both outlive every connection (the
  // worker destroys connections first). The chunk pool is empty (capacity 0)
  // when no l4 listener exists.
  UpstreamPool *upstreams{nullptr};
  ChunkPool *chunks{nullptr};

  // HTTP/1 services (null/empty without an http1 pool): the Exchange pool and
  // the stage ChunkPool (chunk = http1_stage_bytes). Outlive every connection.
  ExchangePool *exchanges{nullptr};
  ChunkPool *stage{nullptr};

  // Set by the worker immediately before ConnPool::spawn(); read once by the
  // new connection's start(). Plain ids (invariant I8), never snapshot
  // pointers.
  AcceptInfo accepted{};

  // Worker-owned, updated on reload. Borrow only during a request lookup.
  const ConfigSnapshot *snapshot{nullptr};
  std::span<char> route_scratch{};
  const StaticRoots *static_roots{};
  IdleUpstreams *idle_upstreams{nullptr};
  AccessLogRing *access_log{nullptr}; // optional, this worker is sole producer
};

} // namespace Coring5
