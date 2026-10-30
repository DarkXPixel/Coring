#pragma once

#include "coring4/core/Types.hpp"
#include <chrono>
#include <string>
namespace Coring4 {
struct ListenerConfig {
  std::string name;
  TransportProtocol transport{TransportProtocol::Tcp};
  std::string address;
  std::string upstream_pool;
  std::chrono::seconds connect_timeout{3};
  std::chrono::seconds idle_timeout{300};
  std::string tls;
  std::vector<ApplicationProtocol> protocols;
  std::string route;
  int redirect_to_port{0};
};
} // namespace Coring4
