# ADR 0005 — Sans-IO protocol state machines; reuse proven libraries

## Context
Protocol code that owns sockets is hard to test, fuzz and port between TCP, TLS
and QUIC transports. Hand-rolling HPACK, TLS or QUIC is a large security
liability for a small team.

## Decision
* Parsers/framers take bytes and return events; they never touch a socket or the
  ring. They are fuzzable in isolation (`fuzz_http1_head` is differential:
  segmentation must not change the verdict).
* HTTP/1.1: own strict parser (policy: *reject*, don't normalise — request
  smuggling is the main H1 proxy vulnerability class).
* HTTP/2: nghttp2. HTTP/3: ngtcp2 + nghttp3 (not msquic: it owns threads and
  sockets). TLS: OpenSSL memory BIOs. No home-grown crypto/HPACK/QUIC.
* `RequestHead` holds `string_view`s into the caller's buffer; the caller keeps
  partial heads in a bounded staging buffer (invariant I10).

## Consequences
* (+) Protocol logic is unit-testable and fuzzable without a kernel ring.
* (+) Same relay/backpressure logic serves H1 bodies, H2 streams, L4.
* (−) Lifetime of `string_view`s is the caller's responsibility (a real bug in
  this repo's first segmentation test: the buffer was a temporary).
* (−) Dependencies (OpenSSL, nghttp2, ngtcp2/nghttp3) must be tracked for CVEs.
