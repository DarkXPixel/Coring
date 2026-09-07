#pragma once

#include <cstddef>
#include <span>
#include <tuple>
namespace Coring2 {
template <typename... Args> class Pipe {
public:
  void on_data(std::span<std::byte> buf);

private:
  std::tuple<Args...> types_;
};
} // namespace Coring2