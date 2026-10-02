#pragma once

#include "coring4/EgressQueue.hpp"
#include <chrono>
#include <string>
#include <variant>
namespace Coring4 {
// struct UpstreamConnection {
//   int fd{-1};
//   std::string address;
//   std::chrono::steady_clock::time_point last_used;
//   EgressQueue *egress_queue{nullptr};
// };

// using UpstreamConnectionVariant =
//     std::variant<std::monostate, UpstreamConnection>;
} // namespace Coring4