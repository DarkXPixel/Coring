#pragma once

// Sans-IO static handlers: Respond (fixed status/headers/body) and Redirect.
// Built and validated on the cold path (config/control thread); the hot path
// (`apply`) allocates nothing: it publishes the head on an Exchange and pushes
// the preconfigured body through the credit protocol (resumable on Blocked).
//
// Headers are exposed via `headers()` for the session's head serializer
// (Exchange stores no head bytes yet). Framing headers (Content-Length,
// Transfer-Encoding) and hop-by-hop headers are rejected: the serializer owns
// them. Names must be tokens; values must not contain CTLs (CR/LF/NUL; HTAB
// allowed) -> no response splitting.

#include "coring5/base/Result.hpp"
#include "coring5/http/Exchange.hpp"
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Coring5 {

struct StaticHeader {
  std::string name;
  std::string value;
};

enum class ApplyStatus : uint8_t {
  Done,    // head published and body fully pushed + finished
  Blocked, // wait for ResWritable, then call apply again with the same cursor
};

// Resume state for Respond::apply. Default-constructed = start.
struct ApplyCursor {
  std::size_t sent{0};
  bool head_done{false};
};

inline constexpr std::size_t kMaxStaticHeaders = 32;
inline constexpr std::size_t kMaxStaticHeaderBytes = 8192;
inline constexpr std::size_t kMaxStaticBodyBytes = 1u << 20;
inline constexpr std::size_t kMaxLocationBytes = 8192;

[[nodiscard]] bool valid_static_header_name(std::string_view n) noexcept;
[[nodiscard]] bool valid_static_header_value(std::string_view v) noexcept;
// Forbidden: framing + hop-by-hop headers (case-insensitive).
[[nodiscard]] bool forbidden_static_header(std::string_view n) noexcept;

class Respond {
public:
  // Validates everything; status in [200,599] (no 1xx); 204/304 need empty
  // body.
  [[nodiscard]] static Result<Respond>
  make(uint16_t status, std::vector<StaticHeader> headers, std::string body);

  [[nodiscard]] uint16_t status() const noexcept { return status_; }
  [[nodiscard]] std::span<const StaticHeader> headers() const noexcept {
    return headers_;
  }
  [[nodiscard]] std::string_view body() const noexcept { return body_; }

  // Routing/Active -> publishes final head, pushes body, finishes.
  // Errors: WrongPhase/Gone/aborted stream -> Errc::Canceled.
  [[nodiscard]] Result<ApplyStatus> apply(Exchange &ex,
                                          ApplyCursor &cur) const noexcept;

private:
  uint16_t status_{0};
  std::vector<StaticHeader> headers_;
  std::string body_;
};

class Redirect {
public:
  [[nodiscard]] static constexpr bool valid_status(uint16_t s) noexcept {
    return s == 301 || s == 302 || s == 303 || s == 307 || s == 308;
  }
  // Location: non-empty, <= kMaxLocationBytes, only visible ASCII (no SP,
  // CTL, DEL, or >= 0x80: callers must percent-encode).
  [[nodiscard]] static bool valid_location(std::string_view loc) noexcept;

  [[nodiscard]] static Result<Redirect> make(uint16_t status,
                                             std::string_view location);

  [[nodiscard]] uint16_t status() const noexcept { return resp_.status(); }
  [[nodiscard]] std::string_view location() const noexcept {
    return resp_.headers().front().value;
  }
  [[nodiscard]] const Respond &response() const noexcept { return resp_; }
  [[nodiscard]] std::span<const StaticHeader> headers() const noexcept {
    return resp_.headers();
  }
  [[nodiscard]] Result<ApplyStatus> apply(Exchange &ex,
                                          ApplyCursor &cur) const noexcept {
    return resp_.apply(ex, cur);
  }

private:
  explicit Redirect(Respond r) : resp_(std::move(r)) {}
  Respond resp_;
};

} // namespace Coring5
