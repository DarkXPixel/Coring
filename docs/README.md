# Coring5 documentation

Coring5 is the rewrite of the Coring4 prototype: a thread-per-core, `io_uring`
based multi-protocol proxy in C++23. This directory is the source of truth for
its design; the code under `include/coring5` and `src/coring5` implements it.

| Document                                     | What it answers                                                        |
| -------------------------------------------- | ---------------------------------------------------------------------- |
| [ARCHITECTURE.md](ARCHITECTURE.md)           | Modules, layering, threading, data path, memory, error model           |
| [INVARIANTS.md](INVARIANTS.md)               | The rules (I1–I10) every change must respect, and how each is enforced |
| [CONFIG_AND_RELOAD.md](CONFIG_AND_RELOAD.md) | Config schema, hot-reload protocol, ordering and failure behaviour     |
| [ROADMAP.md](ROADMAP.md)                     | Tiers, milestones M0–M10, acceptance criteria, risks                   |
| [EXCHANGE.md](EXCHANGE.md)                   | M4.1 design: Exchange state machine, BodyStream credit/backpressure, trailers (only `BodyStream` implemented) |
| [TESTING.md](TESTING.md)                     | Unit / integration / fault-injection / fuzz strategy, how to run it    |
| [BENCHMARKING.md](BENCHMARKING.md)           | Methodology, tools, scenarios, regression gating                       |
| [OPERATIONS.md](OPERATIONS.md)               | Build, run, systemd, containers, kernel requirements, metrics          |
| [MIGRATION.md](MIGRATION.md)                 | Audit of the Coring4 code and where each piece goes                    |
| [adr/](adr/)                                 | Architecture decision records (why, not just what)                     |

## Status

**This is a skeleton, not a finished proxy.** Be precise about what exists:

| Area                                                                   | State                                                                                                                                              |
| ---------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------- |
| `base`, `mem`, `io` (Ring, BufferRing, TimerWheel, ConnCore, ConnPool) | Implemented and tested; no virtual dispatch, one typed pool per protocol                                                                           |
| `runtime` (Worker, WorkerGroup, Mailbox, ConfigManager, Daemon)        | Implemented and tested with the `echo` protocol                                                                                                    |
| `config` (TOML loader, validator, diff), hot reload                    | Implemented and tested, including reload under load                                                                                                |
| `control` (admin socket) + `coringd` / `coringctl`                     | Implemented; blocking, one client at a time (see roadmap M2)                                                                                       |
| `obs` (metrics shards, SPSC ring)                                      | Metrics implemented; access-log writer not yet                                                                                                     |
| `proto/http1` head parser                                              | Implemented and fuzzed; body framing **not** implemented                                                                                           |
| `proto/l4`, `proto/http1conn`, `proto/http2`, `proto/http3`            | `EchoConn` reference; `L4Conn`/`Http1Conn` operational stubs (listeners accepted); `Http2Conn`/`Http3Conn` stubs (pools exist, listeners rejected) |
| `upstream`                                                             | Implemented (M3): `UpstreamPool` with RR/least-conn, passive ejection, panic mode, reload epochs                                                   |
| `http`                                                                 | `BodyStream` implemented and unit-tested (credit/watermark/EOF/abort); `Exchange`, headers, trailers not yet                                      |
| `tls`, `handlers`                                                      | Interface/design notes only                                                                                                                        |
| HTTP/2, UDP, HTTP/3                                                    | Not started                                                                                                                                        |
| `routing`                                                              | Still lives in `include/coring4/routing`; migrates in M4                                                                                           |

`echo`, `l4` and `http1` listeners are accepted by the config validator today
(`l4` is the M3 TCP passthrough and needs an `upstream`; `http1` answers 501/400 and closes);
`http2`/`http3` are rejected until M8/M10.

## Quick start

```sh
cmake --preset coring5-debug && cmake --build --preset coring5-debug
ctest --test-dir build-coring5-debug -L unit            # no kernel features needed
ctest --test-dir build-coring5-debug -L uring           # needs io_uring (see OPERATIONS.md)

build-coring5-debug/src/coring5/coringd --config deploy/coring.example.toml \
                                        --control /tmp/coring.sock &
build-coring5-debug/src/coring5/coringctl --socket /tmp/coring.sock status
echo hello | nc 127.0.0.1 8080
```

Legacy Coring4 targets are still buildable (`-DCORING_BUILD_LEGACY=ON`, the
default); the Coring5 presets turn them off.

## Verified so far

Checked in this repository at the time of writing (not a substitute for CI):

- 56 tests pass under a normal build, ASan+UBSan, and TSan: 44 Coring5 unit
  tests (`-L unit`), 11 Coring5 integration tests (`-L uring`), and the
  pre-existing `routing_test`.
- The three libFuzzer targets ran ~29 M executions in total without a finding.
- An end-to-end run of `coringd` + `coringctl` (ping, status, echo, reload via
  `coringctl` and via `SIGHUP`, rejected bad config, SIGTERM drain) behaved as
  specified in CONFIG_AND_RELOAD.md.
- No performance measurements exist yet. Nothing in these docs claims a number
  that has not been measured; targets are labelled as targets.
