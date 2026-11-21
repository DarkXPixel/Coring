# ADR 0009 — L4 passthrough: one slot owns both sockets; queues copy out of the buffer ring

**Scope: L4 (TCP passthrough) only.** This ADR does *not* decide how HTTP/1,
HTTP/2 or HTTP/3 reverse proxying, TLS, or static-file serving obtain and manage
upstream connections. Those need different lifetimes (see "Not decided here")
and will be designed in a separate ADR at the start of M4.

## Context
M3 needs a client <-> upstream byte relay. The first design note proposed two
`Connection` objects cross-referencing each other through `Handle`s. That
doubles pool slots per proxied connection and creates peer-death races (one
side closes while the other still has a send in flight on shared state).
Separately, holding provided buffers while a sink is slow (I10) would starve the
per-worker buffer ring and turn one slow client into `-ENOBUFS` for everyone.

## Decision
* **L4 only:** `L4Conn` owns both fds. For an opaque byte stream the two sockets
  live and die together, so one lifecycle is enough. `ConnCore` gained an
  optional peer fd: `close()` issues one counted `cancel_fd` per fd (aux
  `kAuxConnection` / `kAuxPeer`) and falls back to `shutdown(2)` on whichever fd
  could not be queued. Both fds are closed together in `finish()` (I3).
* **Dialing is not L4-specific and is not part of `L4Conn`.** `net/UpstreamDialer`
  (non-virtual, a member of the dialing connection) picks a backend, opens the
  socket, runs `connect` + `LINK_TIMEOUT`, retries on another backend within ONE
  connect budget (`min(3, backends)` attempts, `upstream.connect_timeout_ms`
  else `limits.connect_timeout_ms`) and keeps the health books. While dialing,
  the socket is parked as the owner's peer fd so a `close()` cancels it like any
  other fd. On success `take()` moves the fd out together with a `BackendLease`
  (RAII over `UpstreamPool::acquire/release`); what happens next is the
  caller's business. `L4Conn` keeps both as its peer and relays. Retrying is
  only safe because the caller has not consumed any client byte yet.
* Received data is copied into a `ChunkQueue` (linked chunks from a per-worker
  `ChunkPool`; named `RelayQueue` and living in `proto/l4` when this ADR was
  written, moved to `mem/` in M4.1) and the buffer lease is released immediately. A send reads
  straight from the queue head; chunks are freed when the send completes, and
  the queue is cleared in the destructor (after `inflight == 0`).
* Backpressure: pause the source above `limits.queue_high_watermark` (or when
  the chunk pool is nearly empty), resume at `queue_low_watermark`.
* `UpstreamPool` (per worker, no sockets, no ring): round-robin /
  least-connections, passive ejection (`max_fails`, `eject_time_ms`, half-open
  re-probe), panic mode (pick anyway when everything is ejected), and a
  per-upstream `epoch` so a `BackendRef` from before a reload is a no-op instead
  of touching an unrelated backend.

## Not decided here (M4+)
These reuse `UpstreamPool` and probably `UpstreamDialer`, but not L4's ownership
model, and no generic "Connection" interface is introduced for them yet:
* HTTP/1 egress: upstream connection outlives a request (keep-alive reuse), so
  it belongs to a per-worker idle cache and is borrowed by handlers.
* HTTP/2 and HTTP/3 egress: one upstream session carries many client streams;
  the session is its own pooled object and streams refer to it by handle.
  HTTP/3 is UDP + QUIC and does not use the TCP dialer at all.
* Cross-protocol proxying (e.g. H3 in, H2 out): needs a protocol-neutral
  request/response layer (head, bounded body stream, trailers) so ingress and
  egress sessions meet in the middle instead of forming an N x M matrix.
* TLS: a sans-IO adapter usable for both termination and re-encryption.
* Static files: a handler kind with a file-backed body source, not an upstream.

## Consequences
* (+) One lifecycle per proxied L4 connection; half the slots; no peer lookups.
* (+) Relay memory is bounded by `relay_memory_mib` (`MAP_NORESERVE`, lazily
  faulted) regardless of how slow a peer is.
* (+) The connect/retry/health code is separate from the relay, so a later
  HTTP handler does not have to copy it.
* (-) One copy per direction (recv buffer -> chunk). Splice would avoid it; it
  stays optional until a benchmark justifies it.
* (-) Existing relayed connections keep their upstream after a reload; only new
  connections see the new backend list.
* (-) `UpstreamDialer` is TCP-only and assumes the owner has not consumed any
  client data; whether that holds for HTTP (request already sent) is part of
  the M4 decision.
