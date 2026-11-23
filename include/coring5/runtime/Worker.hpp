#pragma once

// One thread-per-core worker. Constructed AND run on its own thread (invariant
// I1: IORING_SETUP_SINGLE_ISSUER), see WorkerGroup.
//
// Connections live in per-type pools (runtime/ConnPools.hpp, sizes from the
// loaded configuration); there is no polymorphic Connection base class.
//
// Member declaration order is the destruction order contract:
//   ring_ -> buffers_ -> timers_ -> connections_ ...
// destroyed in REVERSE, so connections (whose TimerNodes unlink from the wheel
// and whose FdOwners close fds) die before the wheel, buffers and ring.

#include "coring5/base/Types.hpp"
#include "coring5/config/Snapshot.hpp"
#include "coring5/io/IoContext.hpp"
#include "coring5/io/RingSeam.hpp"
#include "coring5/http/ExchangePool.hpp"
#include "coring5/io/TimerWheel.hpp"
#include "coring5/mem/ChunkPool.hpp"
#include "coring5/mem/HandlePool.hpp"
#include "coring5/net/IdleUpstreams.hpp"
#include "coring5/obs/AccessLog.hpp"
#include "coring5/obs/Metrics.hpp"
#include "coring5/runtime/ConnPools.hpp"
#include "coring5/runtime/Mailbox.hpp"
#include "coring5/upstream/UpstreamPool.hpp"
#include <expected>
#include <memory>
#include <string>
#include <vector>

namespace Coring5 {

struct WorkerInit {
  WorkerId id;
  RuntimeConfig runtime;
  int cpu{-1}; // -1: do not pin
  Mailbox *mailbox{nullptr};
  WorkerMetrics *metrics{nullptr};
  AccessLogRing *access_log{nullptr};
};

template <class RingT, class BufferRingT> class BasicWorker {
public:
  explicit BasicWorker(WorkerInit init);
  ~BasicWorker();
  BasicWorker(const BasicWorker &) = delete;
  BasicWorker &operator=(const BasicWorker &) = delete;

  // Pin -> ring -> buffer ring -> pools. Must run on the worker thread.
  [[nodiscard]] std::expected<void, std::string> init();
  // Runs until a Shutdown message has been fully processed.
  void run();

private:
  friend class WorkerRingTest;
  void run_once();

  enum class Phase : uint8_t { Running, Draining, Stopping };

  struct ListenerSlot {
    ListenerSlot(ListenerId i, FdOwner f) : id(i), fd(std::move(f)) {}
    ListenerId id;
    FdOwner fd;
    uint8_t pending_ops{0}; // accept (until a CQE without F_MORE) + cancel
    bool accept_armed{false};
    bool removing{false};
    bool rearm{false}; // accept ended with an error: re-arm on the next tick
  };

  // ---- completion dispatch ----
  void dispatch(const Cqe &cqe);
  void on_connection_cqe(const Cqe &cqe);
  void on_accept(const Cqe &cqe);
  void on_listener_cancel(const Cqe &cqe);
  void on_mailbox(const Cqe &cqe);
  void on_tick();

  // ---- control messages ----
  void apply(ApplyConfig &msg);
  void begin_shutdown(const Shutdown &msg);

  // ---- helpers ----
  void arm_accept(Handle h, ListenerSlot &slot);
  void remove_listener(Handle h, ListenerSlot &slot);
  void maybe_free_listener(Handle h, ListenerSlot &slot);
  void arm_tick();
  void arm_mailbox();
  void drain_exchanges();
  void reap_closed();
  void publish_pool_gauges();
  void check_shutdown_progress();
  [[nodiscard]] Handle find_listener(ListenerId id);

  WorkerInit init_;
  std::unique_ptr<RingT> ring_;
  std::unique_ptr<BufferRingT> buffers_;
  std::unique_ptr<TimerWheel> timers_;
  std::shared_ptr<const ConfigSnapshot> snapshot_;
  std::shared_ptr<const StaticRoots> static_roots_;
  std::unique_ptr<IoContext> ctx_;
  // l4 / http1 services. Declared BEFORE connections_ and destroyed AFTER it: a
  // connection's destructor returns its chunks / backend accounting / detaches
  // its exchange. `chunks_` (chunk = buffer_size) feeds relay queues and body
  // streams; `stage_` feeds HTTP/1 sessions. exchanges_ is destroyed before
  // both chunk pools (its streams return chunks).
  ChunkPool chunks_;
  ChunkPool stage_;
  std::unique_ptr<char[]> route_scratch_;
  std::unique_ptr<UpstreamPool> upstreams_;
  IdleUpstreams idle_upstreams_;
  std::unique_ptr<ExchangePool> exchanges_;
  std::unique_ptr<HandlePool<ListenerSlot>> listeners_;
  std::unique_ptr<ConnPools> connections_;

  __kernel_timespec tick_ts_{};
  std::vector<Message> inbox_;
  Phase phase_{Phase::Running};
  bool done_{false};
  bool tick_armed_{false}; // re-armed from the loop if the SQ was full
  bool mailbox_armed_{false};
  TimePoint drain_deadline_{};
  bool initialized_{false};
};

// Match the build-target seam used by IoContext and the protocol pools.
using Worker = BasicWorker<IoRing, IoBufferRing>;

} // namespace Coring5
