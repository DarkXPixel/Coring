# ADR 0002 — 32-bit generation handles and the `OpTag` layout

## Context
A completion must find its owner. Coring4 used `IoOp*` as `user_data` (no owner
or generation) in the new backend and a bit-field `UserData` with a 32-bit
generation in `Engine`. Bit-field layout is implementation-defined.

The first Coring5 sketch used a 16-bit generation. Analysis showed that wraps
within minutes under connection-churn benchmarks (a LIFO free list concentrates
reuse on a few hot slots: ~65k reuses at tens of thousands of accepts/s).

## Decision
* `Handle{uint32 index; uint32 gen}`, `gen == 0` is invalid, `gen` skips 0 on wrap.
* `OpTag` (64-bit, explicit shifts): `gen:32 | index:20 | kind:6 | aux:6`.
  Pool capacity ≤ 2^20.
* **`gen` is a tripwire, not the safety mechanism.** For a connection's own ops
  safety comes from ADR 0003 (no CQE can outlive the slot). A stale lookup
  returns `nullptr`, the worker increments `stale_events`, and tests/CI/prod
  treat non-zero as a bug.
* `gen` is real logic for references that legitimately outlive their target:
  client↔upstream peer links, mailbox/ack messages, a future QUIC CID map.
* No quarantine-on-wrap and no configurable generation width: at 32 bits a wrap
  takes days to years per slot, and ADR 0003 already removes stale own-op CQEs.

## Consequences
* (+) A lifecycle bug becomes a counter instead of bytes sent to the wrong
  client; the check costs one compare on a cache line already loaded.
* (+) Randomized lifecycle tests (TESTING §4) can assert "no stale event ever".
* (−) 20-bit index caps a pool at ~1M slots per worker (ample for per-worker
  connection pools).
* Rejected alternative: pointer as `user_data` (what the new Coring4 `Worker`
  did) — zero cost but silent corruption on any lifecycle slip. ASan poisoning
  of pooled objects can substitute in sanitizer builds only, not in production.
