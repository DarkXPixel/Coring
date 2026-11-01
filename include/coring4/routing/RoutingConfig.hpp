#pragma once

// Control-plane description of routing. Plain, copyable, easy to
// (de)serialize. It is never touched on the request path: RouteTable::compile()
// validates it and turns it into an immutable, lookup-optimised table.

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace Coring4 {

enum class MatchKind : uint8_t {
  // Matches the path itself and everything below it on a path-segment
  // boundary: "/api" matches "/api", "/api/x" but NOT "/apix".
  // The longest matching prefix wins.
  Prefix,
  // Matches only the exact (normalized) path. Beats any prefix.
  Exact,
};

// Serve files from `root`. The request path (after the matched prefix) is
// appended to `root` by the static handler.
struct StaticActionConfig {
  std::string root;
  std::string index{"index.html"};
};

// Forward to a named upstream pool (resolved to an UpstreamId at compile time).
struct ProxyActionConfig {
  std::string upstream;
  // If true the upstream sees only the part of the path after the matched
  // prefix (nginx-style `location /api/ { proxy_pass http://x/; }`).
  bool strip_prefix{false};
};

struct RedirectActionConfig {
  uint16_t status{302}; // 301, 302, 303, 307, 308
  std::string target;
  // If true the consumer appends the remaining path (and query) to `target`.
  bool append_rest{false};
};

// Fixed response (deny / maintenance page / health check ...).
struct RespondActionConfig {
  uint16_t status{404};
  std::string body;
};

using ActionConfig = std::variant<StaticActionConfig, ProxyActionConfig,
                                  RedirectActionConfig, RespondActionConfig>;

struct LocationConfig {
  // Canonical, decoded form: starts with '/', no "//", no dot-segments,
  // no '%', '?' or '#'.
  std::string path{"/"};
  MatchKind match{MatchKind::Prefix};
  ActionConfig action;
};

struct VHostConfig {
  // Exact names ("example.com") or leading-label wildcards ("*.example.com").
  // Wildcards match one OR MORE labels, longest suffix wins, and do not match
  // the bare apex. No ports: the port belongs to the listener.
  std::vector<std::string> hosts;
  // Used when the Host header is missing or matches nothing else.
  bool is_default{false};
  std::vector<LocationConfig> locations;
};

struct RouteSetConfig {
  // Referenced by ListenerConfig::route.
  std::string name;
  std::vector<VHostConfig> vhosts;
};

struct RoutingConfig {
  std::vector<RouteSetConfig> route_sets;
};

} // namespace Coring4
