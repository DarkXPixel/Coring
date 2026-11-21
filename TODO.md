# Active Task

M4.0 / M4.1 — HTTP/1.1 body framing & Exchange layer (see docs/ROADMAP.md §3 M4).

## Goal

Finalize ADR 0010 (Exchange layer), then deliver sans-IO HTTP/1.1 body framing
(Content-Length, chunked, trailers), response serializer, server session, handlers.
Acceptance: parser fuzz clean 1 h; smuggling corpus rejected; first bench vs nginx recorded.
Side items: M1 `FakeRing` + randomized lifecycle tests; M2 event-driven `ControlServer`.

## Target Files

(Layout: headers `include/coring5/<mod>/`, sources `src/coring5/<mod>/`, tests `tests/coring5/`.)

- `include/coring5/proto/http1/` + `src/coring5/proto/http1/` (BodyFraming, ChunkedDecoder)
- `include/coring5/http/` + `src/coring5/http/` (BodyStream, Exchange)
- `include/coring5/mem/` (ChunkQueue)
- `include/coring5/io/` + `src/coring5/io/` (FakeRing seam, server session wiring)
- `tests/coring5/unit/` (chunked/framing tests), `tests/coring5/integration/`
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
  - [ ] Caller contract (session, later): claim -> feed -> `push`(Data, settles claim) / `finish(false)`(Done) / `abort(Req, err)`; no `set_declared` for chunked
  - [x] CMake: ChunkedDecoder.cpp into `proto_http1` (unit target pending); target `coring5_unit_chunked` (LIBS coring5_http coring5_proto_http1); layering 0 violations; -Werror
  - [x] `tests/coring5/unit/chunked_decoder_test.cpp`: happy path, split invariance (byte-by-byte + seeded random splits), pipelined tail,
        NeedCredit/max_data, hex overflow, ext accept/reject, trailers accept/reject/limits, EOF at every state, sticky error, BodyStream integration + pool leak check
  - Note: bare LF right after chunk-size digits -> `BadChunkSize`; `max_ext_bytes` counts the terminating `\r` (`;abc\r` = 5 bytes)
  - [ ] Decide: BWS in ext (default reject), forbidden trailer-name set, `max_ext_bytes` default 1024, config key for max body
  - [ ] Out of scope: stored trailer block, session wiring/tail staging, smuggling corpus (M4.1d), fuzz target (M4.1e), HTTP/1.0+chunked check

- [x] M4.1d Smuggling corpus tests (CL+TE, bad chunk sizes, obs-fold)
  - [x] `tests/coring5/unit/smuggling_corpus_test.cpp` (target `coring5_unit_smuggling`): CL+TE, dup/conflicting CL, bad CL/TE syntax, obs-fold, bare LF, dup Host, bad chunk size/CRLF/trailers; `HeaderSyntaxRejected` asserts explicit codes (`BadHeaderName` for SP before ':', `BadHeader` for NUL in value)
  - [x] Parser: HTTP/1.0 + `Transfer-Encoding: chunked` -> `BadTransferEncoding`
  - [x] Build OK, layering 0 violations; pre-existing failures in `coring5_unit_http1` (KeepAliveRules, BodyFramingHeaders: test heads lack Host on HTTP/1.1) not fixed
- [x] M4.1e Fuzz target for body framing (`tests/coring5/fuzz/http1_body.cpp`, `fuzz_http1_body`; opt-in CORING5_FUZZ)
- [x] Response serializer (`proto/http1/ResponseWriter.{hpp,cpp}`; `response_writer_test.cpp`, target `coring5_unit_response_writer`)
- [ ] Server session (keep-alive, pipelining policy, `Expect: 100-continue`)
- [ ] Handlers: Respond / Redirect
- [ ] Handlers: StaticFile (statx cache, Range, ETag, path-escape tests)
- [ ] Handlers: ReverseProxy (hop-by-hop strip, X-Forwarded-*, retries idempotent only)
- [ ] Router integration into ConfigSnapshot
- [ ] M1: `FakeRing` (same API as Ring, scripted CQEs) + seeded lifecycle tests
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
