# ADR 0006 — One slab per worker, kernel-selected buffers, RAII leases

## Context
Pre-assigning a receive buffer per connection makes idle connections expensive
(100k × 16 KiB = 1.6 GB). `io_uring` provided-buffer rings let the kernel pick a
buffer only when data arrives. Coring4's pool also passed the wrong length to
`io_uring_buf_ring_add` and kept a side table `bid → pointer`.

## Decision
* One contiguous `mmap` slab per worker (`Region`), pre-faulted on the worker
  thread (first-touch on its NUMA node), `MADV_HUGEPAGE` best effort.
  `bid → address` is `base + bid * buf_size`.
* `BufferRing::Lease` is move-only and recycles on destruction; `publish()`
  advances the ring tail once per loop iteration.
* A connection never parks a lease while idle (I10); partial heads are copied
  to a bounded staging buffer.
* `-ENOBUFS` is a signal to shed/retry, not a protocol error;
  `buffers_free` is exported.

## Consequences
* (+) Idle connections cost no receive buffers; recycling is cheap and batched.
* (−) Buffers handed to a `send` must stay alive until its CQE (and for
  `SEND_ZC` until `F_NOTIF`): `EchoConnection` keeps the lease in `pending_`.
* (−) Buffer sizing is a global per-worker knob (`buffer_count`, `buffer_size`);
  tune with `nobufs_total` and `buffers_free`.
