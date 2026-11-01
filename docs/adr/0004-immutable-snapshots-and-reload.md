# ADR 0004 — Immutable snapshots, infallible worker apply, retirement on control

## Context
Hot reload must be zero-downtime and must not leave workers in different,
half-applied states. Coring4's `RoutingControl`/`WorkerRouting` already had the
right shape for routes (compile once, share `shared_ptr<const RouteTable>`,
per-worker pointer swap) but `WorkerRouting::install` could still fail on the
worker.

## Decision
* `ConfigSnapshot` is immutable after publication, shared as
  `shared_ptr<const ConfigSnapshot>`.
* `[runtime]` is fixed for the process lifetime; a reload that changes it is
  rejected ("restart required"). `[limits]` and `[[listener]]` reload.
* `ConfigManager::apply`: validate → reject runtime change → assign stable
  `ListenerId`s (same name+address ⇒ same id) → diff → **bind new sockets**
  (one per worker) → post `ApplyConfig` → wait for acks → retire the old
  snapshot on the control thread.
* Worker apply only swaps the pointer, arms accepts on already-open fds and
  cancels removed listeners. It drops its own snapshot reference *before*
  acking.
* Connections store ids (`ListenerId`, later `RouteBinding`, `UpstreamId`),
  never pointers into a snapshot.
* Kept listeners are never closed/reopened (avoids `SO_REUSEPORT` accept-queue
  loss).

## Consequences
* (+) A bad config cannot partially apply; the only worker-side failure is the
  listener-pool limit.
* (+) Large old snapshots are never destroyed on a hot loop.
* (−) Updates are eventually consistent across workers (short window with
  mixed generations); documented, acceptable for a proxy.
* (−) The control thread holds the manager mutex while waiting for acks, so a
  stalled worker blocks the next reload until `reload_ack_timeout_ms`.
* Follow-up (M4): fold the route table into the snapshot and validate bound
  route sets against the whole snapshot on the control thread so
  `WorkerRouting::install` cannot fail on a worker.
