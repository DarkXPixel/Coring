#include "RouteLoader.hpp"

#include <algorithm>
#include <climits>
#include <format>

namespace Coring5 {
namespace {
using RouteError = std::unexpected<std::string>;
using Keys = std::initializer_list<std::string_view>;

std::expected<void, std::string> check_keys(const toml::table &t, Keys common,
                                            Keys action = {}) {
  for (const auto &[key, value] : t) {
    if (!std::ranges::contains(common, key.str()) &&
        !std::ranges::contains(action, key.str())) {
      return RouteError(std::format("unexpected route key '{}'", key.str()));
    }
  }
  return {};
}

template <class T>
std::expected<T, std::string> field(const toml::table &t, std::string_view key,
                                    T fallback) {
  if (!t.contains(key)) {
    return fallback;
  }
  if (auto val = t[key].value_exact<T>()) {
    return *val;
  }
  return RouteError(std::format(
      "route field '{}' has the wrong type or is out of range", key));
}

std::expected<RouteAction, std::string> parse_action(const toml::table &t) {
  const auto kind = t["action"].value<std::string_view>();
  const Keys common{"action", "match", "path"};
  if (kind == "respond" || kind == "redirect") {
    const bool redirect = kind == "redirect";
    const auto payload = redirect ? "target" : "body";
    if (auto keys = check_keys(t, common, {"status", payload}); !keys) {
      return RouteError(keys.error());
    }
    auto status = field<int64_t>(t, "status", redirect ? 302 : 200);
    if (!status || *status < 0 || *status > UINT16_MAX) {
      return RouteError("invalid route status");
    }
    auto text = field<std::string>(t, payload, {});
    if (!text) {
      return RouteError(text.error());
    }
    auto code = static_cast<uint16_t>(*status);
    if (redirect) {
      return RedirectRoute{code, std::move(*text)};
    }
    return RespondRoute{code, std::move(*text)};
  }
  if (kind == "static_file" || kind == "reverse_proxy") {
    const bool proxy = kind == "reverse_proxy";
    const auto key = proxy ? "upstream" : "root";
    if (auto keys = check_keys(t, common, {key}); !keys) {
      return RouteError(keys.error());
    }
    auto value = t[key].value<std::string>();
    if (!value || value->empty()) {
      return RouteError(std::format("route needs a nonempty '{}'", key));
    }
    if (proxy) {
      return ProxyRoute{std::move(*value)};
    }
    return StaticRoute{std::move(*value)};
  }
  return RouteError(
      "route action must be respond, redirect, static_file or reverse_proxy");
}

std::expected<void, std::string> parse_vhost(const toml::table &t,
                                             RouteSet &set) {
  if (auto keys = check_keys(t, {"hosts", "default", "location"}); !keys) {
    return keys;
  }
  auto fallback = field<bool>(t, "default", false);
  if (!fallback) {
    return RouteError(fallback.error());
  }
  const auto index = set.vhosts.size();
  if (*fallback && !set.hosts.emplace("", index).second) {
    return RouteError("duplicate default vhost");
  }
  auto hosts = t["hosts"].as_array();
  if (t.contains("hosts") && !hosts) {
    return RouteError("vhost hosts must be an array");
  }
  if (!*fallback && (!hosts || hosts->empty())) {
    return RouteError("vhost requires hosts or default");
  }
  if (hosts) {
    for (const auto &node : *hosts) {
      auto name = node.value<std::string>();
      if (!name) {
        return RouteError("vhost host must be a string");
      }
      if (name->starts_with("*.")) {
        name->erase(0, 1);
      } else if (name->starts_with('.')) {
        return RouteError("invalid host pattern");
      }
      auto dns = std::string_view(*name);
      if (dns.starts_with('.')) {
        dns.remove_prefix(1);
      }
      if (!valid_route_host(dns)) {
        return RouteError(std::format("invalid host '{}'", *name));
      }
      for (auto &c : *name) {
        if (c >= 'A' && c <= 'Z') {
          c = static_cast<char>(c + ('a' - 'A'));
        }
      }
      if (!set.hosts.emplace(*name, index).second) {
        return RouteError(std::format("duplicate host '{}'", *name));
      }
    }
  }

  auto locations = t["location"].as_array();
  if (!locations || locations->empty()) {
    return RouteError("vhost needs a nonempty location array");
  }
  auto &vh = set.vhosts.emplace_back();
  for (const auto &node : *locations) {
    const auto *loc = node.as_table();
    if (!loc) {
      return RouteError("location must be a table");
    }
    auto path = (*loc)["path"].value<std::string>();
    if (!path || !canonical_route_path(*path)) {
      return RouteError("location needs a canonical absolute path");
    }
    auto match = field<std::string>(*loc, "match", "prefix");
    if (!match || (*match != "exact" && *match != "prefix")) {
      return RouteError("location match must be exact or prefix");
    }
    auto action = parse_action(*loc);
    if (!action) {
      return RouteError(
          std::format("location '{}': {}", *path, action.error()));
    }
    if (*match == "prefix" && path->size() > 1 && path->back() == '/') {
      path->pop_back();
    }
    auto &paths = *match == "exact" ? vh.exact : vh.prefix;
    if (!paths.emplace(*path, std::move(*action)).second) {
      return RouteError(std::format("duplicate location '{}'", *path));
    }
  }
  return {};
}
} // namespace

std::expected<RouteSets, std::string> parse_routes(const toml::table &root) {
  RouteSets sets;
  if (!root.contains("route_set")) {
    return sets;
  }
  auto arr = root["route_set"].as_array();
  if (!arr) {
    return RouteError("'route_set' must be an array of tables");
  }
  for (const auto &node : *arr) {
    auto t = node.as_table();
    if (!t) {
      return RouteError("route_set must be a table");
    }
    if (auto keys = check_keys(*t, {"name", "vhost"}); !keys) {
      return RouteError(keys.error());
    }
    auto name = (*t)["name"].value<std::string>();
    if (!name || name->empty()) {
      return RouteError("route_set needs a nonempty name");
    }
    auto [it, added] = sets.try_emplace(*name);
    if (!added) {
      return RouteError(std::format("duplicate route set '{}'", *name));
    }
    auto hosts = (*t)["vhost"].as_array();
    if (!hosts || hosts->empty()) {
      return RouteError(std::format("route set '{}': missing vhosts", *name));
    }
    for (const auto &vh : *hosts) {
      auto table = vh.as_table();
      if (!table) {
        return RouteError("vhost must be a table");
      }
      if (auto ok = parse_vhost(*table, it->second); !ok) {
        return RouteError(std::format("route set '{}': {}", *name, ok.error()));
      }
    }
  }
  return sets;
}

namespace {
std::expected<void, std::string> validate_action(const RouteAction &action,
                                                 const ParsedConfig &cfg) {
  if (auto r = std::get_if<RespondRoute>(&action)) {
    if (r->status < 200 || r->status > 599 || r->body().size() > (1U << 20) ||
        (!r->body().empty() && (r->status == 204 || r->status == 304))) {
      return RouteError("invalid respond status or body");
    }
  } else if (auto redirect = std::get_if<RedirectRoute>(&action)) {
    constexpr std::array<uint16_t, 5> codes{301, 302, 303, 307, 308};
    if (!std::ranges::contains(codes, redirect->status) ||
        redirect->target().empty() || redirect->target().size() > 8192 ||
        !std::ranges::all_of(redirect->target(), [](char c) {
          return c > ' ' && static_cast<unsigned char>(c) < 127;
        })) {
      return RouteError("invalid redirect status or target");
    }
  } else if (auto f = std::get_if<StaticRoute>(&action)) {
    if (f->root.size() >= PATH_MAX || !f->root.starts_with('/') ||
        f->root.find('\0') != f->root.npos) {
      return RouteError("static_file root must be an absolute directory path");
    }
  } else {
    const auto &proxy = std::get<ProxyRoute>(action);
    if (proxy.upstream.empty() ||
        std::ranges::find(cfg.upstreams, proxy.upstream, &UpstreamSpec::name) ==
            cfg.upstreams.end()) {
      return RouteError(
          std::format("unknown proxy upstream '{}'", proxy.upstream));
    }
  }
  return {};
}
} // namespace

std::expected<void, std::string> validate_routes(const ParsedConfig &cfg) {
  for (const auto &[name, set] : cfg.routes) {
    if (name.empty() || set.hosts.empty()) {
      return RouteError("route set needs a name and vhosts");
    }
    for (const auto &[host, index] : set.hosts) {
      if (index >= set.vhosts.size()) {
        return RouteError("invalid route vhost index");
      }
    }
    for (const auto &vh : set.vhosts) {
      for (auto paths : {&vh.exact, &vh.prefix}) {
        for (const auto &[path, action] : *paths) {
          if (auto ok = validate_action(action, cfg); !ok) {
            return RouteError(std::format("route set '{}', location '{}': {}",
                                          name, path, ok.error()));
          }
        }
      }
    }
  }
  for (const auto &l : cfg.listeners) {
    if (l.route.empty()) {
      continue;
    }
    if (l.protocol != ProtocolKind::Http1) {
      return RouteError(
          std::format("listener '{}': route requires HTTP", l.name));
    }
    if (!cfg.routes.contains(l.route)) {
      return RouteError(std::format("listener '{}': unknown route set '{}'",
                                    l.name, l.route));
    }
  }
  return {};
}
} // namespace Coring5
