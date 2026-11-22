#include "coring5/runtime/Worker.hpp"
#include "coring5/runtime/ExchangeDelivery.hpp"

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
  exchanges_.reset(); // returns body chunks: before chunks_/stage_ go away
  upstreams_.reset();
  stage_ = ChunkPool{};
  chunks_ = ChunkPool{};
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

  // Chunk memory (chunk = buffer_size): one lazily-faulted slab per worker,
  // only when an l4 or http1 pool exists. Serves l4 relay queues and HTTP/1
  // body streams. Bounded by construction (queues cannot exceed it).
  upstreams_ = std::make_unique<UpstreamPool>();
  const uint32_t http1_cap = init_.runtime.pool_capacity[pool_index(PoolId::Http1)];
  if (init_.runtime.pool_capacity[pool_index(PoolId::L4)] > 0 || http1_cap > 0) {
    const uint64_t n =
        (static_cast<uint64_t>(init_.runtime.relay_memory_mib) << 20) /
        init_.runtime.buffer_size;
    auto cp =
        ChunkPool::create(init_.runtime.buffer_size, static_cast<uint32_t>(n));
    if (!cp) {
      return std::unexpected(std::format(
          "worker {}: relay chunk pool failed ({}, errno {})", init_.id.value,
          to_string(cp.error().code), cp.error().sys));
    }
    chunks_ = std::move(*cp);
  }
  // HTTP/1: one stage chunk per connection (sessions allocate lazily; the slab
  // is mapped noreserve) and one Exchange per connection (serial sessions).
  if (http1_cap > 0) {
    const uint32_t stage_bytes = init_.runtime.http1_stage_effective != 0
                                     ? init_.runtime.http1_stage_effective
                                     : init_.runtime.buffer_size;
    auto sp = ChunkPool::create(stage_bytes, http1_cap);
    if (!sp) {
      return std::unexpected(std::format(
          "worker {}: http1 stage pool failed ({}, errno {})", init_.id.value,
          to_string(sp.error().code), sp.error().sys));
    }
    stage_ = std::move(*sp);
  }
  exchanges_ = std::make_unique<ExchangePool>(
      http1_cap, chunks_, StreamLimits::from(snapshot_->limits));
  ctx_->upstreams = upstreams_.get();
  ctx_->chunks = &chunks_;
  ctx_->exchanges = exchanges_.get();
  ctx_->stage = &stage_;

  listeners_ =
      std::make_unique<HandlePool<ListenerSlot>>(kMaxListenersPerWorker);
  // One pool per connection type, sized from the loaded configuration. A
  // protocol without listeners gets capacity 0 and costs no memory.
  connections_ = std::make_unique<ConnPools>(init_.runtime.pool_capacity);
  ctx_->closed.reserve(connections_->total_capacity());

  tick_ts_ = to_timespec(init_.runtime.tick);
  arm_mailbox();
  arm_tick();
  if (!mailbox_armed_ || !tick_armed_) {
    return std::unexpected(std::format(
        "worker {}: cannot queue initial ring operations", init_.id.value));
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
    // Ready exchanges mean there is work that no CQE will announce: never
    // sleep in the kernel then (submit without waiting).
    auto r = exchanges_->has_ready() ? ring_->submit_nowait()
                                     : ring_->submit_and_wait(1);
    if (!r) {
      const int err = r.error().sys;
      if (err != EINTR && err != EBUSY && err != EAGAIN) {
        std::terminate(); // unrecoverable ring failure: supervisor restarts us
      }
      // Retryable: do NOT skip the rest of the iteration. EBUSY means the
      // kernel could not flush CQ-overflow entries or accept more work until
      // the application reaps; skipping the reap below would livelock. Timers
      // and housekeeping must also keep running after EINTR. Unsubmitted SQEs
      // stay queued and go out with the next submit.
      ring_->flush_events(); // make overflowed CQEs visible to the reap below
    }

    const auto t0 = Clock::now();
    ctx_->now = t0;

    const unsigned n = ring_->reap([this](const Cqe &c) { dispatch(c); });
    inc(init_.metrics->cqes, n);

    timers_->advance(t0, [this](TimerNode &node) {
      if (!connections_->on_timer(*ctx_, node.pool, node.owner, node.kind)) {
        inc(init_.metrics->stale_events); // would mean a leaked TimerNode
      }
    });

    drain_exchanges(); // before reap_closed(): may close connections

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
    set(init_.metrics->relay_chunks_in_use, chunks_.in_use());
    set(init_.metrics->relay_chunks_peak, chunks_.peak_in_use());
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
  case OpKind::Nop:
    return; // placeholder SQE (see Ring::connect_with_timeout): no owner
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
  if (!connections_->on_cqe(*ctx_, cqe, lease)) [[unlikely]] {
    inc(init_.metrics->stale_events); // invariant I3/I4 violated: a bug
  }
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
      const PoolId pool = pool_for(spec->protocol);
      const auto p = pool_index(pool);
      inc(init_.metrics->pool_accepts[p]);
      // Static dispatch inside ConnPools: no virtual call, no per-type branch
      // here. A full pool (or one sized 0) sheds load: the fd closes (I7).
      ctx_->accepted = AcceptInfo{spec->id, spec->upstream_id};
      if (!connections_->spawn(pool, *ctx_, fd)) {
        inc(init_.metrics->conns_rejected);
        inc(init_.metrics->pool_rejected[p]);
      }
      publish_pool_gauges();
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
    exchanges_->set_limits(StreamLimits::from(snapshot_->limits));
    upstreams_->apply_config(
        *snapshot_); // copies: no pointer into the snapshot
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
  connections_->begin_drain_all(*ctx_);
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

void Worker::drain_exchanges() {
  // Bounded per iteration so a busy pool cannot starve the ring; leftovers keep
  // has_ready() true and the next submit does not block.
  constexpr uint32_t kBudget = 1024;
  exchanges_->drain(kBudget, [this](Handle, Exchange &ex) {
    deliver_exchange_ingress(ex, [this](ConnRef ingress) {
      return connections_->on_exchange(*ctx_, ingress);
    });
  });
}

void Worker::reap_closed() {
  if (ctx_->closed.empty()) {
    return;
  }
  for (const ConnRef c : ctx_->closed) {
    connections_->destroy(c);
  }
  ctx_->closed.clear();
  publish_pool_gauges();
}

void Worker::publish_pool_gauges() {
  for (std::size_t p = 0; p < kPoolCount; ++p) {
    set(init_.metrics->pool_live[p],
        connections_->live(static_cast<PoolId>(p)));
  }
  set(init_.metrics->relay_chunks_in_use, chunks_.in_use());
  set(init_.metrics->conns_live, connections_->live());
}

void Worker::check_shutdown_progress() {
  if (phase_ == Phase::Running) {
    return;
  }
  const auto now = ctx_->now;
  if (phase_ == Phase::Draining && now >= drain_deadline_) {
    phase_ = Phase::Stopping;
    connections_->close_all(*ctx_, Errc::Timeout);
  }
  if (connections_->live() == 0 && listeners_->live() == 0 &&
      !exchanges_->has_ready() && exchanges_->live() == 0) {
    done_ = true;
  } else if (phase_ == Phase::Stopping &&
             now >= drain_deadline_ + kHardStopGrace) {
    done_ = true; // cancels did not complete in time: leave it to ring teardown
  }
}

} // namespace Coring5
