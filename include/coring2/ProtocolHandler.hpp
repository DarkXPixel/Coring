#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>
namespace Coring2 {
enum class HandlerResponse : uint8_t {
  Ok,
  NotRead,
  Close,
  SwitchToHttp1,
  SwitchToHttp2
};

enum class TypeOfData {

};

struct HandlerResponse_ {
  enum class Response { Ok, Close, ForceClose } code;
  std::optional<std::vector<std::byte>> response_data;
};

template <typename Loop> class IProtocolHandler {
public:
  virtual ~IProtocolHandler() = default;

  virtual HandlerResponse on_data(Loop &loop,
                                  std::span<const std::byte> data) = 0;
  virtual void on_write_ready(Loop &loop) = 0;
};
} // namespace Coring2