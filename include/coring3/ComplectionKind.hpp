#pragma once

#include <cstdint>
namespace Coring3 {
enum class ComplectionKind : uint8_t {
  Accept,

  ClientRecv,
  ClientSend,

  UpstreamConnect,
  UpstreamRecv,
  UpstreamSend,

  Timeout,
  Shutdown,
};
}