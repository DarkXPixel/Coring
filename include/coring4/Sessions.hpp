#pragma once
#include "SessionStorage.hpp"
#include "coring4/BufferHandle.hpp"
#include <variant>

namespace Coring4 {

constexpr auto HIGH_WATERMARK = 256 * 1024;
constexpr auto LOW_WATERMARK = 64 * 1024;
struct Http1ClientSession {
  SessionHandle stream_handle{};
  int fd{-1};
  uint16_t in_flight_bytes{0};
  bool recv_paused{false};
};

struct TestClientSession {
  int fd{-1};
  BufferHandle16 write_handle;
  uint8_t bgid{0};
};

struct Http1UpstreamSession {
  SessionHandle client_handle{};
  int fd{-1};
};

using ClientConnectionVariant = std::variant<std::monostate, TestClientSession>;

} // namespace Coring4