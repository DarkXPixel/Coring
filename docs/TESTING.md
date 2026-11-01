# Testing strategy

Runner: GoogleTest under `ctest`. Labels select the tier:

| Label         | What                                                       | Needs                                                     |
| ------------- | ---------------------------------------------------------- | --------------------------------------------------------- |
| `unit`        | Pure logic: parsers, config, pools, timer wheel, metrics   | nothing special                                           |
| `uring`       | Integration: real `coringd` stack in-process, real sockets | working `io_uring` (skips itself with a reason otherwise) |
| `fuzz-replay` | Replays corpus/regression files through fuzz targets       | clang build with `CORING5_FUZZ=ON`                        |

```sh
cmake --preset coring5-debug && cmake --build --preset coring5-debug
ctest --test-dir build-coring5-debug -L unit --output-on-failure
ctest --test-dir build-coring5-debug -L uring --output-on-failure --timeout 120
python3 tools/check_layering.py
```

Sanitizer presets: `coring5-asan` (ASan+UBSan), `coring5-tsan`. Run the whole
suite under both before merging anything touching `io/` or `runtime/`.

## 1. Current coverage (what exists)

`tests/coring5/unit`:

| File                    | Covers                                                                                                                                                                                                                                                                                                |
| ----------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `handle_pool_test.cpp`  | **Regression for the Coring4 `SessionStorage` free-list bug** (every index handed out exactly once), full pool, stale-handle rejection after reuse, double destroy, invalid/out-of-range handles, **generation wrap never yields 0**, destructors run once, `for_each` with destroy, churn accounting |
| `optag_test.cpp`        | Round-trip of all fields, no bleed between fields, handle conversion, default tag                                                                                                                                                                                                                     |
| `timer_wheel_test.cpp`  | Ordering, never early, cancel, destructor unlinks, reschedule, deadlines beyond one lap, multi-hour stall, callbacks that reschedule/cancel peers, past deadlines                                                                                                                                     |
| `base_test.cpp`         | `Endpoint::parse` accept/reject tables, `Errc` names, `Error` is 8 bytes, `ReloadAck` across threads and timeout                                                                                                                                                                                      |
| `config_test.cpp`       | Parse + validate, empty doc, syntax/type errors, semantic matrix, missing file, `diff_snapshots`                                                                                                                                                                                                      |
| `http1_parser_test.cpp` | Happy paths, NeedMore at every prefix, pipelined bytes, keep-alive rules, **25 smuggling/malformed cases each pinned to a specific `ParseError`**, limits, segmentation independence                                                                                                                  |
| `obs_test.cpp`          | Histogram quantiles/clamping, metric aggregation, SPSC drop-on-full and 200k-item two-thread ordering                                                                                                                                                                                                 |

`tests/coring5/integration` (uses `support/TestServer.hpp`: in-process
`WorkerGroup` + `ConfigManager`, blocking test client):

| Test                                                                    | Property                                                                                           |
| ----------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------- |
| `Echo.RoundTrip`, `LargePayload...` (1 MiB, short writes, many buffers) | Data integrity through provided buffers                                                            |
| `ManyConcurrentClientsAcrossWorkers`                                    | 64 clients × 4 workers, `stale_events == 0`                                                        |
| `ClientCloseReleasesTheConnectionSlot`                                  | FIN and RST closes; `conns_live` returns to 0 (no slot leak)                                       |
| `PoolExhaustionShedsLoadWithoutCrashing`                                | Pool of 8, 32 clients: `conns_rejected` rises, server survives and recovers                        |
| `IdleTimeoutClosesTheConnection`                                        | Timer wheel → close path                                                                           |
| `GracefulShutdownDrainsAndClosesEverything`                             | Shutdown returns promptly, idle connections get EOF, listener is closed                            |
| `Reload.*` (4 tests)                                                    | See CONFIG_AND_RELOAD.md §3; includes **25 reloads under 4-thread load with zero failed requests** |

`tests/coring5/fuzz` (libFuzzer + ASan + UBSan, clang):

| Target            | Property                                                                                                                                 |
| ----------------- | ---------------------------------------------------------------------------------------------------------------------------------------- |
| `fuzz_http1_head` | **Differential:** the verdict (status, error, consumed, method, target, host, header count) must not depend on how the head is segmented |
| `fuzz_config`     | `parse_config` + `validate_config` never crash on arbitrary text                                                                         |
| `fuzz_endpoint`   | `Endpoint::parse` never crashes                                                                                                          |

## 2. Rules for writing tests here

- **Assert the invariants, not just the output.** Integration tests that stress
  lifecycle must check `stale_events == 0` and `conns_live → 0`.
- **Pin each rejection to a specific error code** (see the smuggling table), so
  a regression that rejects for the wrong reason is still caught.
- **A test for every fixed bug.** The `HandlePool` free-list test fails against
  the old `resize()` logic by construction.
- **Poll with a deadline, never `sleep` and hope** (`wait_until` in
  `echo_test.cpp`). The test client retries `EINTR` (a lesson from this repo:
  `recv` on an idle connection returned `EINTR` in roughly every run, and
  treating that as "closed" made an otherwise correct test fail).
- **Environment must not fail the build.** `CORING5_REQUIRE_RING()` skips with
  the reason (`unsupported errno=1` = EPERM from a seccomp filter) rather than
  failing.
- No test may depend on a fixed port: use `127.0.0.1:0` and read it back from
  the snapshot (`Stack::port()`), or `pick_port()` when two listeners must be
  named in a config.

## 3. What is not covered yet (priorities)

1. **`FakeRing` + randomized lifecycle tests** (§4) — the main missing net;
   build before M3.
2. Per-state client-disconnect table for every connection type.
3. Control server (malformed commands, slow client, wrong uid).
4. Failure injection for `Ring::create` (blocked syscall) → `Daemon::start`
   error message. (`EPERM` mapped to `Unsupported` is exercised accidentally in
   sandboxes; add an explicit test.)
5. Kernel matrix (6.1 LTS, current).

## 4. FakeRing — the deterministic lifecycle harness (to build in M1)

Goal: find lifecycle bugs without flaky networks.

**Status: not written yet** (the path below does not exist).

**Seam.** `io/Ring.hpp` is the real `io_uring` implementation. Add
`tests/coring5/support/FakeRing.hpp` with the same member functions
(`accept_multishot`, `recv_provided`, `send`, `cancel_fd`, `timeout`, `reap`,
`submit_and_wait`, …) and select it per CMake target (link-time or via a
`using Ring = FakeRing` in a test-only `IoContext` build). No runtime cost in
production and no template explosion.

**Behaviour.** Prepared SQEs are recorded. The test (or a seeded scheduler)
decides which CQEs to deliver and in what order:

- success / short sends / EOF / `-ECONNRESET`
- `-ENOBUFS` (buffer ring empty)
- `-ECANCELED` for ops hit by a cancel, delivered in arbitrary order relative
  to other CQEs
- multishot CQEs with and without `F_MORE`
- `SEND_ZC`: result CQE, then `F_NOTIF` later
- late CQEs after a close request; a timer firing between two CQEs; an
  upstream completing before its client
- SQ-full: `prep_*` returning `false`

**Properties asserted after every schedule** (thousands of seeds, CI):

1. `connections.live() == 0` once all in-flight CQEs are delivered
2. every provided buffer is back in the ring (`free_count() == count()`)
3. every connection reached `Closed` with `inflight == 0`
4. `stale_events == 0`
5. no CQE delivered to a connection after it was destroyed
6. no fd left open (fake fd table)

Reduce failures to a seed + a printed CQE trace.

## 5. Fuzzing

Targets exist for the HTTP/1 head, config and endpoint. To add (priority order):
chunked decoder (M4), path normalizer and route match (with property tests:
normalization idempotent, never escapes root), HTTP/1 response parser (upstream
replies), SNI/ClientHello extractor, HTTP/2 frame adapter, QUIC Initial.

Conventions:

- Differential harnesses where possible (segmentation independence, parse →
  serialize → parse round trip).
- Seed corpora from real traffic + the smuggling table; keep them in
  `tests/coring5/fuzz/corpus/<target>/`.
- Every crash becomes a file in `tests/coring5/fuzz/regressions/<target>/`; the
  `*_replay` ctest runs them on every build.
- Run: `cmake --preset coring5-fuzz && cmake --build --preset coring5-fuzz`,
  then `build-coring5-fuzz/tests/coring5/fuzz/fuzz_http1_head corpus/ -max_total_time=600`.
  CI runs 30 s per target as a smoke test; run longer nightly.

## 6. Integration scenarios still to write (by milestone)

- **M3:** upstream refuse / accept-then-reset / hang / partial / slow; slow
  reader and slow writer with bounded RSS; `ulimit -n` low (EMFILE) and a small
  cgroup memory limit.
- **M4:** `curl --http1.1`, chunked bodies, `Expect: 100-continue`,
  `websocat`; client disconnect at every request state; the smuggling corpus
  end to end; static-file path escape (`..`, `%2e%2e`, `%2f`, backslash,
  double-encoding).
- **M5:** `openssl s_client` SNI/ALPN/resumption/wrong cert; `testssl.sh`.
- **M6:** reload under load with routes/certs/upstreams changing; SIGTERM under
  load; `kill -9` + restart.
- **M8/M10:** `h2spec`; QUIC interop runner; migration; `tc netem` loss/reorder.

## 7. Static analysis and CI

CI (`.github/workflows/coring5.yml`): layering check; build + unit + uring
tests in debug, ASan+UBSan, TSan; libFuzzer smoke for 30 s per target.
`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion … -Werror`
on all Coring5 targets (`CORING5_WERROR`). Hosted runners may block `io_uring`,
in which case the `uring` label skips; keep a self-hosted runner or a
QEMU/virtme-ng job on a 6.1+ kernel for the real signal.
Valgrind is not useful for `io_uring`; use ASan/TSan.
