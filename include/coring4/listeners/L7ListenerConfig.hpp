#pragma once

#include "coring4/core/Types.hpp"
#include <string>
#include <vector>
namespace Coring4 {
struct L7ListenerConfig {
  std::string name;
  std::string address;
  TransportProtocol transport{TransportProtocol::Tcp};
  std::string tls;
  std::vector<ApplicationProtocol> protocols;
  std::string route;
};
} // namespace Coring4
