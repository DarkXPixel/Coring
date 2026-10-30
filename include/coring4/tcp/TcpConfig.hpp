#pragma once

#include <cstdint>
namespace Coring4 {
struct TcpConfig {
  bool nodelay{true};
  bool keep_alive{true};
  std::int32_t idle_time{60};
  std::int32_t keep_interval{10};
  std::int32_t keep_count{3};
  std::int32_t max_tcp_connections{100000};
};
} // namespace Coring4
