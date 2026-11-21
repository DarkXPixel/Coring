# Roadmap

## 1. Definition of "production-ready"

| Tier                     | Meaning                                                                                                         | Reached at |
| ------------------------ | --------------------------------------------------------------------------------------------------------------- | ---------- |
| **T1 Usable**            | HTTP/1.1 + TCP proxy + static files, TLS, hot reload, graceful shutdown. You'd run your own services behind it. | M6         |
| **T2 Hardened**          | Fuzzed parsers, smuggling-safe, limits/timeouts everywhere, metrics, 7-day soak without growth in RSS or fds.   | M7         |
| **T3 Full protocol set** | HTTP/2, UDP, HTTP/3 — each optional and independent.                                                            | M8–M10     |

Targets (to be measured, not yet measured), on identical hardware against nginx
with equal cores: ≥ nginx RPS/core on small keep-alive requests; p99.9 no worse
than nginx at 60% of saturation; zero failed requests during reload; < 10 KB
per idle connection.

## 2. Where we are

```
M0 Triage            ██████████  done (as the Coring5 skeleton; see §4)
M1 I/O core          ████████░░  Ring, BufferRing, HandlePool, TimerWheel, Connection, Echo: done.
                                 Missing: FakeRing + randomized lifecycle tests
M2 Runtime           ███████░░░  WorkerGroup, Mailbox, signalfd, drain, metrics: done.
                                 Missing: event-driven control server, access-log writer
M3 L4 proxy          █████████░  done except the optional splice strategy (benchmark-gated)
M4 HTTP/1.1          ██░░░░░░░░  strict head parser + fuzz target only
M5 TLS               ░░░░░░░░░░
M6 Config manager    ██████░░░░  listeners/limits reload done; routes/certs/upstreams pending
M7 Hardening         █░░░░░░░░░  systemd unit, parser strictness; limits per IP etc. pending
M8 HTTP/2            ░░░░░░░░░░
M9 UDP relay         ░░░░░░░░░░
M10 HTTP/3           ░░░░░░░░░░
```

## 3. Milestones

Each milestone ends with a demoable binary and an automated acceptance test.
Estimates assume ~10 h/week and are rough; expect up to 2× slip. M8–M10 are
independent: if time runs out, T1 + M7 is still a complete project.

### M0 — Triage ✔

New tree `coring5/` with per-module libraries, warnings-as-errors, sanitizer
presets, CI, layering check. The known Coring4 defects were fixed _by
construction_ in the new code rather than patched (list in MIGRATION.md).
Legacy code stays buildable (`CORING_BUILD_LEGACY=ON`) until M4 works, then
moves to `legacy/` and is deleted.

### M1 — I/O core (finish)

Done: `Ring`, `OpTag`, `BufferRing`, `HandlePool`, `TimerWheel`, `Connection`
lifecycle, `EchoConnection`.
**Remaining:**

1. `FakeRing` (test double with the same API as `Ring`, scripted CQEs; link-time
   seam) and randomized lifecycle tests — see TESTING.md §4. This is the main
   missing safety net and should come before M3.
2. Acceptance: thousands of seeded schedules end with `live()==0`, buffer ring
   full, `inflight==0`, `stale_events==0`.

### M2 — Runtime (finish)

Done: `WorkerGroup` (two-phase start, errors reported), per-worker
`SO_REUSEPORT` listeners, `Mailbox`, `signalfd`, graceful drain, metric shards.
**Remaining:** event-driven `ControlServer` (own ring or epoll; no blocking
per-client I/O); access-log SPSC → writer thread with rotation; NUMA-aware CPU
list helper.
Acceptance: SIGTERM under load drains within the deadline with no dropped
in-flight response; echo throughput scales with cores (first benchmark, see
BENCHMARKING.md).

### M3 — L4 TCP proxy (2 wk)

Non-blocking connect with `LINK_TIMEOUT`; `UpstreamPool` (numeric endpoints from
the snapshot; round-robin, then least-connections; passive health ejection);
`ChunkQueue` (was `RelayQueue`) with watermarks and half-close; idle timeouts; optional splice
strategy. Config: `protocol = "l4"`, `upstream = "<name>"`.
Acceptance: slow-reader test keeps RSS bounded; 1M-connection churn leaks no fds
or slots; iperf3 through the proxy is limited by the NIC/loopback.

**Status:** implemented (`L4Conn`, `ChunkQueue`, `ChunkPool`, `UpstreamPool`,
`net/UpstreamDialer`, `[[upstream]]` config; ADR 0009, which is scoped to L4
only). Covered by `tests/coring5/integration/l4_test.cpp`
(relay integrity, 8 MiB with tiny watermarks, half-close, upstream close,
refuse/retry/ejection/panic, connect timeout, reset, churn, slow reader bounded
by chunk high-water mark, idle timeout, drain, reload) and `unit/relay_test.cpp`.
**Not done:** the optional `splice` strategy; the 1M-connection churn and iperf3
runs (no benchmark has been run, see BENCHMARKING.md).

### M4 — HTTP/1.1 (5–6 wk)
**First task:** accept or revise the draft [ADR 0010](adr/0010-m4-exchange-layer-sessions-tls-static.md)
(protocol-neutral Exchange layer, upstream session lifecycles, sans-IO TLS
`Transport`, static files as a response producer). It proposes the step order
M4.0–M4.5 and what is deliberately left open. ADR 0009 does not cover these.
The M4.1 detail (Exchange state machine, BodyStream credit rules, trailers) is
specified, design only, in [EXCHANGE.md](EXCHANGE.md); its protocol is checked
by the exhaustive model `tools/exchange_model.py`.

Body framing (Content-Length, chunked decoder as a state machine, trailers),
response serializer, server session (pipelining policy, keep-alive,
`Expect: 100-continue`), router integration (move `routing/` here and fold it
into `ConfigSnapshot`), handlers: `Respond`/`Redirect`/`StaticFile`/
`ReverseProxy` (hop-by-hop stripping, `X-Forwarded-*`, chunked, Upgrade/WebSocket,
upstream keep-alive reuse with liveness probe, retries only for idempotent
methods **and** only before any byte was sent). Static files: fd + `statx`
cache, `splice` or `read`+`send`, `Range`, `ETag`/`If-Modified-Since`, mandatory
path-escape tests.
Acceptance: parser fuzz clean for 1 h; the request-smuggling corpus is rejected;
first benchmark vs nginx recorded.

### M5 — TLS (3 wk)

OpenSSL memory-BIO adapter, ALPN, SNI from the snapshot catalogue, session
tickets with rotating shared keys, optional kTLS. Measure handshake cost on the
loop (loop-stall histogram) before deciding on a helper thread.
Acceptance: `testssl.sh` clean; handshakes/s recorded; reload swaps certificates
with zero failed handshakes.

### M6 — Config manager (finish) (3 wk)

Done: schema, validator, diff, publish/ack/retire, listener add/remove,
`coringctl reload`. **Remaining:** route sets, certificates and upstream
catalogues inside the snapshot (see CONFIG_AND_RELOAD §5); drain deadlines for
removed listeners' connections; `--check-config` extended to cross-reference
checks (listener → route → upstream → tls). **T1 reached here.**
Acceptance: reload loop under 70% of saturation — 0 failed requests, 0 resets;
bad config rejected with the old one serving.

### M7 — Hardening (3 wk) → T2

Per-IP connection limits and rate limits; slowloris/header-bomb limits using
the existing deadlines; access-log ring + writer; more fuzz targets (chunked
decoder, path normalizer, config, SNI/ClientHello); seccomp/systemd review;
7-day soak with chaos injection.
Acceptance: no growth in RSS or fds over 7 days; `stale_events == 0`.

### M8 — HTTP/2 (optional, 4–5 wk)

nghttp2 adapter, flow control mapped to relay watermarks, stream/header limits.
Acceptance: `h2spec` passes; rapid-reset and CONTINUATION-flood tests survive
with bounded CPU.

### M9 — UDP relay (optional, 2 wk)

Session table with timeouts, multishot `recvmsg`, per-session upstream socket.
Acceptance: echo and DNS-style load tests.

### M10 — HTTP/3 (optional, 8+ wk, largest risk)

ngtcp2 + nghttp3, UDP GSO/GRO, worker id in the connection ID + reuseport eBPF
steering. Acceptance: a QUIC interop-runner subset passes.

## 4. What the skeleton already covers vs. the original plan

| Original plan item                                       | State                                                                                     |
| -------------------------------------------------------- | ----------------------------------------------------------------------------------------- |
| Split CMake into module libraries; sanitizer presets; CI | done (`src/coring5/CMakeLists.txt`, `CMakePresets.json`, `.github/workflows/coring5.yml`) |
| Fix `SessionStorage` free-list bug                       | fixed in `HandlePool`; regression test `EveryIndexIsHandedOutExactlyOnce`                 |
| Fix provided-buffer `len` argument                       | fixed in `BufferRing::recycle`                                                            |
| Ring destructor / SQ-full handling                       | `Ring::~Ring` calls `io_uring_queue_exit`; prep functions return `bool`                   |
| Fix `Worker::on_io` fall-through                         | new `Worker::dispatch` switch; one handler per `OpKind`                                   |
| Fix `Listener2`                                          | split into `ListenerSpec` (config) + `create_tcp_listener` (socket); no UB path           |
| Layering enforced                                        | `tools/check_layering.py` + CI job                                                        |

## 5. Risks

| Risk                                                                                     | Mitigation                                                                                                                          |
| ---------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------- |
| Lifecycle bugs (late/stale CQEs, double close)                                           | I2/I3/I4; build `FakeRing` randomized tests before M3                                                                               |
| Scope creep (HTTP/3 first, "one more feature")                                           | Tier gates: no M8+ before M7                                                                                                        |
| Container/seccomp blocks `io_uring` (docker default profile; `kernel.io_uring_disabled`) | Startup probe with a clear error (done, see OPERATIONS.md); integration tests skip with a reason                                    |
| Tail latency from slow work on the loop                                                  | `loop_ns` histogram from day one; helper thread only if data says so                                                                |
| Kernel feature drift                                                                     | `RingCaps` probing; kernel test matrix (6.1 + current)                                                                              |
| `SO_REUSEPORT` drops a closed member's accept queue                                      | Never close kept listeners on reload (done); stop-arm-then-drain for removal; pass fds via `SCM_RIGHTS` for binary upgrade (future) |
| Solo burnout                                                                             | Each milestone demoable; write the acceptance test first                                                                            |

## 6. Known gaps in the current skeleton (be honest)

- The control server is blocking, single-client.
- `Worker::run` calls `std::terminate()` on an unrecoverable `io_uring_enter`
  error (supervisor restarts). Intentional but worth revisiting for graceful
  fail-over.
- Accept errors such as `-EMFILE` are retried on the next tick (default 50 ms);
  there is no adaptive backoff or fd-reserve trick yet.
- The `Listener` slot pool is a fixed 1024 per worker.
- `ConfigManager::apply` waits for acks while holding its mutex (serialises
  reloads by design, but a stalled worker blocks the next reload until timeout).
- No benchmark has been run; no performance claims are made.
