# Benchmarking plan

**Status: nothing here has been run.** No performance numbers exist for
Coring5 and none are claimed anywhere in these docs. This file defines how to
produce trustworthy ones. Build with the `coring5-release` preset (LTO).

## 1. Environment (most noise comes from here)

* Bare metal. Load generators on a **different machine** from the proxy (≥ 2×
  the proxy's core count), or on separate pinned cores. Loopback results are
  only for *relative* comparisons — they skip the NIC, GRO and the real stack,
  and `SEND_ZC` falls back to copying.
* `performance` governor, fixed frequency. Record kernel, CPU model, microcode,
  `mitigations=` state, NIC model/driver, and the Coring commit.
* Proxy workers, NIC queues and the NIC on the same NUMA node; RSS with one
  queue per worker core; IRQ affinity so queue IRQs land on the matching worker
  core (this is what makes `SO_INCOMING_CPU` steering pay off). Optionally
  `isolcpus`/`nohz_full`/`rcu_nocbs` for worker cores.
* Store this fingerprint with every result and refuse to compare runs with
  different fingerprints.

## 2. Tools

| Goal | Tool |
|---|---|
| Latency at fixed rate (open loop, correct for coordinated omission) | **wrk2**: `wrk2 -t4 -c1000 -d120s -R150000 --latency http://proxy:8080/1k.bin` |
| Max throughput | `wrk`, `oha`, `bombardier` (closed loop; do not trust their latencies) |
| HTTP/2, HTTP/3 | `h2load` (`-c`, `-m`, `--rps`); H3 needs a build with ngtcp2/nghttp3 |
| Many connections, L4 | `tcpkali`, `iperf3` (TCP baseline; `-u` for UDP) |
| UDP request/response | `dnsperf`, `trafgen`/pktgen for pps |
| Custom shapes | `vegeta`, `k6` |
| CLI/process timings | **hyperfine**: `coringd --check-config` on a large config, `coringctl reload` latency, startup time |
| Microbenchmarks | Google Benchmark (extend `tests/routing_bench.cpp`; add parser, timer wheel, `HandlePool`). Pin CPU, `--benchmark_repetitions=20`, report median and CV |
| Profiling | `perf stat` (cycles, instructions, branch/cache misses), `perf record -g` + flamegraph, `bpftrace`, `mpstat -P ALL`, `pidstat -w`, `ss -tim`, `/proc/<pid>/smaps_rollup` |
| Syscalls | `perf trace -s` / `strace -c -f` — check `io_uring_enter` calls per request |

## 3. Methodology

* **Open loop for latency.** Find the saturation knee with a rate ramp; run
  latency tests at fixed fractions of it (30/60/80/95%). Report p50, p90, p99,
  p99.9, p99.99, max from an HDR histogram measured by the **external**
  generator — never the proxy's own metrics (its `loop_ns` log2 histogram is a
  diagnostic for loop stalls only).
* 30 s warm-up (discarded), ≥ 120 s measured, ≥ 5 repetitions, report median
  and range; fail the comparison if run-to-run variance exceeds a few percent.
* **Baselines under identical constraints:** nginx, HAProxy, Envoy with the same
  core count, pinning, `reuseport`, TLS settings, logging off. Direct-to-backend
  is the floor. Equivalent configuration matters more than the tool.
* Vary one dimension at a time (not the full cross product):
  * protocol: H1 keep-alive; H1 `Connection: close` (accept/close rate); H2; H3; TCP; UDP
  * payload: 0 B, 1 KB, 64 KB, 1 MB, static 10 MB file
  * connections: 1, 100, 10k; 100k idle + 1k active
  * cores: 1, 2, 4, 8, 16 — efficiency = T_N / (N × T_1); a reasonable gate is
    ≥ 90% up to one NUMA node's physical cores
  * TLS: off; full handshake; resumption
  * upstream latency: 0 ms and a slow backend (e.g. 5 ms)

## 4. Metrics

| Metric | How | Why |
|---|---|---|
| RPS/core, Gbit/s/core | generator output ÷ worker cores | Primary efficiency number |
| Cycles and instructions per request | `perf stat` | More stable than CPU % across builds |
| `io_uring_enter` per request; CQEs per enter | `perf trace`, `coring_cqes_total / coring_loop_iterations_total` | Shows batching; should be well < 1 syscall/request under load |
| Involuntary context switches, CPU migrations | `pidstat -w`, `perf sched` | ≈ 0 on pinned workers |
| p99 / p99.9 / p99.99 | wrk2 / h2load | Headline latency |
| Loop stall | `coring_loop_ns_p999_upper_bound_max` (+ `perf record` around worst iterations) | Finds the cause of tail latency; set a budget (e.g. < 200 µs) |
| RSS per idle connection at 100k | `smaps_rollup` | Validates the provided-buffer design (target < 10 KB, unmeasured) |
| Heap allocations per request | count `operator new` or `heaptrack` | Should be 0 on the hot path; assert in a test |
| Page faults, L3 misses per request | `perf stat` | Cold pools, bad layout |
| SQ-full, CQ overflow, `-ENOBUFS` | `sq_full_total`, `cq_overflow_total`, `nobufs_total` | Capacity-planning signals |
| TLS handshakes/s per core | `h2load`, `openssl s_time` (resumption off) | Bounds accept-heavy workloads |

All the `coring_*` counters come from `coringctl stats`.

## 5. Scenarios specific to this architecture

* **Echo scaling (available now):** the `echo` listener already exercises
  accept, provided-buffer recv, send, close, timers. Use `tcpkali` against it
  with 1–N workers to validate scaling and CQE batching *before* any protocol
  exists. This is the first benchmark to run.
* **Reload under load:** run at 70% of the knee; loop `coringctl reload` every
  200 ms; compare p99.9 and error counts with a no-reload run; record apply time
  per worker.
* **Slow clients:** 1k clients reading at 1 B/s; measure proxy RSS and the
  effect on other clients' latency.
* **Churn:** accept/close at maximum rate while holding 50k idle connections.
* **Overload:** 150% of the knee; the proxy must shed load (`conns_rejected`,
  503s later) with bounded memory and no latency collapse for admitted work.
* **Imbalance:** skewed connection distribution across workers (a few heavy
  connections); quantify the cost of thread-per-core vs. the aggregate.
* **ZC and splice A/B:** same workload on/off while sweeping payload size to
  find the real crossover points (do not assume the 4 KB constant from Coring4).

## 6. Tail-latency checklist (p99.9 bad, median fine)

1. Loop-stall histogram, then `perf record` around the worst iterations.
2. TLS handshakes inline on the loop.
3. Snapshot destruction on a worker (should not happen: old snapshots retire on the control thread — verify).
4. Page faults from lazily touched memory, or `munmap`.
5. Logging on the loop (move to the writer thread).
6. Timer-wheel bursts (many deadlines expiring in the same tick).
7. A worker sharing an SMT sibling or an IRQ core.
8. CQ overflow (kernel falls back to a slower overflow list).

## 7. Regression gating

* Per commit: microbenchmarks gated on **instruction counts** (±5%, more stable
  than time) and a small e2e smoke for sanity only.
* Nightly on a dedicated runner: the full matrix with stored time series and
  the environment fingerprint; alert on throughput drops or p99.9 rises beyond
  the measured noise band.
