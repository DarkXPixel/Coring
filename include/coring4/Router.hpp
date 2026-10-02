#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
namespace Coring4 {

struct StringHash {
  using is_transparent = void;

  std::size_t operator()(std::string_view sv) const noexcept {
    return std::hash<std::string_view>{}(sv);
  }
};
struct Route {
  bool tls_on{false};
  bool http2_on{false};
  bool http3_on{false};
  std::string server_name;
  uint16_t port{0};

  struct OutRoute {
    std::string root;
    std::string index;
    bool is_static;
  };

  std::unordered_map<std::string, OutRoute, StringHash, std::equal_to<>>
      out_routes;
};

struct RouteListener {
  bool tls_on{false};
  bool http2_support{false};
  uint16_t port{0};
  std::unordered_map<std::string, Route, StringHash, std::equal_to<>> routes_in;
};

class Router {
public:
  void add_route(uint16_t port, const RouteListener &listener) {
    routes[port] = listener;
  }

  const Route *get_route(uint16_t port,
                         std::string_view server_name) const noexcept {
    if (auto it1 = routes.find(port); it1 != routes.end()) {
      if (auto it2 = it1->second.routes_in.find(server_name);
          it2 != it1->second.routes_in.end()) {
        return &it2->second;
      }
    }
    return nullptr;
  }

  std::optional<bool> tls_on(uint16_t port) const noexcept {
    if (auto it = routes.find(port); it != routes.end()) {
      return it->second.tls_on;
    }
    return std::nullopt;
  }

private:
  std::unordered_map<uint16_t, RouteListener> routes;
};
} // namespace Coring4