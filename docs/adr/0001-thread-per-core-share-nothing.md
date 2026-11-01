# ADR 0001 — Thread-per-core, share-nothing workers

## Context
Coring4 mixed concerns inside one `Engine`; cross-thread edges were implicit.
The goal is predictable tail latency and linear scaling with cores.

## Decision
* N workers, each pinned, each owning one `io_uring`, one buffer slab, its own
  connection/listener pools, timer wheel, metric shard.
* Cross-thread edges are explicit and few: `Mailbox` (control→worker),
  `ReloadAck` (worker→control), metric shards (single writer, relaxed atomics).
* Everything that can fail (parse, validate, bind, later DNS/cert loading) runs
  on the control thread *before* a worker is told anything.
* Each worker has its own `SO_REUSEPORT` listener socket (created on the control
  thread, handed over as an fd); the kernel balances accepts.
* The `Ring` is created on the worker thread (`SINGLE_ISSUER`).

## Consequences
* (+) No locks on the data path; data structures are single-threaded and simple.
* (+) Worker apply is infallible (see ADR 0004).
* (−) Load imbalance is possible (few heavy connections on one worker); measure
  it (BENCHMARKING §5) before adding work stealing/`MSG_RING`.
* (−) Slow work on the loop (TLS handshakes, cold file opens) shows up as tail
  latency; the loop-stall histogram decides whether a helper thread is needed.
* (−) `SO_REUSEPORT` drops a closed member's accept queue, so kept listeners
  are never closed/reopened on reload.
