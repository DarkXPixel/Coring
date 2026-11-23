# Coring5 build and operations guide

Coring5 is a Linux TCP relay and HTTP/1.1 server and reverse proxy. It also has a TCP echo listener for testing. This guide uses unprivileged loopback listeners and a private control socket.

## Prerequisites

- Linux 6.1 or newer with `io_uring_setup`, `io_uring_enter` and `io_uring_register` permitted. Check `uname -r` and, when present, `sysctl kernel.io_uring_disabled`.
- A C++23 compiler and standard library with `std::expected`, `std::format` and C++23 ranges algorithms. Use GCC 14 or newer for the daemon and test presets. Use Clang with a recent C++ standard library, libFuzzer and the sanitizer runtimes for the fuzz targets.
- CMake 3.30 or newer, Ninja, pkg-config, liburing development files, toml++ headers and GoogleTest development files. Python 3 runs the layering, Exchange model, CPU-selection and TCP load tools. The local examples use curl, and the optional network and HTTP throughput comparisons use iperf3 and oha.
- Budget file descriptors for listeners, worker internals, accepted clients, upstream/file fds and idle HTTP upstreams. Verify `ulimit -n` and the service manager's equivalent. A worker with an HTTP/1 pool can park up to 64 upstream sockets. If ring or buffer registration fails, check the log and the process's memlock limit.

## Build and test under /tmp

Run these commands in Bash from the repository root.

```bash
set -e
out=$(mktemp -d /tmp/coring5.XXXXXX)

# Capture everything, print up to 60 lines, and preserve the command's status.
run_log() {
  local log=$1 rc=0
  shift
  "$@" >"$log" 2>&1 || rc=$?
  tail -n 60 "$log"
  return "$rc"
}

for mode in debug release; do
  run_log "$out/config-$mode.log" cmake --preset "coring5-$mode" -B "$out/$mode" \
    -DCORING5_BUILD_TESTS=ON -DCORING5_WERROR=ON
  run_log "$out/build-$mode.log" cmake --build "$out/$mode" --parallel 4
  run_log "$out/test-$mode.log" ctest --test-dir "$out/$mode" --no-tests=error \
    --output-on-failure --timeout 120
done
python3 tools/check_layering.py
python3 tools/exchange_model.py
```

The executables are under `$out/release/src/coring5/`. Release uses LTO. The sanitizer and fuzz presets disable it. The Coring5 warning policy enables `-Werror` on production targets and selected test targets. To treat warnings as errors on all CMake targets, add `-DCMAKE_COMPILE_WARNING_AS_ERROR=ON`. Set `--parallel` to fit available memory. `ctest -L unit` includes the FakeRing suite and `ctest -L uring` selects real-ring tests. Check the summary for skipped tests even if CTest exits successfully.

### Sanitizers and fuzzing

```bash
for mode in asan tsan; do
  run_log "$out/config-$mode.log" cmake --preset "coring5-$mode" -B "$out/$mode"
  run_log "$out/build-$mode.log" cmake --build "$out/$mode" --parallel 4
  run_log "$out/test-$mode.log" ctest --test-dir "$out/$mode" --no-tests=error \
    --output-on-failure --timeout 120
done

run_log "$out/config-fuzz.log" cmake --preset coring5-fuzz -B "$out/fuzz"
run_log "$out/build-fuzz.log" cmake --build "$out/fuzz" --parallel 4 --target \
  fuzz_http1_head fuzz_http1_body fuzz_config fuzz_endpoint

for target in fuzz_http1_head fuzz_http1_body fuzz_config fuzz_endpoint; do
  mkdir -p "$out/corpus/$target" "$out/artifacts/$target"
  run_log "$out/$target.log" "$out/fuzz/tests/coring5/fuzz/$target" \
    -max_total_time=30 -artifact_prefix="$out/artifacts/$target/" "$out/corpus/$target"
done
```

The `asan` preset combines ASan and UBSan. TSan uses its own build. The fuzz preset enables coverage, ASan and UBSan in the implementation as well as in the harness. Use a seed corpus of valid messages and `-max_total_time=3600` for the HTTP head and body acceptance runs. To replay a saved artifact, pass its filename to the corresponding fuzzer. For corpus files registered by CMake, use `ctest --test-dir "$out/fuzz" -L fuzz-replay --output-on-failure`.

## Run a local HTTP service

Select the binaries and create a private test directory. Run the demo and client commands in the same shell so they share the paths and process IDs.

```bash
bin="$out/release/src/coring5"
demo=$(mktemp -d /tmp/coring5-demo.XXXXXX)
cfg="$demo/coring.toml"
sock="$demo/control.sock"
mkdir "$demo/www"
printf 'hello from disk\n' >"$demo/www/file.txt"
printf 'hello from the origin\n' >"$demo/www/api"
cat >"$cfg" <<EOF
[runtime]
workers = 1
max_connections_per_worker = 512

[limits]
max_request_body_bytes = 10485760

[access_log]
path = "$demo/access.log"
rotate_bytes = 1048576
rotate_files = 3

[[upstream]]
name = "origin"
backends = ["127.0.0.1:18081"]
policy = "round_robin"

[[listener]]
name = "demo"
address = "127.0.0.1:18080"
protocol = "http1"
route = "demo"

[[route_set]]
name = "demo"

[[route_set.vhost]]
default = true

[[route_set.vhost.location]]
path = "/"
match = "exact"
action = "respond"
status = 200
body = "hello from Coring5"

[[route_set.vhost.location]]
path = "/go"
match = "exact"
action = "redirect"
status = 302
target = "/"

[[route_set.vhost.location]]
path = "/file.txt"
match = "exact"
action = "static_file"
root = "$demo/www"

[[route_set.vhost.location]]
path = "/api"
match = "prefix"
action = "reverse_proxy"
upstream = "origin"
EOF

"$bin/coringd" --config "$cfg" --check-config
python3 -m http.server 18081 --bind 127.0.0.1 --directory "$demo/www" \
  >"$demo/origin.log" 2>&1 &
origin_pid=$!
"$bin/coringd" --config "$cfg" --control "$sock" >"$demo/daemon.log" 2>&1 &
daemon_pid=$!
```

Run the client commands after startup. A successful `ping` returns `PONG`. If the socket is still coming up, retry `ctl ping`. If the daemon exits, check `$demo/daemon.log`.

```bash
ctl() { "$bin/coringctl" --socket "$sock" "$@"; }

ctl ping
ctl status
curl --fail http://127.0.0.1:18080/
curl -i http://127.0.0.1:18080/go
curl -I http://127.0.0.1:18080/file.txt
curl -i -H 'Range: bytes=0-4' http://127.0.0.1:18080/file.txt
curl --fail http://127.0.0.1:18080/api
ctl stats
```

The expected responses are 200 with the inline body, 302 with `Location: /`, a HEAD response, a 206 for the byte range, and the origin's body. To test conditional requests, copy `ETag` from the HEAD reply to an `If-None-Match` header or copy `Last-Modified` to `If-Modified-Since`. An unchanged file returns 304. `If-None-Match` takes precedence when both conditional headers are present. [HTTP Semantics](https://www.rfc-editor.org/rfc/rfc9110.html#section-13.1.3)

`coringd --config FILE [--control PATH] [--check-config]` requires a config path. `--check-config` validates TOML and configuration relationships before binding listeners, opening static roots, or opening the log sink. A successful check prints `configuration OK`. Start the daemon to verify operating-system resources and permissions. The default control socket for both binaries is `/run/coring/control.sock`. Give the daemon a writable parent directory and each instance a distinct path. The Unix socket has mode 0660. The server also checks peer credentials and accepts the daemon user or root.

`coringctl [--socket PATH] ping|status|stats|reload|shutdown` sends one command. `status` reports workers, generation and listener count. `stats` prints aggregated counters and gauges, including access-log and trailer metrics. `shutdown` acknowledges the stop request. Wait for the process to exit to confirm the drain and log flush have finished.

## Configuration reference

| Section | Fields and behavior |
| --- | --- |
| `[runtime]` | Restart-only. Defaults are `workers=1`, unpinned workers without `cpus`, and `max_connections_per_worker=10000`. Supply at least one CPU ID per worker when pinning. `buffer_count=1024` must be a power of two in 2–32768. `buffer_size=16384` accepts 512–1048576 bytes. `ring_sq_entries=2048` and `ring_cq_entries=8192` must be powers of two with CQ at least as large as SQ. `tick_ms=50` accepts 1–1000 ms. `drain_timeout_ms=10000` and `reload_ack_timeout_ms=5000` set shutdown and reload wait budgets. |
| Memory and pools | `runtime.relay_memory_mib=256` per worker backs L4 queues and HTTP Exchange bodies. `runtime.http1_stage_bytes=0` fixes the HTTP/1 stage budget at the startup `max_head_bytes + buffer_size`. An explicit budget must be at least that sum and at most 8 MiB. `[runtime.pools]` accepts `echo`, `l4` and `http1` reservations within the per-worker connection budget. A value of zero uses automatic sizing. The parser also reserves `http2` and `http3` pool keys for future adapters. |
| `[limits]` | Reloadable. Defaults in ms are `idle_timeout_ms=60000`, `header_timeout_ms=10000`, `connect_timeout_ms=3000` and `keepalive_timeout_ms=60000`. The first three must be at least `tick_ms`. Size defaults are `max_head_bytes=16384`, `max_headers=100` (HTTP/1 head parser ceiling 128), `max_uri_bytes=8192`, `queue_high_watermark=262144`, `queue_low_watermark=65536`. Keep low below high and high at least `buffer_size`. The startup stage budget caps future head-limit increases. `max_requests=1000` limits HTTP requests per client connection. `max_request_body_bytes=0` and `max_requests=0` mean unlimited for their respective settings. |
| `[[listener]]` | Unique `name`, numeric `address` (for example `[::1]:18080`), and `protocol` of `echo` (default), `l4` (alias `tcp`) or `http1`. A positive `max_connections` contributes to the per-protocol pool budget per worker unless overridden by `[runtime.pools]`. The default of zero takes a share of the remaining budget. L4 requires `upstream`. HTTP/1 uses `route` to select a route set. The `tls` field is reserved for a future milestone. |
| `[[upstream]]` | Unique `name`, 1–64 concrete numeric `backends` with nonzero ports, and `policy`. The default is `round_robin` (alias `rr`). Use `least_conn` (alias `least_connections`) for least-connections selection. `connect_timeout_ms=0` inherits the global connect timeout. Positive overrides must be at least `tick_ms`. `max_fails=3` and `eject_time_ms=10000` govern passive connect-failure ejection. Set `max_fails=0` to disable ejection. |
| `[[route_set]]` | Unique `name`, with one or more `[[route_set.vhost]]` tables. A vhost declares `hosts` (exact names and `*.example.com`-style patterns), `default = true`, or both. Exact host wins, then longest matching wildcard suffix, then the default. Each vhost has one or more `[[route_set.vhost.location]]` entries with a canonical absolute `path`, an `action` from the table below, and `match = "prefix"` (default) or `"exact"`. Exact paths win over the longest segment prefix. Query strings are excluded from route lookup. A configured route miss returns 404. An HTTP listener without a route returns 501. |
| `[access_log]` | Optional and restart-only. Requires `path`. `rotate_bytes` and `rotate_files` both default to zero for append-only mode. Set both to positive values for rotation. |

For CPU pinning, run `python3 tools/coring5_cpus.py 4` and copy its output under `[runtime]`. Use `--node 0` to restrict the selection to a NUMA node. Run it under the service's effective affinity mask so the choices are available at startup.

| HTTP action | Fields and behavior |
| --- | --- |
| `respond` | Optional `status` (200–599, default 200) and `body` (default empty, up to 1 MiB). Status 204 and 304 require an empty body. |
| `redirect` | Required `target` sets the Location header and accepts up to 8192 printable ASCII bytes without spaces. `status` is 301, 302 (default), 303, 307 or 308. |
| `static_file` | `root` is an absolute directory path. The full request path maps beneath the root, including the location prefix. Coring opens regular files beneath this directory, rejects symlinks, and supports GET, HEAD, a single byte range, weak ETag and Last-Modified validators. It reads metadata from the opened fd on each request. |
| `reverse_proxy` | `upstream` names a backend group. The proxy forwards the original target, strips hop-by-hop fields, regenerates forwarding and framing headers, and streams through bounded queues. HTTP/1.1 upstream sockets can be reused after complete uploads and persistent, self-delimited responses. The worker probes cached sockets before reuse. An idempotent request can be retried once on transient failure within the connect budget before any upstream write succeeds. A bodyless HTTP/1.1 GET with `Connection: Upgrade`, a single `Upgrade: websocket`, and no `Expect`, `Content-Length` or `Transfer-Encoding` switches both directions to raw bytes after a matching upstream 101. Other protocol upgrades and CONNECT are unsupported. |

HTTP/1 client pipelining is serial. The server accepts Content-Length and chunked request bodies, validates and drops request trailers, and supports `Expect: 100-continue`. For chunked upstream responses, an HTTP/1.1 client can opt into permitted response trailers with `Connection: TE` and `TE: trailers`. The proxy regenerates these fields for the upstream hop and tracks forwarded, dropped and rejected trailers in stats. An early final response closes the client connection after the response. See [the shipped reverse-proxy example](deploy/coring.example.toml) for a single-route configuration.

## Reload, access logs and shutdown

- Save the full updated TOML at the original path and run `"$bin/coringd" --config "$cfg" --check-config`. Use `ctl reload` or send SIGHUP with `kill -HUP "$daemon_pid"`. SIGHUP writes the reload result to the daemon log. Limits, listeners, routes and upstreams can change within startup capacity. Kept HTTP connections select current routes between requests. Runtime or access-log settings require a restart. Parse, validation and preparation errors leave the old configuration serving. Check for `acked=true` and use `ctl status` to check the published generation. With `acked=false`, the wait timed out and workers can still apply the update. A worker apply error can leave a partially applied configuration even when status shows the new generation.
- `[access_log]` records HTTP/1 requests. Set `path` to a regular file in an existing writable directory. Omit both rotation settings or set both to zero for append-only logging. Set positive `rotate_bytes` and `rotate_files` (1–255) to rotate before a new record would exceed the limit. An oversized record gets its own file. `.1` is the newest archive. The writer keeps the fd open and owns rotation. With rotation enabled the log path must be free of a trailing symlink.
- Log rows are tab-separated. Fields are request-start Unix ms, elapsed µs, listener ID, ingress and egress byte counts, status, method and request target. Method and target are bounded, and the writer percent-escapes control/non-ASCII bytes and percent signs. A full worker log ring drops records and increments `coring_access_log_dropped_total`. Check `coring_access_log_sink_errno` for writer failures.
- `ctl shutdown`, SIGTERM and SIGINT stop accepts and drain active work up to `drain_timeout_ms`, then cancel outstanding I/O and reap connections. The writer flushes queued access records after producers stop. In the demo, run `ctl shutdown`, then `wait "$daemon_pid"` and stop the Python backend with `kill "$origin_pid"`. A log sink failure makes the daemon exit with status 1.
- For a systemd deployment, review the supplied unit. Install the binaries with `cmake --install "$out/release" --prefix /usr/local` and point both start commands at `/usr/local/sbin/coringd`. Set the config path and give the service user access to static roots, the socket directory and a writable log directory under the unit's filesystem restrictions. Keep `TimeoutStopSec` longer than the configured drain budget to allow for cleanup and the final log flush. [Supplied systemd unit](deploy/coringd.service)

## L4 relay and benchmarks

The following standalone configuration proxies a TCP echo backend. Save it as a separate file and start a second daemon using a different control socket.

```toml
[runtime]
workers = 2
max_connections_per_worker = 512
buffer_count = 256

[[listener]]
name = "relay"
protocol = "l4"
address = "127.0.0.1:19000"
upstream = "echo"

[[listener]]
name = "backend"
protocol = "echo"
address = "127.0.0.1:19001"

[[upstream]]
name = "echo"
backends = ["127.0.0.1:19001"]
```

The benchmark harness runs directly with Python. The CMake integration targets for focused checks are `coring5_it_echo`, `coring5_it_l4`, `coring5_it_http1_handlers` and `coring5_it_reload`. Use a Release build for measurements.

```bash
# One request/response per TCP connection through the relay.
python3 bench/coring5/tcp_load.py 19000 --connections 1000000 --clients 16 --bytes 3
# Keep one TCP connection open per load process for the direct echo benchmark.
python3 bench/coring5/tcp_load.py 19001 --seconds 20 --clients 16 --bytes 1024
# HTTP keep-alive benchmark against the demo server.
oha -z 30s -c 64 http://127.0.0.1:18080/
```

For the churn run, record `ls /proc/<daemon-pid>/fd` before and after traffic with admin clients disconnected. Query `stats` after quiescence and verify that live connections, relay chunks in use and stale events are zero. Benchmark echo with one, two and four workers using the same client processes and payload. Pin clients to different cores from workers for each run. Record throughput, load, metrics and the host configuration.

For a bandwidth comparison, run `iperf3 -s -B 127.0.0.1 -p 5201`, change the relay upstream to `127.0.0.1:5201`, and reload. Compare `iperf3 -c 127.0.0.1 -p 5201 -t 20 -P 4` directly with `iperf3 -c 127.0.0.1 -p 19000 -t 20 -P 4` through the relay. Use the same run length, parallelism and CPU placement. To compare HTTP with nginx, use the same inline body, pinned worker count, keep-alive settings, client concurrency, warmup and measurement duration for both.

M3 physical-link throughput acceptance is deferred because the audit setup has one host. The recorded loopback measurements characterize that host. The physical-link check needs endpoints on a representative link and a direct baseline over the same network path. [Milestone status and measurements](TODO.md)

## Troubleshooting and invariants

| Symptom | Check |
| --- | --- |
| Worker startup fails at `io_uring` with `EPERM` or tests skip | Check kernel, seccomp, `kernel.io_uring_disabled` and process permissions. Run real-ring checks on a permitted host. |
| Cannot pin to CPU | Compare `[runtime].cpus` with the effective affinity mask and cpuset. |
| Bind or socket failure | Check port occupancy, numeric addresses, control socket parent permissions and whether another instance is using the path. |
| 404 or 501 | Check the listener's route binding, Host and path selection. For static files, confirm the full path beneath the root and the daemon user's permissions. |
| 502, 503, 504 or early connection close | Check the origin, upstream framing and timeout metrics. Inspect connection rejections, accept errors, `coring_nobufs_total`, `coring_sq_full_total`, `coring_cq_overflow_total` and `coring_relay_shed_total` for capacity pressure. |
| Config check passes and reload fails | Check restart-only settings, startup pool/stage sizes, bind and directory access, worker ACK status and the running generation. |
| `coring_stale_events_total` grows | Own-operation completions must resolve to live generation handles. Capture the workload and worker stats for a lifecycle regression. |

For I/O changes, verify that every successfully queued operation is counted, closing waits for all completions, reaping follows the CQE batch, and pool slots and buffer leases return to their initial counts after drain. Keep data-path memory bounded by fixed pools and queue watermarks, arm deadlines for waits, and run both FakeRing schedules and real-ring tests. Keep configuration preparation on the control thread and preserve the sans-I/O module boundaries.
