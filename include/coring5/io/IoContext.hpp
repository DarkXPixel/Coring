#pragma once

// Everything a connection may touch on its worker. Owned by the Worker and
// passed by reference into every callback. This is a plain struct of references
// (not an interface): no virtual dispatch on the hot path, and protocol modules
// depend on `io/` instead of on `runtime/` (see docs/ARCHITECTURE.md layering).

#include "coring5/base/Types.hpp"
#include "coring5/config/Snapshot.hpp"
#include "coring5/io/BufferRing.hpp"
#include "coring5/io/Ring.hpp"
#include "coring5/io/TimerWheel.hpp"
#include "coring5/mem/ChunkPool.hpp"
#include "coring5/obs/Metrics.hpp"
#include <vector>

namespace Coring5 {

class UpstreamPool; // upstream/UpstreamPool.hpp (kept out of io's include
                    // graph)

struct IoContext {
  Ring &ring;
  BufferRing &buffers;
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

  // Set by the worker immediately before ConnPool::spawn(); read once by the
  // new connection's start(). Plain ids (invariant I8), never snapshot
  // pointers.
  AcceptInfo accepted{};
};

} // namespace Coring5
