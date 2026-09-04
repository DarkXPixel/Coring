#pragma once

#include "coring2/ProtocolHandler.hpp"
#include <cstdint>
namespace Coring2 {

template <typename Loop> class TcpConnection;

enum class ProtocolResult : uint8_t { Ok, SwitchToHttp2, NeedsMoreData };
template <typename Loop> class UninitProtocol {
public:
  HandlerResponse on_data(Loop &loop, class TcpConnection<Loop> &conn,
                          std::span<const std::byte> data) noexcept {
    return HandlerResponse::Close;
  }
};
} // namespace Coring2