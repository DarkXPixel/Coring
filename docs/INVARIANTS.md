# Invariants

These ten rules are the contract every module relies on. Most `io_uring` proxy
bugs are violations of one of them. For each: the rule, why it exists, how it
is enforced in code, and how it is tested.

| # | Rule (short) | Enforced by |
|---|---|---|
| I1 | A `Ring` lives and is used only on its worker thread | `Worker::init()` runs on the worker thread; `WorkerGroup` constructs `Worker` there; `SINGLE_ISSUER` makes the kernel reject violations |
| I2 | Every SQE is counted in-flight at prep time; only a CQE without `F_MORE` decrements | `Connection::counted()`, `Ring::prep` returns `bool`; `assert` in `handle_cqe` |
| I3 | fd closed / slot freed only when `Closing && inflight == 0` | `Connection::finish()`, `Worker::reap_closed()` |
| I4 | `user_data = gen:32 \| index:20 \| kind:6 \| aux:6`, checked on every lookup; `stale_events == 0` | `OpTag`, `HandlePool::get`, `WorkerMetrics::stale_events` asserted in tests |
| I5 | Applying config on a worker is infallible | `ConfigManager` does all fallible work first; `Worker::apply` only swaps and arms |
| I6 | Every byte stream has a bounded queue with watermarks | `limits.queue_*_watermark`; relay design in ARCHITECTURE §7 (M3) |
| I7 | No heap allocation on the data path; shed load, never `terminate()` | Fixed-capacity pools, `conns_rejected`, SQ-full handling |
| I8 | Connections store stable ids, never pointers into a snapshot; old snapshots die on the control thread | `ListenerId`, `ReloadAck`, `ApplyConfig::snapshot.reset()` before ack |
| I9 | Every wait has a deadline | `TimerWheel`, `DeadlineKind`, limits table |
| I10 | Never hold a provided buffer across an idle period | `BufferRing::Lease` is for one recv→process→send step |

---

## I1 — Ring thread affinity

`IORING_SETUP_SINGLE_ISSUER` binds the ring to the task that created it. The
Coring4 `Worker::create()` was a static factory returning by value, so a ring
created in `main` and used on another thread would fail with `-EEXIST`.
In Coring5 the `Worker` object is constructed inside the thread lambda
(`WorkerGroup::start`) and `init()` creates the ring there.

## I2 — In-flight accounting

A connection may be destroyed only after the kernel can no longer deliver a CQE
that refers to it. So *every* submission that produces a CQE must be counted,
at the moment it is queued (not when it is submitted):

* **Cancel SQEs have their own CQE** (`OpKind::Cancel`) — counted.
* **Linked timeouts** (`link_timeout`) cost a second SQE *and* a second CQE —
  both counted.
* **Multishot** ops (accept, poll, multishot recv) keep producing CQEs with
  `IORING_CQE_F_MORE`; the count drops only on the CQE *without* it.
* **`SEND_ZC`** produces a result CQE (`F_MORE`) then an `F_NOTIF` CQE; the
  buffer and slot live until the second.
* A submission that could not be queued (SQ full after a forced submit) returns
  `false` and must **not** be counted. `counted(bool queued)` encodes this.

```cpp
if (!counted(ctx.ring.recv_provided(fd(), bgid, tag(OpKind::Recv))))
  close(ctx, Errc::Overloaded);          // shed this connection, never terminate
```

## I3 — Close sequence

```
close(why):  state = Closing; cancel timer
             if inflight == 0            → finish()
             else cancel_fd (counted)    → (if SQ full: shutdown(2) forces completions)
each CQE:    --inflight (unless F_MORE); in Closing, on_cqe() is NOT called
             if Closing && inflight == 0 → finish()
finish():    close(fd); state = Closed; ctx.mark_closed(handle)
reap:        Worker destroys Closed connections after the CQE batch, never from
             inside their own callback
```

Closing the fd only at the end also prevents fd-number reuse from hitting a
later `cancel_fd` meant for a different socket.

## I4 — Handles and the generation tripwire

`HandlePool` handles are `{index, gen}`; `gen == 0` is invalid; `gen` is a
plain 32-bit counter skipping 0 on wrap (once per 2^32 reuses of one slot).

**What `gen` is for.** For a connection's *own* in-flight ops, I3 already
guarantees no stale CQE can exist; `gen` is a free tripwire that turns a
violation (bytes delivered to the wrong client) into a counted, harmless event:

```cpp
Connection *c = connections_->get(cqe.tag.handle());
if (c == nullptr) inc(metrics.stale_events);   // must be 0 forever
```

**Where `gen` is real logic**, not just a tripwire: peer links between
connections (client ↔ upstream — the peer can die at any time), mailbox/ack
messages that cross threads or loop iterations, a future QUIC CID → connection
map. Stale hits there are normal and must be handled, not counted as bugs.

Why 32 bits and no quarantine: with a 16-bit generation a hot slot in a
connection-churn benchmark wraps within minutes; at 32 bits it takes days to
years depending on load, and I3 means a stale reference to the *same* slot
cannot survive anyway. `HandlePool::debug_set_generation` exists so tests can
exercise the wrap (`GenerationNeverBecomesZeroOnWrap`).

Tests/CI must assert `stale_events == 0` (integration tests do).

## I5 — Infallible apply

`ConfigManager::apply` (control thread): validate → reject runtime changes →
assign ids → **bind new sockets** → build snapshot → post → wait for acks →
retire old snapshot. Anything that can fail is before the post. The worker side
swaps a `shared_ptr`, arms accepts on already-open fds, and cancels removed
listeners. The one worker-side failure left is "listener pool full"
(1024 per worker), reported through `ReloadAck::failures()`.

## I6 — Bounded queues

See ARCHITECTURE §7. A direction pauses its source above the high watermark
and resumes below the low watermark; data that arrives after the pause decision
is still accepted; the queue itself is bounded by pool-backed storage, so it
cannot grow without limit.

## I7 — No data-path allocation, no terminate

* Pools are fixed-capacity; exhaustion ⇒ `conns_rejected++` and close the fd.
* SQ exhaustion ⇒ `Ring::prep_*` forces one submit and retries; if still full it
  returns `false`; callers shed the connection. The worker loop re-arms its own
  tick/mailbox ops on the next iteration instead of aborting.
* `std::terminate()` is reserved for an unrecoverable `io_uring_enter` error in
  `Worker::run`; the supervisor restarts the process.

## I8 — Snapshots

* Connections keep `ListenerId`/`RouteBinding`/`UpstreamId`, and look things up
  per request, so keep-alive connections see new config on their next request.
* A worker drops its reference to the old snapshot **before** acking
  (`msg.snapshot.reset(); ack->complete()`), and `ConfigManager` holds the last
  reference until all acks arrive, so the (possibly large) old snapshot is
  destroyed on the control thread, never in a worker's hot loop.

## I9 — Deadlines

Every wait has a `DeadlineKind` and a configured limit: idle, header read, body
idle, upstream connect, upstream first byte, write stall, keep-alive, max
lifetime, drain. The timer wheel is one coarse wheel per worker (resolution
`runtime.tick_ms`); only `connect` uses a `LINK_TIMEOUT`. `EchoConnection`
implements the idle deadline.

## I10 — No buffers parked on idle connections

Provided buffers are chosen by the kernel when data arrives, which is why
100k idle connections cost no receive buffers. Parking a lease on an idle
connection would defeat that and starve the ring. A partial request head is
copied into a bounded per-connection staging area and the lease released.

---

## Review checklist for any new `Connection` subclass

- [ ] Every ring submission is wrapped in `counted()`, and failure to queue closes the connection.
- [ ] No submission when `closing()`.
- [ ] No `delete this`, no pool destroy from inside a callback.
- [ ] Other connections referenced only by `Handle`; a stale lookup is handled.
- [ ] Leases are released (or moved into a bounded staging buffer) before going idle.
- [ ] Every wait arms a deadline; `on_timer` handles it.
- [ ] `on_drain` finishes the in-progress unit of work and then closes.
- [ ] A `FakeRing`-style test closes it at every state (see TESTING.md).
