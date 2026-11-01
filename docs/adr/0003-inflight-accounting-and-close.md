# ADR 0003 — In-flight accounting at prep time; close only at `inflight == 0`

## Context
With `io_uring`, closing an fd does not stop operations already submitted for
it, and a CQE may arrive after the object that owned the operation is gone. A
late CQE on a recycled slot is silent data corruption. "Destroy only when
nothing is in flight" is only as good as the counter, and the kernel interface
has several traps.

## Decision
Every SQE is counted at the moment it is queued (`Connection::counted(bool)`),
including cases that are easy to forget:
* cancel SQEs (they have their own CQE),
* linked timeouts (second SQE, second CQE),
* multishot ops (decrement only on a CQE without `IORING_CQE_F_MORE`),
* `SEND_ZC` (result CQE with `F_MORE`, then the `F_NOTIF` CQE),
* only a submission that was really queued is counted (`prep_*` returns `bool`).

Close sequence: `Closing` → `cancel_fd` (counted; if the SQ is full,
`shutdown(2)` forces completions) → wait until `inflight == 0` → `close(fd)` →
`Closed` → the worker destroys the slot after the CQE batch (never inside the
connection's own callback).

## Consequences
* (+) No CQE can reference a destroyed connection; fd numbers are not reused
  while a cancel for the old socket is pending.
* (+) Protocol code gets one rule: wrap every submission in `counted()`.
* (−) Closing is asynchronous; shutdown must wait for CQEs (bounded by
  `drain_timeout` + a hard-stop grace period).
* (−) The counter is hand-maintained. Mitigations: `assert` in `handle_cqe`,
  `stale_events`, and the FakeRing randomized tests (TESTING §4), which exist to
  catch exactly this class of mistake.
