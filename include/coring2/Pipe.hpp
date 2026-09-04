#pragma once

#include <cstddef>
#include <span>
namespace Coring2 {
class Pipe {
public:
  void on_data(std::span<std::byte> buf);

private:
};
} // namespace Coring2