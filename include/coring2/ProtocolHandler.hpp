#pragma once

#include <cstddef>
#include <span>
namespace Coring2 {
enum class HandlerResponse {
  Ok,
  NotRead,
  Close,
};

template <typename Loop> class IProtocolHandler {
public:
  virtual ~IProtocolHandler() = default;

  virtual HandlerResponse on_data(Loop &loop,
                                  std::span<const std::byte> data) = 0;
  virtual void on_write_ready(Loop &loop) = 0;
};
} // namespace Coring2