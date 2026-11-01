#pragma once

// Control thread -> worker message channel. Cold path by design: a mutex-
// protected vector plus an eventfd the worker polls through its own ring.
// (Data-plane cross-worker handoff, if ever needed, should use
// IORING_OP_MSG_RING instead; see docs/ARCHITECTURE.md.)

#include "coring5/base/Ack.hpp"
#include "coring5/base/Types.hpp"
#include "coring5/config/Snapshot.hpp"
#include "coring5/io/FdOwner.hpp"
#include <memory>
#include <mutex>
#include <variant>
#include <vector>

namespace Coring5 {

struct ListenerFd {
  ListenerId id;
  FdOwner fd; // this worker's own SO_REUSEPORT socket for the listener
};

// Infallible on the worker (invariant I5): everything that can fail (parse,
// validate, bind) was done by the control thread before this was posted.
struct ApplyConfig {
  std::shared_ptr<const ConfigSnapshot> snapshot;
  std::vector<ListenerFd> add;
  std::vector<ListenerId> remove;
  std::shared_ptr<ReloadAck> ack; // may be null
};

struct Shutdown {
  Millis drain_timeout{10000};
};

using Message = std::variant<ApplyConfig, Shutdown>;

class Mailbox {
public:
  Mailbox();
  Mailbox(const Mailbox &) = delete;
  Mailbox &operator=(const Mailbox &) = delete;
  ~Mailbox();

  [[nodiscard]] bool valid() const noexcept { return efd_.valid(); }
  [[nodiscard]] int event_fd() const noexcept { return efd_.get(); }

  // Any thread.
  void post(Message m);
  // Worker thread: resets the eventfd and moves out all pending messages.
  void drain(std::vector<Message> &out);

private:
  FdOwner efd_;
  std::mutex mtx_;
  std::vector<Message> queue_;
};

} // namespace Coring5
