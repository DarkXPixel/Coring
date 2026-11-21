#pragma once

// HTTP/1.1 request body framing, Content-Length part (M4.1b). Sans-IO and
// allocation-free: it only counts bytes. The caller claims credit on the
// BodyStream, receives bytes, asks `take(avail)` how many of them belong to
// this body, pushes exactly that prefix, and calls BodyStream::finish() once
// `done()`. Bytes beyond the body (pipelined next request) are never taken.
//
// Strict: the declared length comes from the already-validated RequestHead
// (the head parser rejects bad syntax, overflow, duplicate/conflicting CL and
// CL+TE). `begin()` re-checks the combination defensively.

#include "coring5/base/Result.hpp"
#include "coring5/proto/http1/Parser.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace Coring5::Http1 {

enum class BodyKind : uint8_t { None, ContentLength, Chunked };

class ContentLengthFramer {
public:
  // Starts a body of exactly `total` bytes (0 is complete immediately).
  constexpr void start(uint64_t total) noexcept {
    declared_ = total;
    remaining_ = total;
  }

  // Bytes of `avail` that belong to the body (never more than remaining).
  [[nodiscard]] constexpr std::size_t take(std::size_t avail) noexcept {
    const uint64_t n = std::min<uint64_t>(avail, remaining_);
    remaining_ -= n;
    return static_cast<std::size_t>(n);
  }
  // Upper bound for the next claim: no point reading past the body (the rest
  // of the buffer belongs to the next pipelined request).
  [[nodiscard]] constexpr uint64_t remaining() const noexcept {
    return remaining_;
  }
  [[nodiscard]] constexpr uint64_t declared() const noexcept {
    return declared_;
  }
  [[nodiscard]] constexpr bool done() const noexcept { return remaining_ == 0; }

  // Peer closed the transport: ok only if the body is complete.
  [[nodiscard]] constexpr Errc on_eof() const noexcept {
    return remaining_ == 0 ? Errc::Ok : Errc::ProtocolViolation;
  }

private:
  uint64_t declared_{0};
  uint64_t remaining_{0};
};

struct BodyPlan {
  BodyKind kind{BodyKind::None};
  uint64_t length{0};
  Errc error{Errc::Ok};
};

// Derives the body framing of a parsed request head. Rejects (never
// normalises) a head carrying both Content-Length and chunked.
[[nodiscard]] constexpr BodyPlan begin_body(const RequestHead &h) noexcept {
  if (h.chunked && h.has_content_length) {
    return {BodyKind::None, 0, Errc::ProtocolViolation};
  }
  if (h.chunked) {
    return {BodyKind::Chunked, 0, Errc::Ok};
  }
  if (h.has_content_length && h.content_length > 0) {
    return {BodyKind::ContentLength, h.content_length, Errc::Ok};
  }
  return {BodyKind::None, 0, Errc::Ok};
}

} // namespace Coring5::Http1
