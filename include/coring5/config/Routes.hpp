#pragma once

#include "coring5/base/Types.hpp"
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace Coring5 {

// Response bytes can outlive a route lookup while a response is streaming.
// Sharing the immutable text keeps the rest of the snapshot reloadable.
struct RespondRoute {
  uint16_t status{200};
  std::shared_ptr<const std::string> text;

  RespondRoute() = default;
  RespondRoute(uint16_t code, std::string body)
      : status(code),
        text(std::make_shared<const std::string>(std::move(body))) {}
  [[nodiscard]] std::string_view body() const noexcept {
    return text ? std::string_view(*text) : std::string_view{};
  }
  bool operator==(const RespondRoute &other) const noexcept {
    return status == other.status && body() == other.body();
  }
};
struct RedirectRoute {
  uint16_t status{302};
  std::shared_ptr<const std::string> text;

  RedirectRoute() = default;
  RedirectRoute(uint16_t code, std::string target)
      : status(code),
        text(std::make_shared<const std::string>(std::move(target))) {}
  [[nodiscard]] std::string_view target() const noexcept {
    return text ? std::string_view(*text) : std::string_view{};
  }
  bool operator==(const RedirectRoute &other) const noexcept {
    return status == other.status && target() == other.target();
  }
};
struct StaticRoute {
  std::string root;
  bool operator==(const StaticRoute &) const = default;
};
struct ProxyRoute {
  std::string upstream;
  UpstreamId upstream_id{}; // resolved before publication
  bool operator==(const ProxyRoute &) const = default;
};
using RouteAction =
    std::variant<RespondRoute, RedirectRoute, StaticRoute, ProxyRoute>;
using PathRoutes = std::map<std::string, RouteAction, std::less<>>;

struct VirtualHost {
  PathRoutes exact;
  PathRoutes prefix;
  bool operator==(const VirtualHost &) const = default;
};

struct RouteSet {
  std::vector<VirtualHost> vhosts;
  // Lower-case exact names, wildcard suffixes (".example.com"), and "" for
  // default.
  std::map<std::string, std::size_t, std::less<>> hosts;
  bool operator==(const RouteSet &) const;
};
using RouteSets = std::map<std::string, RouteSet, std::less<>>;

// Provide scratch space for the request path (up to limits.max_uri_bytes).
// Path and remainder borrow scratch until the next lookup.
// Action borrows the snapshot and must be consumed within its lifetime.
struct RouteMatch {
  const RouteAction *action{};
  std::string_view path;
  std::string_view remainder;
};
[[nodiscard]] RouteMatch match_route(const RouteSet &routes,
                                     std::string_view host,
                                     std::string_view target,
                                     std::span<char> scratch) noexcept;

} // namespace Coring5
