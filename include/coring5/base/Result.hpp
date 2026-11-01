#pragma once

// Error model for the data path (see docs/ARCHITECTURE.md, "Error model").
//
//  * No exceptions on worker threads.
//  * No heap allocation on failure paths: Error is 8 bytes, trivially copyable.
//  * Human readable strings exist only on the cold path (config, startup).

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>

namespace Coring5 {

enum class Errc : uint8_t {
  Ok = 0,
  Eof,                 // orderly end of stream
  Reset,               // peer reset / I/O error on the socket
  Timeout,             // any deadline (see docs: timeout table)
  Canceled,            // operation canceled by us
  NoBuffers,           // provided buffer ring exhausted (-ENOBUFS)
  Overloaded,          // pool/queue limit reached, load shed
  ProtocolViolation,   // malformed or forbidden input from the peer
  TooLarge,            // head/body/URI over a configured limit
  UpstreamUnavailable, // connect/health failure
  TlsFailure,
  InvalidArgument, // caller/config error
  Unsupported,     // kernel/feature/not-yet-implemented
  Shutdown,        // closed because of drain/stop
  Internal,
  Count_ // sentinel, keep last
};

inline constexpr std::size_t kErrcCount =
    static_cast<std::size_t>(Errc::Count_);

struct Error {
  Errc code{Errc::Internal};
  int sys{0}; // errno / -cqe->res when relevant, else 0
};

template <class T = void> using Result = std::expected<T, Error>;

[[nodiscard]] constexpr std::string_view to_string(Errc e) noexcept {
  switch (e) {
  case Errc::Ok:
    return "ok";
  case Errc::Eof:
    return "eof";
  case Errc::Reset:
    return "reset";
  case Errc::Timeout:
    return "timeout";
  case Errc::Canceled:
    return "canceled";
  case Errc::NoBuffers:
    return "no_buffers";
  case Errc::Overloaded:
    return "overloaded";
  case Errc::ProtocolViolation:
    return "protocol_violation";
  case Errc::TooLarge:
    return "too_large";
  case Errc::UpstreamUnavailable:
    return "upstream_unavailable";
  case Errc::TlsFailure:
    return "tls_failure";
  case Errc::InvalidArgument:
    return "invalid_argument";
  case Errc::Unsupported:
    return "unsupported";
  case Errc::Shutdown:
    return "shutdown";
  case Errc::Internal:
    return "internal";
  case Errc::Count_:
    break;
  }
  return "unknown";
}

[[nodiscard]] constexpr std::unexpected<Error>
unexpected_error(Errc code, int sys = 0) noexcept {
  return std::unexpected<Error>(Error{code, sys});
}

} // namespace Coring5
