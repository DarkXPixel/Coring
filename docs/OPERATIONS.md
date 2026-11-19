# Operations

## 1. Requirements

- Linux **6.1+** (hard minimum: `Ring::create` rejects older kernels with
  `Unsupported`). Features are probed at start-up: `DEFER_TASKRUN` is requested
  and silently dropped if the kernel rejects it; `SEND_ZC`, `splice`, buffer
  rings and multishot recv are recorded in `RingCaps`.
- liburing ≥ 2.4, a C++23 compiler (GCC 14+/Clang 18+ expected; developed on
  GCC 16), CMake ≥ 3.25, Ninja, GoogleTest, toml++. (Legacy targets also need Boost.)
- **`io_uring` must be permitted.** It is blocked by:
  - Docker's default seccomp profile (use a profile that allows
    `io_uring_setup/enter/register`),
  - `kernel.io_uring_disabled=1|2` (kernels ≥ 6.6),
  - some hardened/sandboxed environments.

  Symptom: `coringd` exits at start-up with
  `worker 0: io_uring setup failed (unsupported, errno 1). Blocked by seccomp or kernel.io_uring_disabled? Kernel >= 6.1 required.`
  and the `uring` test label skips with `io_uring unavailable: unsupported errno=1`.

## 2. Build

```sh
cmake --preset coring5-release && cmake --build --preset coring5-release
# artifacts: build-coring5-release/src/coring5/{coringd,coringctl}
```

Presets: `coring5-debug`, `coring5-release`, `coring5-asan`, `coring5-tsan`,
`coring5-fuzz` (clang). `-DCORING5_WERROR=OFF` relaxes warnings-as-errors.
`-DCORING_BUILD_LEGACY=OFF` (set by these presets) skips the Coring4 targets and
the Boost dependency.

## 3. Run

```sh
coringd --config /etc/coring/coring.toml [--control /run/coring/control.sock] [--check-config]
```

`--check-config` parses and validates, prints `configuration OK`, exits 0 (or
prints the error, exit 1). Exit codes: 0 clean shutdown, 1 start-up error, 2 usage.

| Signal              | Effect                                                                                                  |
| ------------------- | ------------------------------------------------------------------------------------------------------- |
| `SIGHUP`            | reload config (same path as `coringctl reload`; result is logged to stderr)                             |
| `SIGTERM`, `SIGINT` | graceful shutdown: stop accepting, drain connections up to `runtime.drain_timeout_ms`, then force-close |

`coringctl --socket PATH ping|status|reload|stats|shutdown` (default socket
`/run/coring/control.sock`).

### systemd

`deploy/coringd.service` runs `--check-config` in `ExecStartPre`, reloads with
`SIGHUP`, drops to user `coring`, keeps only `CAP_NET_BIND_SERVICE`, sets
`LimitNOFILE=1048576` and `LimitMEMLOCK=infinity`, and enables the usual
sandboxing. Adjust `LimitNOFILE` to your connection budget
(≈ `workers × max_connections_per_worker`, ×2 once upstream connections exist).
Do not add a seccomp filter that blocks `io_uring_*`.

### Sizing

| Setting                                                                     | Guidance                                                                                                                                                                                                                                 |
| --------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `workers`                                                                   | One per physical core you dedicate; pin with `cpus` on multi-socket/NUMA machines                                                                                                                                                        |
| `buffer_count × buffer_size`                                                | One slab per worker (default 1024 × 16 KiB = 16 MiB); too few buffers ⇒ `-ENOBUFS` closes (`nobufs_total`) and eventually shedding                                                                                                       |
| `max_connections_per_worker`, `[runtime.pools]`, listener `max_connections` | Budget split into one fixed pool per protocol (sized at startup; slot = `sizeof` of that connection type); beyond a pool's capacity new connections are accepted-and-closed (`connections_rejected_total`, `pool_rejected_total{proto}`) |
| `ring_sq_entries` / `ring_cq_entries`                                       | cq ≥ sq, ideally ≥ 2×; watch `sq_full_total` and `cq_overflow_total`                                                                                                                                                                     |
| `tick_ms`                                                                   | Timer resolution and the cadence of listener re-arm after accept errors                                                                                                                                                                  |

## 4. Metrics (`coringctl stats`)

Prometheus-style text aggregated across worker shards (single-writer, relaxed
atomics; scraping does not perturb the data path).

| Metric                                                                          | Type          | Meaning / what to alert on                                                                                              |
| ------------------------------------------------------------------------------- | ------------- | ----------------------------------------------------------------------------------------------------------------------- |
| `coring_accepts_total`                                                          | counter       | Accepted connections                                                                                                    |
| `coring_accept_errors_total`                                                    | counter       | Accept CQE errors (e.g. `-EMFILE`); non-zero ⇒ raise `LimitNOFILE`                                                      |
| `coring_connections_rejected_total`                                             | counter       | Pool full, accepted-and-closed; non-zero ⇒ raise `max_connections_per_worker` or scale out                              |
| `coring_connections_live`                                                       | gauge         | Current connections (all pools)                                                                                         |
| `coring_pool_{accepts_total,rejected_total,connections_live}{proto="…"}`        | counter/gauge | Same, per connection pool (`echo`, `l4`, `http1`, `http2`, `http3`)                                                     |
| `coring_upstream_{connect_ok,connect_fail,retries,ejections,panic_picks}_total` | counter       | l4 upstream health: failures, retries on another backend, passive ejections, picks made while every backend was ejected |
| `coring_relay_pauses_total`, `coring_relay_shed_total`                          | counter       | Directions paused at the high watermark; connections closed because the relay chunk pool was empty                      |
| `coring_relay_chunks_{in_use,peak}`                                             | gauge         | Relay chunks in use now / high-water mark (bounds relay memory)                                                         |
| **`coring_stale_events_total`**                                                 | counter       | **Must be 0.** Non-zero is a lifecycle bug (invariant I3/I4); file it with the log                                      |
| `coring_nobufs_total`                                                           | counter       | Receive buffer ring exhausted                                                                                           |
| `coring_sq_full_total`, `coring_cq_overflow_total`                              | counter       | Ring capacity pressure                                                                                                  |
| `coring_bytes_in_total`, `coring_bytes_out_total`                               | counter       | Payload bytes                                                                                                           |
| `coring_loop_iterations_total`, `coring_cqes_total`                             | counter       | Ratio = CQEs per loop iteration (batching)                                                                              |
| `coring_buffers_free`                                                           | gauge         | Free provided buffers (sum over workers)                                                                                |
| `coring_closes_total{reason="…"}`                                               | counter       | Closes by `Errc` reason (`eof`, `reset`, `timeout`, `no_buffers`, `overloaded`, `shutdown`, …)                          |
| `coring_loop_ns_p999_upper_bound_max`                                           | gauge         | Worst worker's loop-iteration p99.9 (power-of-two bucket upper bound, ns)                                               |
| `coring_workers`                                                                | gauge         | Worker count                                                                                                            |

`config_generation` is per worker internally and surfaced as `generation=` in
`coringctl status`.

## 5. Troubleshooting

| Symptom                                                              | Likely cause / action                                                                                 |
| -------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------- |
| Start-up error mentioning io_uring / errno 1                         | Seccomp or `kernel.io_uring_disabled` (see §1)                                                        |
| `worker N: cannot pin to cpu X`                                      | `cpus` lists a cpu outside the process affinity mask / cgroup cpuset                                  |
| `listener 'x' (addr): cannot bind (errno 98)`                        | Address in use by a process **without** `SO_REUSEPORT` (all coring workers share the port through it) |
| `coringctl: connect …: Permission denied`                            | Control socket is 0660; run as the daemon's user/group, or root                                       |
| Reload returns `ERR the [runtime] section changed: restart required` | By design; restart, or revert the `[runtime]` edit                                                    |
| Reload returns `acked=false`                                         | A worker did not ack within `reload_ack_timeout_ms`; check loop stalls (`loop_ns_p999…`)              |
| `connections_rejected_total` climbing                                | Pool exhausted (see sizing)                                                                           |
| `stale_events_total` > 0                                             | Bug; capture config + traffic pattern                                                                 |
| Idle connections closed at exactly `idle_timeout_ms`                 | Intended (`closes{reason="timeout"}`)                                                                 |

## 6. Upgrades

Config changes to `[limits]`/`[[listener]]`: `systemctl reload coringd`. Changes
to `[runtime]` or a new binary: restart (connections are drained up to
`drain_timeout_ms` first). Zero-downtime binary upgrade (passing listener fds to
the new process over `SCM_RIGHTS`) is a future item; it matters because closing
an `SO_REUSEPORT` group member drops that socket's accept queue.
