#pragma once
#include "SessionStorage.hpp"
#include "coring4/BufferHandle.hpp"
#include "coring4/EgressQueue.hpp"
#include "coring4/Http1Stage.hpp"
#include "coring4/LocalIndexQueue.hpp"
#include "coring4/UpstreamConnection.hpp"
#include <cstdint>
#include <variant>

namespace Coring4 {

constexpr auto HIGH_WATERMARK = 256 * 1024;
constexpr auto LOW_WATERMARK = 64 * 1024;

struct BaseSession {
  int fd{-1};
  bool tls_on{false};
};

struct SendRecvSession : BaseSession {
  EgressQueue *egress_queue{nullptr};
  bool recv_paused{false};
  bool multishot_recv{false};
};

struct Http1ClientSession : public SendRecvSession {
  SessionHandle stream_handle{};
  BufferHandle16 recv_handle;
  int recv_offset{0};
  int port{0};
  Http1Stage stage{};
};

struct TestClientSession : public SendRecvSession {
  SessionHandle upstream_handle_;
};

struct Http1UpstreamSession : public BaseSession {
  SessionHandle client_handle{};
};

struct StaticUpstreamSession {
  SessionHandle client_handle{};
  int fd{-1};
  uint32_t readed{0};
  uint32_t size{0};
};

using ClientConnectionVariant =
    std::variant<std::monostate, TestClientSession, Http1ClientSession>;

using UpstreamConnectionVariant =
    std::variant<std::monostate, StaticUpstreamSession>;

} // namespace Coring4