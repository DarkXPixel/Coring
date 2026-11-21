# Active Task

M4.0 / M4.1 — HTTP/1.1 body framing & Exchange layer (see docs/ROADMAP.md §3 M4).

## Goal

Finalize ADR 0010 (Exchange layer), then deliver sans-IO HTTP/1.1 body framing
(Content-Length, chunked, trailers), response serializer, server session, handlers.
Acceptance: parser fuzz clean 1 h; smuggling corpus rejected; first bench vs nginx recorded.
Side items: M1 `FakeRing` + randomized lifecycle tests; M2 event-driven `ControlServer`.

## Target Files

(Layout: headers `include/coring5/<mod>/`, sources `src/coring5/<mod>/`, tests `tests/coring5/`.)

- `include/coring5/proto/http1/` + `src/coring5/proto/http1/` (BodyFraming, ChunkedDecoder, ResponseWriter)
- `include/coring5/proto/http1session/` + `src/coring5/proto/http1session/` (ServerSession, sans-IO)
- `include/coring5/http/` + `src/coring5/http/` (BodyStream, Exchange, ExchangePool)
- `include/coring5/mem/` (ChunkQueue, ChunkPool)
- `tests/coring5/unit/` (chunked/framing/session tests), `tests/coring5/integration/`
- S2 only: `proto/http1conn`, `runtime/Worker`, `io/` (FakeRing)
- TBD: `src/coring5/routing/` -> fold into ConfigSnapshot; ControlServer location

## Current State & Progress

- [ ] M4.0 Accept/revise ADR 0010 (user decision; do not edit docs unless asked)
- [x] M4.0a OpTag split (`io/OpTag.hpp`, `optag_test.cpp`); [ ] M4.0b ADR 0010 still PROPOSED (user decision)
- [x] M4.1.0 BodyStream (`include/coring5/http/BodyStream.hpp`, `http/BodyStream.cpp`, `body_stream_test.cpp`)
- [x] M4.1a Exchange state machine (EXCHANGE.md §4; `http` deps base/mem/config only; sessions/Worker untouched)
  - [x] `http/ReadyList.hpp`: preallocated ring of Handle, cap = pool cap; one entry per live Exchange
  - [x] `http/Exchange.{hpp,cpp}`: Phase, Side, StreamId, RequestMeta/ResponseMeta, uint16_t event bits, `status_for`, `content_forbidden`
  - [x] E1 ctor, E2 `attach_responder`, E2b `respond_synthetic`, E3 `publish_interim`, E4 `publish_final` (R1 gate, no-content => Finished)
  - [x] Stream wrappers (claim/push/finish/consume/wait_readable/abort) -> `harvest()` raises events AFTER state change
  - [x] E6 `cancel_request_body`; E7/E8/E9/E10 via idempotent `detach(who, why)` (R3, always raises an event)
  - [x] `http/ExchangePool.{hpp,cpp}`: `open` (full => invalid, ingress sheds 503), `get`, `has_ready`, `drain(budget, deliver)` reaps (E11)
  - [x] No timers in Exchange; expose `consumed(StreamId)`; sessions own deadlines and call `detach(self, Timeout)`
  - [x] `tests/coring5/unit/exchange_test.cpp`: E1..E11, R1..R5, early response, 502/503/504, post-head abort, stale handle skip
  - [x] `tests/coring5/unit/exchange_random_test.cpp`: seeded; SAFETY/CLEAN/LOSTWAKE; action labels = tools/exchange_model.py
  - [x] CMake: add sources to `coring5_module(http)`, tests to `coring5_unit_http`; `tools/check_layering.py` 0 violations; -Werror
  - [ ] Decide: Shutdown->503, interim coalescing, forced detach, head-byte storage (open Q4)
  - [ ] Out of scope: trailer block/validator, Worker ready-list drain + non-blocking submit, sessions, obs counters
- [x] M4.1b Content-Length body framing + unit tests (`proto/http1/BodyFraming.hpp`, `body_framing_test.cpp`)
- [ ] M4.1c Chunked decoder state machine (ext, trailers, strict rejects); sans-IO, deps base+config ONLY (no http/mem)
  - [x] `proto/http1/ChunkedDecoder.{hpp,cpp}`: `ChunkedDecoder::feed(in, max_data) -> ChunkedStep{event,error,reason,consumed,data}`
        events NeedMore|NeedCredit|Data|Done|Error; one data run per step (zero-copy span of `in`); resumable at any byte; fixed state, 0 heap
  - [x] Strict: HEXDIG only, <=16 digits (no 0x/+/-/SP/BWS), CRLF only (bare LF/CR rejected), ext syntax validated + dropped (`max_ext_bytes`)
  - [x] Trailers: validated (token name, no obs-fold, no SP before ':', no CTL), forbidden framing/hop-by-hop names rejected,
        `max_headers`/`max_head_bytes` per block; contents discarded + counted (request trailers dropped, EXCHANGE.md 5.3)
  - [x] Errors: size/limit overflow -> `Errc::TooLarge`; all else + premature EOF (`on_eof`) -> `Errc::ProtocolViolation`; sticky Failed; `reason` enum for tests
  - [x] Caller contract (ServerSession implements it; tick after S1 tests pass): claim -> feed -> `push`(Data, settles claim) / `finish(false)`(Done) / `abort(Req, err)`; no `set_declared` for chunked
  - [x] CMake: ChunkedDecoder.cpp into `proto_http1` (unit target pending); target `coring5_unit_chunked` (LIBS coring5_http coring5_proto_http1); layering 0 violations; -Werror
  - [x] `tests/coring5/unit/chunked_decoder_test.cpp`: happy path, split invariance (byte-by-byte + seeded random splits), pipelined tail,
        NeedCredit/max_data, hex overflow, ext accept/reject, trailers accept/reject/limits, EOF at every state, sticky error, BodyStream integration + pool leak check
  - Note: bare LF right after chunk-size digits -> `BadChunkSize`; `max_ext_bytes` counts the terminating `\r` (`;abc\r` = 5 bytes)
  - [x] Decided: BWS in ext rejected, `max_ext_bytes` = 1024, config key `limits.max_request_body_bytes` (0 = unlimited)
  - [ ] Out of scope: stored trailer block, session wiring/tail staging, smuggling corpus (M4.1d), fuzz target (M4.1e), HTTP/1.0+chunked check

- [x] M4.1d Smuggling corpus tests (CL+TE, bad chunk sizes, obs-fold)
  - [x] `tests/coring5/unit/smuggling_corpus_test.cpp` (target `coring5_unit_smuggling`): CL+TE, dup/conflicting CL, bad CL/TE syntax, obs-fold, bare LF, dup Host, bad chunk size/CRLF/trailers; `HeaderSyntaxRejected` asserts explicit codes (`BadHeaderName` for SP before ':', `BadHeader` for NUL in value)
  - [x] Parser: HTTP/1.0 + `Transfer-Encoding: chunked` -> `BadTransferEncoding`
  - [x] Build OK, layering 0 violations; pre-existing failures in `coring5_unit_http1` (KeepAliveRules, BodyFramingHeaders: test heads lack Host on HTTP/1.1) not fixed
- [x] M4.1e Fuzz target for body framing (`tests/coring5/fuzz/http1_body.cpp`, `fuzz_http1_body`; opt-in CORING5_FUZZ)
- [x] Response serializer (`proto/http1/ResponseWriter.{hpp,cpp}`; `response_writer_test.cpp`, target `coring5_unit_response_writer`)
- [ ] Server session (keep-alive, pipelining policy, `Expect: 100-continue`)
  - Decided defaults: `max_requests = 1000`; `limits.max_request_body_bytes` (0 = unlimited); stage chunk = `max_head_bytes + buffer_size`;
    BWS in chunk-ext rejected; `max_ext_bytes = 1024`; early final response => close; only `100` interim forwarded
  - [ ] S0 prerequisites
    - [x] Fix `coring5_unit_http1` KeepAliveRules/BodyFramingHeaders test heads (add Host) (test pass, left for DeepSeek)
    - [x] `limits.max_request_body_bytes` + `max_requests` (Snapshot, Loader parse; Diff via Limits `==`); [x] config tests
    - [x] Validate `queue_high_watermark >= buffer_size`; stage sizing check (`max_head_bytes + buffer_size` <= 8 MiB); [ ] worker creates stage ChunkPool (S2)
    - [x] Module `proto/http1session`: CMake, `tools/check_layering.py` (+http/http1session for http1conn/runtime), 0 violations
  - [x] S1 sans-IO `ServerSession` implemented (builds -Werror clean); [x] unit tests pending (DeepSeek)
    - [x] Rx states, head staging, `on_data` consumes all input, RequestMeta, 503/400/414/431/413/417 + close
    - [x] Caller contract: prepare_recv (claim) -> on_data (push settles / release) -> finish / abort; Blocked parks tail in stage, resumes on ReqWritable
    - [x] Tx: static 100, head via ResponseWriter (+ small-body coalescing), CL/chunked payload from front(Res) + consume in on_sent, abort after head -> close
    - [x] `keep_alive_after` constexpr predicate + `connection_token`; HTTP/1.0 close-delimited => close
    - [x] Pipelining serial; no recv while Held; early final response => close + cancel_request_body
    - [x] Expect rules; [x] Deadlines + `on_drain`
    - [x] `tests/coring5/unit/server_session_test.cpp` (target `coring5_unit_server_session`; end state: pools full, live()==0)
    - [x] Tick M4.1c "Caller contract" + parent when tests pass
  - [ ] S2 wiring (code done, build + layering + ctest green; tests left for DeepSeek)
    - [x] `runtime.http1_stage_bytes` (+ `http1_stage_effective`, restart-only, checked vs `max_head_bytes + buffer_size` on load/reload); `ExchangePool::set_limits`
    - [x] `ServerSession::close_reason()`, `refresh_limits()` (between requests only)
    - [x] `Ring::submit_nowait()`, `io/Cqe.hpp`, `io/RingSeam.hpp`, `io/FakeRing.hpp` (`CORING5_FAKE_RING`)
    - [x] `Http1Conn` on ServerSession (still synthetic 501 until handlers), `ConnPool/ConnPools::on_exchange`
    - [x] Worker: `exchanges_`, `stage_`, `drain_exchanges()` before `reap_closed()`, nowait submit when `has_ready()`, shutdown needs `live()==0`
    - [x] FakeRing lifecycle test (close at every state); INVARIANTS checklist walk-through
          (`tests/coring5/unit/http1_conn_fake_test.cpp` + `http1_conn_harness.hpp`: seeded 1000 seeds x <=12 steps,
          close trigger from every state, short sends, late/stale CQEs, timer/drain/EOF/error; end state
          live()==0, inflight==0, stale_events==0, pools full, no leaked fds)
- [x] Handlers: Respond / Redirect (`http/Handlers.{hpp,cpp}`; `tests/coring5/unit/handlers_test.cpp`, target `coring5_unit_handlers`; cold-path validation + apply() state machine, 32 tests)
- [ ] Handlers: StaticFile (statx cache, Range, ETag, path-escape tests)
- [ ] Handlers: ReverseProxy (hop-by-hop strip, X-Forwarded-*, retries idempotent only)
- [ ] Router integration into ConfigSnapshot
- [ ] M1: `FakeRing` (io/FakeRing.hpp + RingSeam done; [x] seeded lifecycle tests, Worker not yet templated on the seam)
- [ ] M2: event-driven `ControlServer`; access-log SPSC writer

## Key Decisions & Invariants

- C++23; work only in the `coring5/` tree (not legacy `src/` roots outside it).
- `-Werror` on; follow `.clang-tidy` / clang-format.
- Errors via `Result<T> = std::expected<T, Error>`; no exceptions/terminate on data path.
- Parsers/framers are sans-IO: bytes in, events out; strict, reject never normalize.
- No heap allocation on data path; fixed pools; bounded queues with watermarks.
- `HandlePool` handles `{index, gen32}`, gen 0 invalid; stale own-op events must stay 0.
- Wrap every submission in `core.counted(queued)`; no `on_cqe` in Closing.
- `FakeRing` mirrors `Ring` API; end state: live()==0, inflight==0, stale_events==0.
- Layering: deps point downward; `http`/`mem`/`config` must not depend on `io`.
- Every wait has a deadline (`DeadlineKind`).

## Blockers / Last Compiler Error

None
