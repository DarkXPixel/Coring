#pragma once

#include "coring4/core/Types.hpp"
#include <chrono>
#include <string>
namespace Coring4 {
struct L4ListenerConfig {
  std::string name;
  TransportProtocol transport{TransportProtocol::Tcp};
  std::string address;
  std::string upstream_pool;
  std::chrono::seconds connect_timeout{3};
  std::chrono::seconds idle_timeout{300};
};
} // namespace Coring4
