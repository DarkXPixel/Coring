#pragma once

#include <span>
namespace Coring2 {

struct WriteBufferDesc {
  std::span<std::byte> buffer;
};

class WriteBuffer {};
} // namespace Coring2