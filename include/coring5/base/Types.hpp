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

} // namespace Coring5
