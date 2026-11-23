#pragma once

// HTTP/1.1 request-head parser (sans-IO, allocation-free). M4.
//
// STATUS: the HEAD parser is implemented and strict; body framing (chunked
// decoder, Content-Length tracking) is TODO(M4). Strictness policy (request
// smuggling defence): REJECT, never normalise:
//   * bare LF, bare CR, NUL, DEL, control characters, obs-fold, whitespace
//     before ':'; bytes >= 0x80 are allowed only inside header VALUES
//   * duplicate Host, and a missing/empty Host on HTTP/1.1
//   * Content-Length + Transfer-Encoding together, duplicate/conflicting CL
//   * Transfer-Encoding other than exactly "chunked"
//   * request target / header count / head size over the configured limits
//
// All string_views in RequestHead point INTO the buffer passed to parse_head();
// they are valid only while that buffer is.

#include "coring5/config/Snapshot.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace Coring5::Http1 {

enum class ParseStatus : uint8_t { NeedMore, Done, Error };

enum class ParseError : uint8_t {
  None,
  HeadTooLarge,
  TooManyHeaders,
  UriTooLong,
  BadRequestLine,
  BadMethod,
  BadVersion,
  BadHeader,
  BadHeaderName,
  BadHeaderValue,
  ObsFold,
  BareLf,
  ConflictingLength,
  BadTransferEncoding,
  BadContentLength,
  BareCr,
  DuplicateHost,
  MissingHost,
};

struct Header {
  std::string_view name;
  std::string_view value;
  [[nodiscard]] bool named(std::string_view key) const noexcept;
};

inline constexpr std::size_t kMaxHeadersHard = 128;
// Distinct tokens remembered from all `Connection` headers (they name the
// hop-by-hop headers a proxy must strip). More than this is rejected.
inline constexpr std::size_t kMaxConnectionTokens = 8;

struct RequestHead {
  std::string_view method;
  std::string_view target;
  uint8_t minor_version{1}; // HTTP/1.<minor>
  std::array<Header, kMaxHeadersHard> headers;
  std::size_t header_count{0};

  bool chunked{false};
  bool has_content_length{false};
  uint64_t content_length{0};
  // Effective persistence: HTTP/1.1 stays open unless a `close` token is
  // present; HTTP/1.0 closes unless a `keep-alive` token is present.
  bool keep_alive{true};
  bool upgrade{false}; // `upgrade` token present in Connection
  std::string_view host;
  // Every token of every `Connection` header, in order (views into the input).
  std::array<std::string_view, kMaxConnectionTokens> connection_tokens;
  std::size_t connection_token_count{0};

  [[nodiscard]] std::string_view
  find(std::string_view lower_name) const noexcept;
};

struct HeadResult {
  ParseStatus status{ParseStatus::NeedMore};
  ParseError error{ParseError::None};
  std::size_t consumed{0}; // bytes of head including the terminating CRLFCRLF
};

// Parses a complete request head from `in`. `in` must be the WHOLE head so far
// (the caller accumulates partial heads in a bounded staging buffer, invariant
// I10). Returns NeedMore until CRLFCRLF is seen; Error as soon as a violation
// is detected (do not wait for more bytes).
[[nodiscard]] HeadResult parse_head(std::span<const char> in,
                                    const Limits &limits, RequestHead &out);

// The response adapter uses the same strict field and framing parser.
[[nodiscard]] HeadResult parse_response_head(std::span<const char> in,
    const Limits &limits, RequestHead &fields, uint16_t &status);

} // namespace Coring5::Http1
