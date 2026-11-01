#pragma once

// Everything a Connection may touch on its worker. Owned by the Worker and
// passed by reference into every callback. This is a plain struct of references
// (not an interface): no virtual dispatch on the hot path, and protocol modules
// depend on `io/` instead of on `runtime/` (see docs/ARCHITECTURE.md layering).

#include "coring5/base/Types.hpp"
#include "coring5/config/Snapshot.hpp"
#include "coring5/io/BufferRing.hpp"
#include "coring5/io/Ring.hpp"
#include "coring5/io/TimerWheel.hpp"
#include "coring5/obs/Metrics.hpp"
#include <vector>

namespace Coring5 {

struct IoContext {
  Ring &ring;
  BufferRing &buffers;
  TimerWheel &timers;
  WorkerMetrics &metrics;
  const Limits *limits{nullptr}; // swapped by the worker on config apply
  TimePoint now{};               // refreshed once per loop iteration

  // Connections that reached Closed. The worker destroys them at a safe point
  // (end of the loop iteration), never from inside their own callback.
  std::vector<Handle> closed;

  void mark_closed(Handle h) { closed.push_back(h); }
};

} // namespace Coring5
