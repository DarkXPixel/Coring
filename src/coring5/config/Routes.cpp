#include "coring5/config/Snapshot.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <charconv>
#include <optional>

namespace Coring5 {
namespace {
namespace PathDetail {

[[nodiscard]] constexpr int hex_value(unsigned char c) noexcept {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

[[nodiscard]] constexpr bool is_unreserved(unsigned char c) noexcept {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
}

[[nodiscard]] constexpr bool is_forbidden_raw(unsigned char c) noexcept {
  return c <= 0x20 || c == 0x7f;
}

// Escapes that must not stay escaped in canonical form.
[[nodiscard]] constexpr bool is_decoded_in_canonical(unsigned char c) noexcept {
  return is_unreserved(c) || c >= 0x80;
}

[[nodiscard]] constexpr bool is_dot_segment(const char *seg,
                                            std::size_t len) noexcept {
  return (len == 1 && seg[0] == '.') ||
         (len == 2 && seg[0] == '.' && seg[1] == '.');
}

} // namespace PathDetail

// Canonicalises `in` into `out`, which must have room for in.size() bytes.
// Returns the output length or nullopt if the path is invalid.
[[nodiscard]] inline std::optional<std::size_t>
normalize_path(std::string_view in, char *out) noexcept {
  static constexpr char kHex[] = "0123456789ABCDEF";
  const std::size_t n = in.size();
  if (n == 0 || in[0] != '/') {
    return std::nullopt;
  }

  std::size_t pos = 0; // end of the canonical output so far
  std::size_t i = 0;
  while (i < n) {
    ++i; // skip the '/' that opens this segment
    std::size_t w = pos + 1;
    while (i < n && in[i] != '/') {
      const auto c = static_cast<unsigned char>(in[i]);
      if (c == '%') {
        if (i + 2 >= n) {
          return std::nullopt;
        }
        const int hi =
            PathDetail::hex_value(static_cast<unsigned char>(in[i + 1]));
        const int lo =
            PathDetail::hex_value(static_cast<unsigned char>(in[i + 2]));
        if (hi < 0 || lo < 0) {
          return std::nullopt;
        }
        const auto v = static_cast<unsigned char>((hi << 4) | lo);
        if (v == 0) {
          return std::nullopt;
        }
        if (PathDetail::is_decoded_in_canonical(v)) {
          out[w++] = static_cast<char>(v);
        } else {
          out[w++] = '%';
          out[w++] = kHex[hi];
          out[w++] = kHex[lo];
        }
        i += 3;
      } else if (PathDetail::is_forbidden_raw(c)) {
        return std::nullopt;
      } else {
        out[w++] = static_cast<char>(c);
        ++i;
      }
    }

    const bool is_last = i >= n;
    const std::size_t len = w - (pos + 1);
    out[pos] = '/';

    if (PathDetail::is_dot_segment(out + pos + 1, len)) {
      if (len == 2) { // ".."
        if (pos == 0) {
          return std::nullopt; // above the root
        }
        std::size_t k = pos;
        while (k > 0 && out[k - 1] != '/') {
          --k;
        }
        pos = k - 1; // slash that opened the previous segment
      }
      if (is_last) {
        pos += 1; // "/a/b/.." -> "/a/", "/a/." -> "/a/"
      }
    } else if (len == 0) {
      if (is_last) {
        pos += 1; // trailing slash
      } // else "//" collapses
    } else {
      pos = w;
    }
  }

  if (pos == 0) {
    out[0] = '/';
    pos = 1;
  }
  return pos;
}

const VirtualHost *find_host(const RouteSet &routes,
                             std::string_view key) noexcept {
  const auto it = routes.hosts.find(key);
  return it == routes.hosts.end() ? nullptr : &routes.vhosts[it->second];
}

} // namespace

bool canonical_route_path(std::string_view path) {
  if (path.find_first_of("?#") != path.npos) {
    return false;
  }
  std::string scratch(path.size(), '/');
  auto n = normalize_path(path, scratch.data());
  return n && path == std::string_view(scratch.data(), *n);
}

bool valid_route_host(std::string_view host) noexcept {
  if (host.empty() || host.size() > 253) {
    return false;
  }
  std::size_t label = 0;
  char previous = 0;
  for (char c : host) {
    if (c == '.') {
      if (!label || previous == '-') {
        return false;
      }
      label = 0;
    } else {
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-')) {
        return false;
      }
      if ((label == 0 && c == '-') || ++label > 63) {
        return false;
      }
    }
    previous = c;
  }
  return label != 0 && previous != '-';
}

void ConfigSnapshot::install_routes(RouteSets sets) {
  for (auto &[name, set] : sets) {
    for (auto &vh : set.vhosts) {
      for (auto paths : {&vh.exact, &vh.prefix}) {
        for (auto &[path, action] : *paths) {
          if (auto proxy = std::get_if<ProxyRoute>(&action)) {
            auto it = std::ranges::find(upstreams, proxy->upstream,
                                        &UpstreamSpec::name);
            proxy->upstream_id = it->id;
          }
        }
      }
    }
  }
  routes = std::move(sets);
}

bool RouteSet::operator==(const RouteSet &other) const {
  if (hosts.size() != other.hosts.size()) {
    return false;
  }
  for (const auto &[name, index] : hosts) {
    const auto *vhost = find_host(other, name);
    if (!vhost || !(vhosts[index] == *vhost)) {
      return false;
    }
  }
  return true;
}

RouteMatch match_route(const RouteSet &routes, std::string_view host,
                       std::string_view target,
                       std::span<char> scratch) noexcept {
  const VirtualHost *vh = nullptr;
  if (host.starts_with(':')) {
    return {};
  }
  const bool literal = host.starts_with('[');
  const auto end = literal ? host.find(']') : host.find(':');
  if (literal && end == host.npos) {
    return {};
  }
  const auto colon = literal ? end + 1 : end;
  if (colon < host.size()) {
    auto port = host.substr(colon + 1);
    uint16_t number{};
    const auto [ptr, ec] =
        std::from_chars(port.data(), port.data() + port.size(), number);
    if (host[colon] != ':' || ec != std::errc{} ||
        ptr != port.data() + port.size()) {
      return {};
    }
    host = host.substr(0, colon);
  }
  if (literal) {
    auto address = host.substr(1, host.size() - 2);
    std::array<char, INET6_ADDRSTRLEN> text{};
    in6_addr parsed{};
    if (address.size() >= text.size() || address.find(char{}) != address.npos) {
      return {};
    }
    std::ranges::copy(address, text.begin());
    if (inet_pton(AF_INET6, text.data(), &parsed) != 1) {
      return {};
    }
  } else if (!host.empty()) {
    if (host.ends_with('.')) {
      host.remove_suffix(1);
    }
    if (!valid_route_host(host)) {
      return {};
    }
    std::array<char, 253> lower;
    std::transform(host.begin(), host.end(), lower.begin(), [](char c) {
      return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
    });
    std::string_view key(lower.data(), host.size());
    vh = find_host(routes, key);
    // Wildcard keys include the leading dot; the bare apex never matches.
    for (auto dot = key.find('.'); !vh && dot != key.npos;
         dot = key.find('.', dot + 1)) {
      vh = find_host(routes, key.substr(dot));
    }
  }
  if (!vh) {
    vh = find_host(routes, "");
  }
  if (!vh) {
    return {};
  }

  const auto raw = target.substr(0, target.find('?'));
  if (raw.size() > scratch.size() || raw.find('#') != raw.npos) {
    return {};
  }
  const auto length = normalize_path(raw, scratch.data());
  if (!length) {
    return {};
  }
  const std::string_view path(scratch.data(), *length);
  if (auto it = vh->exact.find(path); it != vh->exact.end()) {
    return {&it->second, path, {}};
  }
  auto prefix = path;
  while (prefix.size() > 1) {
    if (auto it = vh->prefix.find(prefix); it != vh->prefix.end()) {
      return {&it->second, path, path.substr(prefix.size())};
    }
    prefix = prefix.substr(0, prefix.rfind('/'));
  }
  if (auto it = vh->prefix.find("/"); it != vh->prefix.end()) {
    return {&it->second, path, path};
  }
  return {};
}
} // namespace Coring5
