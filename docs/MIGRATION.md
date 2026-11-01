# Migrating from Coring4

Audit of the Coring4 prototype (`include/coring4`, `src/Coring4`) and what
happened to each piece. "Fixed by construction" means the new Coring5 code does
not have the defect and a test or design rule prevents its return; the Coring4
files are untouched and still build with `CORING_BUILD_LEGACY=ON`.

## 1. Defects found in Coring4

| # | Where | Defect | Coring5 |
|---|---|---|---|
| 1 | `Worker::on_io` | `Send`, `Recv`, `StopSignal`, `Close`, `Cancel`, `Connect`, `Read`, `Write` all fell through to `on_manage()`: every recv CQE would read the eventfd; `StopSignal` never stopped the loop | `Worker::dispatch` switch with one handler per `OpKind`; shutdown is a mailbox message |
| 2 | `SessionStorage` ctor | `resize(N)` then `push_back` ⇒ N bogus zero entries at the bottom of the free list; index 0 handed out again while live | `HandlePool` uses `reserve`; test `EveryIndexIsHandedOutExactlyOnce` |
| 3 | `ProvidedBufferPool::recycle_buffer` | passed the buffer **count** as the buffer **length** to `io_uring_buf_ring_add`; class had a destructor but copyable (double free), never unregistered the ring | `BufferRing::recycle` passes `buf_size`; move-only; `io_uring_free_buf_ring` in the destructor |
| 4 | `IOUringBackend` | no destructor (ring leaked); move ctor copied the struct, leaving `config` behind | `Ring` is non-copyable/non-movable (held in `unique_ptr`), `~Ring` calls `io_uring_queue_exit` |
| 5 | `IOUringBackend::prep_*`, `Engine::get_sqe` | `io_uring_get_sqe` may return null; callers dereferenced it; `Engine` called `std::terminate()` | `Ring::sqe()` submits and retries; `prep_*` return `bool`; callers shed the connection; the worker re-arms its own ops later |
| 6 | `Worker::create` + `SINGLE_ISSUER` | static factory returned a `Worker` by value; a ring created in `main` and used on another thread fails with `-EEXIST` | `Worker` is constructed and `init()`ed on its own thread (`WorkerGroup::start`) |
| 7 | `Worker::on_accept`/`on_io` | freed the `IoOp` when `!more()`; an accept ending with `-EMFILE` was never re-armed ⇒ listener silently dead; `res < 0` never checked | accept errors counted; re-arm on the next tick when the multishot ended; same for the mailbox poll |
| 8 | `IoOp*` as `user_data` | no owner/generation: a CQE could not be routed to a connection | `OpTag` (gen/index/kind/aux) + `HandlePool` |
| 9 | `Listener2` | `create()` fell off the end for unknown transports (UB); `create_udp` returned a default object; move ctor dropped `tls_path`; `tls_path{false}` built a 1-char string; hard-coded `AF_INET`; not `SOCK_NONBLOCK\|SOCK_CLOEXEC`; mixed config with socket | `ListenerSpec` (data) + `create_tcp_listener` (socket, v4/v6, nonblocking, cloexec, all errors returned) |
| 10 | duplicates | two `OpCode`s, two `Listener`s (`ManageService.hpp` still included the legacy one), three conflicting watermark constants, a CMake target mixing four generations of code | one `OpKind`, one set of watermarks in `Limits`, per-module libraries |
| 11 | `main.cpp`, `ControlServer` | `CoringDaemon` was an atomic flag; no workers started; control server was blocking/one-client, exact-match commands (`"PING\n"` failed), no `chmod`/peer-credential check; signals polled every 500 ms | `Daemon` orchestrates; `signalfd`; trimmed commands; mode 0660 + `SO_PEERCRED` (still blocking: roadmap M2) |
| 12 | `WorkerChannel` | `ev_fd` never closed | `Mailbox` owns it through `FdOwner` |
| 13 | `Engine::process_*` | `std::visit` fan-out multiplies handlers by variant alternatives; bit-field `UserData` relies on implementation-defined layout | virtual `Connection::handle_cqe`; explicit shifts in `OpTag` |

## 2. Where each Coring4 piece goes

| Coring4 | Coring5 / action |
|---|---|
| `routing/*` (`RouteTable`, `PathNormalizer`, `RouteHash`, `WorkerRouting`, `RoutingControl`, …), `routing_test`, `routing_bench` | **Keep as is**; migrate under `coring5/routing` in M4 and fold into `ConfigSnapshot` (see CONFIG_AND_RELOAD §5). `routing_test` still runs in the Coring5 ctest. |
| `core/IpEndpoint.hpp` | Superseded by `base/Endpoint`; the old one can serve as reference for hostnames/IPv6 scope ids when DNS arrives |
| `IoUring/IoUringBackend.hpp` | `io/Ring` |
| `ProvidedBufferPool.hpp` | `io/BufferRing` + `mem/Region` |
| `SessionStorage.hpp`, `storage/IndexObjectPool.hpp`, `ObjectPool.hpp` | `mem/HandlePool` |
| `Worker/Worker.hpp` | `runtime/Worker` |
| `ManageService.hpp`, `WorkerChannel` | `runtime/Mailbox` + `ConfigManager` |
| `listeners/Listener.hpp`, `ListenerConfig.hpp` | `config/Snapshot.hpp` (`ListenerSpec`) + `net/ListenSocket` |
| `EgressQueue.hpp`, `StackRingBuffer.hpp` | Idea (64-byte chunk descriptors, watermarks) folds into `Relay` (M3); drop the Boost dependency |
| `BufferPool.hpp`, `ThreadLocalSlabPool.hpp`, `BufferHandle.hpp` | Replaced by `BufferRing::Lease`; revisit only if a general slab is needed for staging buffers |
| `application/ControlServer.hpp`, `CoringDaemon.hpp`, `src/cli` | `control/ControlServer`, `runtime/Daemon`, `apps/coringctl` |
| `Engine.hpp`, `Router.hpp`, `Listener.hpp`, `Sessions.hpp`, `ThreadWorker.hpp`, `UpstreamPool.hpp`, `UpstreamConnection.hpp`, `Http1Stage.hpp`, `src/Coring4/Engine*.cpp`, `src/coring`, `coring2`, `coring3`, `coring_uring` | Prototype code: quarantine. Delete after M4 works (plan: `git mv` to `legacy/`, then remove) |

## 3. Removing legacy

1. Today: legacy targets build by default (`CORING_BUILD_LEGACY=ON`); Coring5
   presets turn them off.
2. After M4 (HTTP/1.1 working): `git mv` the legacy sources into `legacy/`,
   keep them only as reference for a release.
3. After T1 (M6): delete `legacy/` and the `CORING_BUILD_LEGACY` option.

Note for existing local config: the old `/home/darkxpixel/...` static root and
the hard-coded `localhost:8088` route in `Engine::create()` have no equivalent;
routes will come from the config file in M4.
