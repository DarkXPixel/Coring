#include "coring5/runtime/Worker.hpp"

#include "coring5/proto/l4/EchoConnection.hpp"
#include <cerrno>
#include <csignal>
#include <format>
#include <pthread.h>
#include <sched.h>

namespace Coring5 {

namespace {

constexpr uint32_t kMaxListenersPerWorker = 1024;
constexpr Millis kHardStopGrace{5000}; // after the drain deadline

bool pin_to_cpu(int cpu) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(static_cast<std::size_t>(cpu), &set);
  return ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set) == 0;
}

void block_all_signals() {
  sigset_t all;
  sigfillset(&all);
  ::pthread_sigmask(SIG_BLOCK, &all, nullptr); // handled by the main thread
}

__kernel_timespec to_timespec(Millis ms) {
  __kernel_timespec ts{};
  ts.tv_sec = ms.count() / 1000;
  ts.tv_nsec = (ms.count() % 1000) * 1'000'000;
  return ts;
}

} // namespace

Worker::Worker(WorkerInit init) : init_(std::move(init)) {}

Worker::~Worker() {
  // Explicit order: connections (fds, timer nodes) first, ring last.
  connections_.reset();
  listeners_.reset();
  ctx_.reset();
  timers_.reset();
  buffers_.reset();
  ring_.reset();
}

std::expected<void, std::string> Worker::init() {
  if (init_.mailbox == nullptr || init_.metrics == nullptr ||
      !init_.mailbox->valid()) {
    return std::unexpected("worker: mailbox/metrics not provided");
  }
  block_all_signals();
  // 1. pin BEFORE any allocation so pages are first-touched on this core/node.
  if (init_.cpu >= 0 && !pin_to_cpu(init_.cpu)) {
    return std::unexpected(std::format("worker {}: cannot pin to cpu {}",
                                       init_.id.value, init_.cpu));
  }

  // 2. ring on the thread that will use it (I1).
  RingConfig rc;
  rc.sq_entries = init_.runtime.ring_sq_entries;
  rc.cq_entries = init_.runtime.ring_cq_entries;
  auto ring = Ring::create(rc);
  if (!ring) {
    return std::unexpected(std::format(
        "worker {}: io_uring setup failed ({}, errno {}). Blocked by seccomp "
        "or "
        "kernel.io_uring_disabled? Kernel >= 6.1 required.",
        init_.id.value, to_string(ring.error().code), ring.error().sys));
  }
  ring_ = std::move(*ring);

  auto bufs = BufferRing::create(*ring_, 0, init_.runtime.buffer_count,
                                 init_.runtime.buffer_size);
  if (!bufs) {
    return std::unexpected(std::format(
        "worker {}: buffer ring failed ({}, errno {})", init_.id.value,
        to_string(bufs.error().code), bufs.error().sys));
  }
  buffers_ = std::make_unique<BufferRing>(std::move(*bufs));

  timers_ = std::make_unique<TimerWheel>(
      Clock::now(), std::chrono::duration_cast<Nanos>(init_.runtime.tick),
      1024);

  snapshot_ =
      std::make_shared<const ConfigSnapshot>(); // defaults until first apply
  ctx_ = std::make_unique<IoContext>(IoContext{*ring_,
                                               *buffers_,
                                               *timers_,
                                               *init_.metrics,
                                               &snapshot_->limits,
                                               Clock::now(),
                                               {}});
  ctx_->closed.reserve(init_.runtime.max_connections_per_worker);

  listeners_ =
      std::make_unique<HandlePool<ListenerSlot>>(kMaxListenersPerWorker);
  connections_ = std::make_unique<HandlePool<Connection, kConnectionSlotBytes>>(
      init_.runtime.max_connections_per_worker);

  tick_ts_ = to_timespec(init_.runtime.tick);
  arm_mailbox();
  arm_tick();
  if (!mailbox_armed_ || !tick_armed_) {
    return std::unexpected(
        std::format("worker {}: cannot queue initial ring operations", init_.id.value));
  }
  set(init_.metrics->buffers_free, buffers_->free_count());
  initialized_ = true;
  return {};
}

void Worker::arm_mailbox() {
  mailbox_armed_ = ring_->poll_multishot(init_.mailbox->event_fd(),
                                         OpTag::make(OpKind::Mailbox, 0, 0));
}

void Worker::arm_tick() {
  // If the SQ is full this stays false and run() retries on the next iteration;
  // the worker never terminates because of SQ exhaustion (invariant I7).
  tick_armed_ = ring_->timeout(&tick_ts_, OpTag::make(OpKind::Tick, 0, 0));
}

void Worker::run() {
  if (!initialized_) {
    return;
  }
  while (!done_) {
    auto r = ring_->submit_and_wait(1);
    if (!r) {
      if (r.error().sys == EINTR || r.error().sys == EBUSY ||
          r.error().sys == EAGAIN) {
        continue;
      }
      std::terminate(); // unrecoverable ring failure: supervisor restarts us
    }

    const auto t0 = Clock::now();
    ctx_->now = t0;

    const unsigned n = ring_->reap([this](const Cqe &c) { dispatch(c); });
    inc(init_.metrics->cqes, n);

    timers_->advance(t0, [this](TimerNode &node) {
      if (Connection *c = connections_->get(node.owner)) {
        c->handle_timer(*ctx_, node.kind);
      } else {
        inc(init_.metrics->stale_events); // would mean a leaked TimerNode
      }
    });

    if (!tick_armed_) {
      arm_tick();
    }
    if (!mailbox_armed_) {
      arm_mailbox();
    }
    reap_closed();
    buffers_->publish(); // one tail store per iteration
    check_shutdown_progress();

    inc(init_.metrics->loop_iterations);
    set(init_.metrics->buffers_free, buffers_->free_count());
    set(init_.metrics->sq_full, ring_->sq_full_events());
    if (ring_->cq_overflowed()) {
      inc(init_.metrics->cq_overflow);
    }
    init_.metrics->loop_ns.record(static_cast<uint64_t>(
        std::chrono::duration_cast<Nanos>(Clock::now() - t0).count()));
  }
}

// ---------------------------------------------------------------- dispatch

void Worker::dispatch(const Cqe &cqe) {
  switch (cqe.tag.kind()) {
  case OpKind::Accept:
    on_accept(cqe);
    return;
  case OpKind::Mailbox:
    on_mailbox(cqe);
    return;
  case OpKind::Tick:
    on_tick();
    return;
  case OpKind::Cancel:
    if (cqe.tag.aux() == kAuxWorker) {
      on_listener_cancel(cqe);
      return;
    }
    [[fallthrough]];
  default:
    on_connection_cqe(cqe);
    return;
  }
}

void Worker::on_connection_cqe(const Cqe &cqe) {
  // The Lease recycles the provided buffer on every path out of this function.
  BufferRing::Lease lease;
  if (cqe.has_buffer()) {
    lease = buffers_->take(cqe.buffer_id(),
                           cqe.res > 0 ? static_cast<uint32_t>(cqe.res) : 0U);
  }
  Connection *c = connections_->get(cqe.tag.handle());
  if (c == nullptr) [[unlikely]] {
    inc(init_.metrics->stale_events); // invariant I3/I4 violated: a bug
    return;
  }
  c->handle_cqe(*ctx_, cqe, lease);
}

void Worker::on_accept(const Cqe &cqe) {
  const Handle lh = cqe.tag.handle();
  ListenerSlot *slot = listeners_->get(lh);
  if (slot == nullptr) [[unlikely]] {
    inc(init_.metrics->stale_events);
    if (cqe.res >= 0) {
      ::close(cqe.res);
    }
    return;
  }

  if (cqe.res >= 0) {
    FdOwner fd(cqe.res);
    const ListenerSpec *spec = snapshot_->find(slot->id);
    if (phase_ != Phase::Running || slot->removing || spec == nullptr) {
      // fd closes via FdOwner: draining or listener already removed.
    } else {
      inc(init_.metrics->accepts);
      switch (spec->protocol) {
      case ProtocolKind::Echo: {
        auto [h, conn] = connections_->create<EchoConnection>(std::move(fd));
        if (conn == nullptr) {
          inc(init_.metrics->conns_rejected); // pool full: shed load (I7)
          break;
        }
        conn->bind(h);
        set(init_.metrics->conns_live, connections_->live());
        conn->start(*ctx_);
        break;
      }
      default:
        break; // not implemented yet: fd closes
      }
    }
  } else if (cqe.res != -ECANCELED) {
    inc(init_.metrics->accept_errors); // e.g. -EMFILE: back off, retry on tick
  }

  if (!cqe.more()) {
    slot->accept_armed = false;
    --slot->pending_ops;
    if (!slot->removing && phase_ == Phase::Running) {
      slot->rearm = true; // never re-arm inline: -EMFILE would spin
    }
    maybe_free_listener(lh, *slot);
  }
}

void Worker::on_listener_cancel(const Cqe &cqe) {
  const Handle lh = cqe.tag.handle();
  ListenerSlot *slot = listeners_->get(lh);
  if (slot == nullptr) [[unlikely]] {
    inc(init_.metrics->stale_events);
    return;
  }
  --slot->pending_ops; // -ENOENT/-EALREADY are fine: accept had already ended
  maybe_free_listener(lh, *slot);
}

void Worker::on_mailbox(const Cqe &cqe) {
  if (cqe.res >= 0) {
    inbox_.clear();
    init_.mailbox->drain(inbox_);
    for (auto &m : inbox_) {
      if (auto *a = std::get_if<ApplyConfig>(&m)) {
        apply(*a);
      } else if (auto *s = std::get_if<Shutdown>(&m)) {
        begin_shutdown(*s);
      }
    }
    inbox_.clear();
  }
  if (!cqe.more()) {
    mailbox_armed_ = false;
    arm_mailbox(); // multishot poll ended: re-arm (retried by run() if SQ full)
  }
}

void Worker::on_tick() {
  tick_armed_ = false;
  // Retry listeners whose accept ended on an error (-EMFILE etc.).
  listeners_->for_each([this](Handle h, ListenerSlot &s) {
    if (s.rearm && !s.removing && phase_ == Phase::Running) {
      s.rearm = false;
      arm_accept(h, s);
    }
  });
  arm_tick();
}

// ----------------------------------------------------------------- control

void Worker::apply(ApplyConfig &msg) {
  bool ok = (phase_ == Phase::Running);
  if (ok) {
    snapshot_ = msg.snapshot; // drops our ref to the old snapshot
    ctx_->limits = &snapshot_->limits;
    set(init_.metrics->config_generation, snapshot_->generation);

    for (auto &id : msg.remove) {
      if (Handle h = find_listener(id); h.valid()) {
        remove_listener(h, *listeners_->get(h));
      }
    }
    for (auto &lf : msg.add) {
      auto [h, slot] =
          listeners_->create<ListenerSlot>(lf.id, std::move(lf.fd));
      if (slot == nullptr) {
        ok = false;
        continue;
      }
      arm_accept(h, *slot);
    }
  }
  msg.snapshot.reset(); // release BEFORE acking: retirement happens on control
  if (msg.ack) {
    msg.ack->complete(ok);
  }
}

void Worker::begin_shutdown(const Shutdown &msg) {
  if (phase_ != Phase::Running) {
    return;
  }
  phase_ = Phase::Draining;
  drain_deadline_ = ctx_->now + msg.drain_timeout;
  listeners_->for_each(
      [this](Handle h, ListenerSlot &s) { remove_listener(h, s); });
  connections_->for_each(
      [this](Handle, Connection &c) { c.begin_drain(*ctx_); });
}

// ----------------------------------------------------------------- helpers

Handle Worker::find_listener(ListenerId id) {
  Handle found{};
  listeners_->for_each([&](Handle h, ListenerSlot &s) {
    if (s.id == id) {
      found = h;
    }
  });
  return found;
}

void Worker::arm_accept(Handle h, ListenerSlot &slot) {
  if (ring_->accept_multishot(slot.fd.get(), OpTag::make(OpKind::Accept, h))) {
    slot.accept_armed = true;
    ++slot.pending_ops;
  } else {
    slot.rearm = true; // SQ full: retry on the next tick
  }
}

void Worker::remove_listener(Handle h, ListenerSlot &slot) {
  if (slot.removing) {
    return;
  }
  slot.removing = true;
  slot.rearm = false;
  if (slot.accept_armed) {
    // The cancel has its own CQE and counts as pending (same rule as I2).
    if (ring_->cancel_fd(slot.fd.get(),
                         OpTag::make(OpKind::Cancel, h, kAuxWorker))) {
      ++slot.pending_ops;
    } else {
      ::shutdown(slot.fd.get(), SHUT_RD); // makes the pending accept fail
    }
  }
  maybe_free_listener(h, slot);
}

void Worker::maybe_free_listener(Handle h, ListenerSlot &slot) {
  if (slot.removing && slot.pending_ops == 0) {
    listeners_->destroy(h); // FdOwner closes the listening socket here
  }
}

void Worker::reap_closed() {
  if (ctx_->closed.empty()) {
    return;
  }
  for (Handle h : ctx_->closed) {
    connections_->destroy(h);
  }
  ctx_->closed.clear();
  set(init_.metrics->conns_live, connections_->live());
}

void Worker::check_shutdown_progress() {
  if (phase_ == Phase::Running) {
    return;
  }
  const auto now = ctx_->now;
  if (phase_ == Phase::Draining && now >= drain_deadline_) {
    phase_ = Phase::Stopping;
    connections_->for_each(
        [this](Handle, Connection &c) { c.close(*ctx_, Errc::Timeout); });
  }
  if (connections_->live() == 0 && listeners_->live() == 0) {
    done_ = true;
  } else if (phase_ == Phase::Stopping &&
             now >= drain_deadline_ + kHardStopGrace) {
    done_ = true; // cancels did not complete in time: leave it to ring teardown
  }
}

} // namespace Coring5
