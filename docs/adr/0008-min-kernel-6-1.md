# ADR 0008 — Kernel floor 6.1; probe the rest

## Context
`io_uring` evolves quickly. Supporting old kernels would mean maintaining
fallback paths for features the design depends on.

## Decision
* Hard minimum **6.1** (`Ring::create` returns `Unsupported` below it):
  `SINGLE_ISSUER` + `DEFER_TASKRUN`, buffer rings, multishot accept/recv,
  `SEND_ZC` are available there.
* `DEFER_TASKRUN` is requested and dropped silently if the kernel rejects it
  (`-EINVAL`). `RingCaps` records `send_zc`, `splice`, `multishot_recv`,
  `buf_ring` via `io_uring_get_probe_ring`; optional paths are gated on it.
* A blocked `io_uring` (seccomp/`kernel.io_uring_disabled`, `EPERM`/`ENOSYS`) is
  reported as `Unsupported` with a message naming the likely cause; tests skip
  with the reason rather than fail.
* CI runs a kernel matrix (6.1 LTS + current) where possible; hosted runners may
  block `io_uring`, so a self-hosted/QEMU job provides the real signal.

## Consequences
* (+) One code path for the core features; no emulation layer.
* (−) Excludes older enterprise distros (e.g. 5.x kernels).
* (−) Container users must allow `io_uring` in their seccomp profile.
