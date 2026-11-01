#pragma once

#include "coring5/base/Result.hpp"
#include <cstdint>
#include <string>
#include <string_view>
#include <sys/socket.h>

namespace Coring5 {

// A resolved socket address. Numeric hosts only: DNS happens on the control
// thread before an Endpoint is built (see docs/CONFIG_AND_RELOAD.md).
struct Endpoint {
  sockaddr_storage addr{};
  socklen_t len{0};

  // "1.2.3.4:80", "[::1]:80", ":80" (= 0.0.0.0:80)
  [[nodiscard]] static Result<Endpoint> parse(std::string_view text);

  [[nodiscard]] int family() const noexcept { return addr.ss_family; }
  [[nodiscard]] uint16_t port() const noexcept;
  [[nodiscard]] std::string to_string() const;
  [[nodiscard]] const sockaddr *sa() const noexcept {
    return reinterpret_cast<const sockaddr *>(&addr);
  }
};

} // namespace Coring5
