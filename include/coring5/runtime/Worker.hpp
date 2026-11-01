#pragma once

// One thread-per-core worker. Constructed AND run on its own thread (invariant
// I1: IORING_SETUP_SINGLE_ISSUER), see WorkerGroup.
//
// Member declaration order is the destruction order contract:
//   ring_ -> buffers_ -> timers_ -> connections_ ...
// destroyed in REVERSE, so connections (whose TimerNodes unlink from the wheel
// and whose FdOwners close fds) die before the wheel, buffers and ring.

#include "coring5/base/Types.hpp"
#include "coring5/config/Snapshot.hpp"
#include "coring5/io/BufferRing.hpp"
#include "coring5/io/Connection.hpp"
#include "coring5/io/IoContext.hpp"
#include "coring5/io/Ring.hpp"
#include "coring5/io/TimerWheel.hpp"
#include "coring5/mem/HandlePool.hpp"
#include "coring5/obs/Metrics.hpp"
#include "coring5/runtime/Mailbox.hpp"
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
};

class Worker {
public:
  explicit Worker(WorkerInit init);
  ~Worker();
  Worker(const Worker &) = delete;
  Worker &operator=(const Worker &) = delete;

  // Pin -> ring -> buffer ring -> pools. Must run on the worker thread.
  [[nodiscard]] std::expected<void, std::string> init();
  // Runs until a Shutdown message has been fully processed.
  void run();

private:
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
  void reap_closed();
  void check_shutdown_progress();
  [[nodiscard]] Handle find_listener(ListenerId id);

  WorkerInit init_;
  std::unique_ptr<Ring> ring_;
  std::unique_ptr<BufferRing> buffers_;
  std::unique_ptr<TimerWheel> timers_;
  std::shared_ptr<const ConfigSnapshot> snapshot_;
  std::unique_ptr<IoContext> ctx_;
  std::unique_ptr<HandlePool<ListenerSlot>> listeners_;
  std::unique_ptr<HandlePool<Connection, kConnectionSlotBytes>> connections_;

  __kernel_timespec tick_ts_{};
  std::vector<Message> inbox_;
  Phase phase_{Phase::Running};
  bool done_{false};
  bool tick_armed_{false};    // re-armed from the loop if the SQ was full
  bool mailbox_armed_{false};
  TimePoint drain_deadline_{};
  bool initialized_{false};
};

} // namespace Coring5
