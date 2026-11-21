#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace Coring5 {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Nanos = std::chrono::nanoseconds;

struct WorkerId {
  uint16_t value{0};
};

struct ListenerId {
  uint32_t value{0};
  bool operator==(const ListenerId &) const = default;
};

struct UpstreamId {
  uint32_t value{0};
  bool operator==(const UpstreamId &) const = default;
};

// Index + generation. generation == 0 means "invalid" (invariant I4).
struct Handle {
  uint32_t index{0};
  uint32_t gen{0};

  [[nodiscard]] constexpr bool valid() const noexcept { return gen != 0; }
  constexpr bool operator==(const Handle &) const = default;
};

// Maximum number of slots a HandlePool may have: the index lives in 20 bits of
// OpTag (see io/OpTag.hpp).
inline constexpr uint32_t kMaxHandleIndex = (1U << 20) - 1;
inline constexpr uint32_t kMaxPoolCapacity = 1U << 20;

enum class TransportProtocol : uint8_t { Tcp, Udp };

enum class ApplicationProtocol : uint8_t { L4, Http1, Http2, Quic };

// What a listener's accepted connections are served with. Only Echo exists in
// the skeleton; the rest are filled in by milestones M3/M4/M8/M10.
enum class ProtocolKind : uint8_t { Echo, L4, Http1, Http2, Http3 };

// One connection pool per worker for each connection type. The id travels in
// the io_uring user_data (4 bits, see io/OpTag.hpp), hence at most 16 pools.
// The authoritative check against the field width is the static_assert in
// io/OpTag.hpp; this one only keeps the limit visible next to the enum.
enum class PoolId : uint8_t { Echo, L4, Http1, Http2, Http3, Count_ };

inline constexpr std::size_t kPoolCount =
    static_cast<std::size_t>(PoolId::Count_);
static_assert(kPoolCount <= 16, "PoolId must fit in 4 bits of OpTag");

[[nodiscard]] constexpr std::size_t pool_index(PoolId p) noexcept {
  return static_cast<std::size_t>(p);
}

[[nodiscard]] constexpr PoolId pool_for(ProtocolKind k) noexcept {
  switch (k) {
  case ProtocolKind::Echo:
    return PoolId::Echo;
  case ProtocolKind::L4:
    return PoolId::L4;
  case ProtocolKind::Http1:
    return PoolId::Http1;
  case ProtocolKind::Http2:
    return PoolId::Http2;
  case ProtocolKind::Http3:
    return PoolId::Http3;
  }
  return PoolId::Echo;
}

[[nodiscard]] constexpr const char *to_string(PoolId p) noexcept {
  switch (p) {
  case PoolId::Echo:
    return "echo";
  case PoolId::L4:
    return "l4";
  case PoolId::Http1:
    return "http1";
  case PoolId::Http2:
    return "http2";
  case PoolId::Http3:
    return "http3";
  case PoolId::Count_:
    break;
  }
  return "unknown";
}

// What the worker knows about an accepted connection beyond its fd: which
// listener accepted it and (for l4) which upstream it must be relayed to.
// Plain ids, never pointers into a snapshot (invariant I8).
struct AcceptInfo {
  ListenerId listener{};
  UpstreamId upstream{};
};

// A connection anywhere on this worker: which pool, and the slot inside it.
struct ConnRef {
  PoolId pool{PoolId::Echo};
  Handle handle{};
};

} // namespace Coring5
