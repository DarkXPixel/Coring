#include "coring5/config/Loader.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <bit>
#include <format>
#include <fstream>
#include <netinet/in.h>
#include <set>
#include <sstream>
#include <toml++/toml.hpp>

namespace Coring5 {

namespace {

std::expected<ProtocolKind, std::string> parse_protocol(std::string_view s) {
  if (s == "echo") {
    return ProtocolKind::Echo;
  }
  if (s == "l4" || s == "tcp") {
    return ProtocolKind::L4;
  }
  if (s == "http1") {
    return ProtocolKind::Http1;
  }
  if (s == "http2") {
    return ProtocolKind::Http2;
  }
  if (s == "http3") {
    return ProtocolKind::Http3;
  }
  return std::unexpected(std::format("unknown protocol '{}'", s));
}

// Which protocols a listener may use today (docs/ROADMAP.md). l4 and http1 are
// operational stubs (they accept and answer/close with a labelled reason);
// http2/http3 stay rejected: a listener that can only close is a footgun in a
// real configuration, even though their pools exist.
bool protocol_implemented(ProtocolKind p) {
  return p == ProtocolKind::Echo || p == ProtocolKind::L4 ||
         p == ProtocolKind::Http1;
}

std::expected<LbPolicy, std::string> parse_policy(std::string_view s) {
  if (s == "round_robin" || s == "rr") {
    return LbPolicy::RoundRobin;
  }
  if (s == "least_conn" || s == "least_connections") {
    return LbPolicy::LeastConnections;
  }
  return std::unexpected(
      std::format("unknown policy '{}' (round_robin, least_conn)", s));
}

// 0.0.0.0 / :: : connecting there reaches the local host by accident.
bool is_unspecified(const Endpoint &e) {
  if (e.family() == AF_INET) {
    return reinterpret_cast<const sockaddr_in *>(&e.addr)->sin_addr.s_addr ==
           htonl(INADDR_ANY);
  }
  if (e.family() == AF_INET6) {
    return IN6_IS_ADDR_UNSPECIFIED(
        &reinterpret_cast<const sockaddr_in6 *>(&e.addr)->sin6_addr);
  }
  return true;
}

bool is_loopback(const Endpoint &e) {
  if (e.family() == AF_INET) {
    return (ntohl(reinterpret_cast<const sockaddr_in *>(&e.addr)
                      ->sin_addr.s_addr) >>
            24) == 127U;
  }
  if (e.family() == AF_INET6) {
    return IN6_IS_ADDR_LOOPBACK(
        &reinterpret_cast<const sockaddr_in6 *>(&e.addr)->sin6_addr);
  }
  return false;
}

constexpr std::size_t kMaxBackendsPerUpstream = 64;

std::expected<PoolId, std::string> parse_pool_key(std::string_view k) {
  if (k == "echo") {
    return PoolId::Echo;
  }
  if (k == "l4") {
    return PoolId::L4;
  }
  if (k == "http1") {
    return PoolId::Http1;
  }
  if (k == "http2") {
    return PoolId::Http2;
  }
  if (k == "http3") {
    return PoolId::Http3;
  }
  return std::unexpected(
      std::format("[runtime.pools] unknown pool '{}' (echo, l4, http1, http2, "
                  "http3)",
                  k));
}

template <class T>
std::expected<void, std::string> read_uint(const toml::table &t,
                                           std::string_view key, T &out,
                                           std::string_view section) {
  const auto *node = t.get(key);
  if (node == nullptr) {
    return {};
  }
  const auto v = node->template value<int64_t>();
  if (!v || *v < 0 ||
      static_cast<uint64_t>(*v) > std::numeric_limits<T>::max()) {
    return std::unexpected(
        std::format("[{}] '{}' must be a non-negative integer", section, key));
  }
  out = static_cast<T>(*v);
  return {};
}

std::expected<void, std::string> read_ms(const toml::table &t,
                                         std::string_view key, Millis &out,
                                         std::string_view section) {
  uint32_t v = 0;
  if (t.get(key) == nullptr) {
    return {};
  }
  if (auto r = read_uint(t, key, v, section); !r) {
    return r;
  }
  out = Millis{v};
  return {};
}

#define C5_TRY(expr)                                                           \
  if (auto _r = (expr); !_r) {                                                 \
    return std::unexpected(std::move(_r.error()));                             \
  }

} // namespace

ConfigResult parse_config(std::string_view text) {
  toml::table root;
  try {
    root = toml::parse(text);
  } catch (const toml::parse_error &e) {
    return std::unexpected(
        std::format("config syntax error: {}", e.description()));
  }

  ParsedConfig out;

  if (const auto *rt = root["runtime"].as_table()) {
    auto &r = out.runtime;
    C5_TRY(read_uint(*rt, "workers", r.workers, "runtime"));
    C5_TRY(read_uint(*rt, "max_connections_per_worker",
                     r.max_connections_per_worker, "runtime"));
    C5_TRY(read_uint(*rt, "buffer_count", r.buffer_count, "runtime"));
    C5_TRY(read_uint(*rt, "buffer_size", r.buffer_size, "runtime"));
    C5_TRY(read_uint(*rt, "ring_sq_entries", r.ring_sq_entries, "runtime"));
    C5_TRY(read_uint(*rt, "ring_cq_entries", r.ring_cq_entries, "runtime"));
    C5_TRY(read_uint(*rt, "relay_memory_mib", r.relay_memory_mib, "runtime"));
    C5_TRY(read_ms(*rt, "tick_ms", r.tick, "runtime"));
    C5_TRY(read_ms(*rt, "drain_timeout_ms", r.drain_timeout, "runtime"));
    C5_TRY(
        read_ms(*rt, "reload_ack_timeout_ms", r.reload_ack_timeout, "runtime"));
    if (const auto *pools = (*rt)["pools"].as_table()) {
      for (const auto &[key, node] : *pools) {
        auto id = parse_pool_key(key.str());
        if (!id) {
          return std::unexpected(std::move(id.error()));
        }
        const auto v = node.value<int64_t>();
        if (!v || *v < 0 || *v > static_cast<int64_t>(kMaxPoolCapacity)) {
          return std::unexpected(
              std::format("[runtime.pools] '{}' must be an integer in 0..{}",
                          key.str(), kMaxPoolCapacity));
        }
        r.pools[pool_index(*id)] = static_cast<uint32_t>(*v);
      }
    } else if (rt->contains("pools")) {
      return std::unexpected("[runtime] 'pools' must be a table");
    }
    if (const auto *cpus = (*rt)["cpus"].as_array()) {
      for (const auto &c : *cpus) {
        const auto v = c.value<int64_t>();
        if (!v || *v < 0 || *v > 4095) {
          return std::unexpected(
              "[runtime] 'cpus' must be an array of cpu ids");
        }
        r.cpus.push_back(static_cast<int>(*v));
      }
    }
  } else if (root.contains("runtime")) {
    return std::unexpected("'runtime' must be a table");
  }

  if (const auto *lt = root["limits"].as_table()) {
    auto &l = out.limits;
    C5_TRY(read_ms(*lt, "idle_timeout_ms", l.idle_timeout, "limits"));
    C5_TRY(read_ms(*lt, "header_timeout_ms", l.header_timeout, "limits"));
    C5_TRY(read_ms(*lt, "connect_timeout_ms", l.connect_timeout, "limits"));
    C5_TRY(read_ms(*lt, "keepalive_timeout_ms", l.keepalive_timeout, "limits"));
    C5_TRY(read_uint(*lt, "max_head_bytes", l.max_head_bytes, "limits"));
    C5_TRY(read_uint(*lt, "max_headers", l.max_headers, "limits"));
    C5_TRY(read_uint(*lt, "max_uri_bytes", l.max_uri_bytes, "limits"));
    C5_TRY(read_uint(*lt, "queue_high_watermark", l.queue_high_watermark,
                     "limits"));
    C5_TRY(
        read_uint(*lt, "queue_low_watermark", l.queue_low_watermark, "limits"));
  } else if (root.contains("limits")) {
    return std::unexpected("'limits' must be a table");
  }

  if (const auto *arr = root["upstream"].as_array()) {
    std::size_t i = 0;
    for (const auto &node : *arr) {
      const auto *t = node.as_table();
      if (t == nullptr) {
        return std::unexpected(std::format("upstream #{} must be a table", i));
      }
      UpstreamSpec u;
      u.name = (*t)["name"].value_or(std::string{});
      if (u.name.empty()) {
        return std::unexpected(std::format("upstream #{} has no 'name'", i));
      }
      const auto *bs = (*t)["backends"].as_array();
      if (bs == nullptr) {
        return std::unexpected(std::format(
            "upstream '{}': 'backends' must be an array of \"ip:port\"",
            u.name));
      }
      for (const auto &b : *bs) {
        const auto addr_text = b.value<std::string>();
        auto ep =
            addr_text
                ? Endpoint::parse(*addr_text)
                : Result<Endpoint>{unexpected_error(Errc::InvalidArgument)};
        if (!ep) {
          return std::unexpected(std::format(
              "upstream '{}': invalid backend address (numeric ip:port "
              "expected)",
              u.name));
        }
        u.backends.push_back(*ep);
      }
      auto policy =
          parse_policy((*t)["policy"].value_or(std::string{"round_robin"}));
      if (!policy) {
        return std::unexpected(
            std::format("upstream '{}': {}", u.name, policy.error()));
      }
      u.policy = *policy;
      C5_TRY(read_ms(*t, "connect_timeout_ms", u.connect_timeout, "upstream"));
      C5_TRY(read_uint(*t, "max_fails", u.max_fails, "upstream"));
      C5_TRY(read_ms(*t, "eject_time_ms", u.eject_time, "upstream"));
      out.upstreams.push_back(std::move(u));
      ++i;
    }
  } else if (root.contains("upstream")) {
    return std::unexpected(
        "'upstream' must be an array of tables ([[upstream]])");
  }

  if (const auto *arr = root["listener"].as_array()) {
    std::size_t i = 0;
    for (const auto &node : *arr) {
      const auto *t = node.as_table();
      if (t == nullptr) {
        return std::unexpected(std::format("listener #{} must be a table", i));
      }
      ListenerSpec spec;
      spec.name = (*t)["name"].value_or(std::string{});
      spec.address = (*t)["address"].value_or(std::string{});
      spec.route = (*t)["route"].value_or(std::string{});
      spec.upstream = (*t)["upstream"].value_or(std::string{});
      spec.tls = (*t)["tls"].value_or(std::string{});
      if (spec.name.empty()) {
        return std::unexpected(std::format("listener #{} has no 'name'", i));
      }
      auto ep = Endpoint::parse(spec.address);
      if (!ep) {
        return std::unexpected(std::format(
            "listener '{}': invalid address '{}'", spec.name, spec.address));
      }
      spec.endpoint = *ep;
      auto proto =
          parse_protocol((*t)["protocol"].value_or(std::string{"echo"}));
      if (!proto) {
        return std::unexpected(
            std::format("listener '{}': {}", spec.name, proto.error()));
      }
      spec.protocol = *proto;
      C5_TRY(
          read_uint(*t, "max_connections", spec.max_connections, "listener"));
      if (spec.max_connections > kMaxPoolCapacity) {
        return std::unexpected(std::format(
            "listener '{}': max_connections out of range", spec.name));
      }
      out.listeners.push_back(std::move(spec));
      ++i;
    }
  } else if (root.contains("listener")) {
    return std::unexpected(
        "'listener' must be an array of tables ([[listener]])");
  }

  out.runtime.pool_capacity =
      derive_pool_capacities(out.runtime, out.listeners);
  return out;
}

PoolCapacities derive_pool_capacities(const RuntimeConfig &rt,
                                      std::span<const ListenerSpec> listeners) {
  std::array<uint64_t, kPoolCount> explicit_sum{};
  std::array<uint32_t, kPoolCount> unspecified{};
  for (const auto &l : listeners) {
    const auto p = pool_index(pool_for(l.protocol));
    if (l.max_connections > 0) {
      explicit_sum[p] += l.max_connections;
    } else {
      ++unspecified[p];
    }
  }

  uint64_t used = 0;
  uint64_t flexible = 0; // protocols that share the remaining budget
  for (std::size_t p = 0; p < kPoolCount; ++p) {
    used += rt.pools[p] > 0 ? rt.pools[p] : explicit_sum[p];
    if (rt.pools[p] == 0 && unspecified[p] > 0) {
      ++flexible;
    }
  }
  const uint64_t budget = rt.max_connections_per_worker;
  const uint64_t remaining = budget > used ? budget - used : 0;
  const uint64_t share = flexible > 0 ? remaining / flexible : 0;

  PoolCapacities cap{};
  for (std::size_t p = 0; p < kPoolCount; ++p) {
    uint64_t c = rt.pools[p];
    if (c == 0) {
      c = explicit_sum[p] + (unspecified[p] > 0 ? share : 0);
    }
    cap[p] = static_cast<uint32_t>(std::min<uint64_t>(c, kMaxPoolCapacity));
  }
  return cap;
}

std::expected<void, std::string>
check_pools_fit(const RuntimeConfig &running,
                std::span<const ListenerSpec> listeners) {
  std::array<uint64_t, kPoolCount> explicit_sum{};
  for (const auto &l : listeners) {
    const auto p = pool_index(pool_for(l.protocol));
    if (running.pool_capacity[p] == 0) {
      return std::unexpected(std::format(
          "listener '{}': no '{}' connection pool was sized at startup: "
          "restart required (pools are fixed for the process lifetime)",
          l.name, to_string(pool_for(l.protocol))));
    }
    explicit_sum[p] += l.max_connections;
  }
  for (std::size_t p = 0; p < kPoolCount; ++p) {
    if (explicit_sum[p] > running.pool_capacity[p]) {
      return std::unexpected(std::format(
          "'{}' listeners ask for {} connections per worker but the pool was "
          "sized for {} at startup: restart required",
          to_string(static_cast<PoolId>(p)), explicit_sum[p],
          running.pool_capacity[p]));
    }
  }
  return {};
}

ConfigResult load_config_file(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    return std::unexpected(std::format("cannot open config file '{}'", path));
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  return parse_config(ss.str());
}

std::expected<void, std::string> validate_config(const ParsedConfig &cfg) {
  const auto &r = cfg.runtime;
  if (r.workers == 0 || r.workers > 1024) {
    return std::unexpected("[runtime] workers must be in 1..1024");
  }
  if (!r.cpus.empty() && r.cpus.size() < r.workers) {
    return std::unexpected(
        "[runtime] cpus must list at least one cpu per worker");
  }
  if (r.max_connections_per_worker == 0 ||
      r.max_connections_per_worker > kMaxPoolCapacity) {
    return std::unexpected("[runtime] max_connections_per_worker out of range");
  }
  if (!std::has_single_bit(r.buffer_count) || r.buffer_count < 2 ||
      r.buffer_count > 32768) {
    return std::unexpected(
        "[runtime] buffer_count must be a power of two <= 32768");
  }
  if (r.buffer_size < 512 || r.buffer_size > (1U << 20)) {
    return std::unexpected("[runtime] buffer_size must be in 512..1048576");
  }
  if (r.tick < Millis{1} || r.tick > Millis{1000}) {
    return std::unexpected("[runtime] tick_ms must be in 1..1000");
  }
  if (!std::has_single_bit(r.ring_sq_entries) ||
      !std::has_single_bit(r.ring_cq_entries) ||
      r.ring_cq_entries < r.ring_sq_entries) {
    return std::unexpected(
        "[runtime] ring sizes must be powers of two with cq >= sq");
  }

  // Pools: recomputed here (not trusted from the struct) so that hand-built
  // configs are validated too.
  const PoolCapacities cap = derive_pool_capacities(r, cfg.listeners);
  uint64_t total = 0;
  for (const uint32_t c : cap) {
    total += c;
  }
  if (total > r.max_connections_per_worker) {
    return std::unexpected(
        std::format("pools need {} connections per worker but "
                    "[runtime] max_connections_per_worker is {}",
                    total, r.max_connections_per_worker));
  }

  const auto &l = cfg.limits;
  if (l.queue_low_watermark >= l.queue_high_watermark) {
    return std::unexpected(
        "[limits] queue_low_watermark must be < high watermark");
  }
  if (l.idle_timeout < r.tick || l.header_timeout < r.tick ||
      l.connect_timeout < r.tick) {
    return std::unexpected("[limits] timeouts must be >= runtime tick");
  }
  if (l.max_head_bytes < 256 || l.max_headers == 0 || l.max_uri_bytes == 0) {
    return std::unexpected("[limits] head/header/uri limits too small");
  }

  if (r.relay_memory_mib == 0 || r.relay_memory_mib > 65536) {
    return std::unexpected("[runtime] relay_memory_mib must be in 1..65536");
  }
  {
    const uint64_t chunks =
        (static_cast<uint64_t>(r.relay_memory_mib) << 20) / r.buffer_size;
    if (chunks < 4 || chunks >= 0xFFFFFFFFULL) {
      return std::unexpected(
          "[runtime] relay_memory_mib must hold at least 4 buffer_size chunks");
    }
  }

  std::set<std::string> upstream_names;
  for (const auto &u : cfg.upstreams) {
    if (!upstream_names.insert(u.name).second) {
      return std::unexpected(
          std::format("duplicate upstream name '{}'", u.name));
    }
    if (u.backends.empty() || u.backends.size() > kMaxBackendsPerUpstream) {
      return std::unexpected(std::format("upstream '{}': needs 1..{} backends",
                                         u.name, kMaxBackendsPerUpstream));
    }
    for (const auto &b : u.backends) {
      if (b.port() == 0 || is_unspecified(b)) {
        return std::unexpected(std::format(
            "upstream '{}': backend '{}' needs a concrete address and a "
            "non-zero port",
            u.name, b.to_string()));
      }
    }
    if (u.connect_timeout.count() != 0 && u.connect_timeout < r.tick) {
      return std::unexpected(std::format(
          "upstream '{}': connect_timeout_ms must be 0 or >= runtime tick",
          u.name));
    }
    if (u.eject_time < Millis{1}) {
      return std::unexpected(
          std::format("upstream '{}': eject_time_ms must be >= 1", u.name));
    }
  }

  std::set<std::string> names;
  std::set<std::string> addrs;
  for (const auto &s : cfg.listeners) {
    if (s.protocol == ProtocolKind::L4 && s.upstream.empty()) {
      return std::unexpected(std::format(
          "listener '{}': protocol l4 needs `upstream = \"<name>\"`", s.name));
    }
    if (!s.upstream.empty()) {
      const auto it = std::find_if(
          cfg.upstreams.begin(), cfg.upstreams.end(),
          [&](const UpstreamSpec &u) { return u.name == s.upstream; });
      if (it == cfg.upstreams.end()) {
        return std::unexpected(std::format(
            "listener '{}': unknown upstream '{}'", s.name, s.upstream));
      }
      // A proxy that dials itself loops until the pool is exhausted.
      for (const auto &b : it->backends) {
        if (s.endpoint.port() != 0 && b.port() == s.endpoint.port() &&
            (b == s.endpoint ||
             (is_unspecified(s.endpoint) && is_loopback(b)))) {
          return std::unexpected(std::format(
              "listener '{}': upstream '{}' contains the listener's own "
              "address {}",
              s.name, s.upstream, b.to_string()));
        }
      }
    }
    if (!names.insert(s.name).second) {
      return std::unexpected(
          std::format("duplicate listener name '{}'", s.name));
    }
    if (s.endpoint.port() != 0 &&
        !addrs.insert(s.endpoint.to_string()).second) {
      return std::unexpected(
          std::format("duplicate listener address '{}'", s.address));
    }
    if (!protocol_implemented(s.protocol)) {
      return std::unexpected(std::format(
          "listener '{}': protocol not implemented yet (see docs/ROADMAP.md)",
          s.name));
    }
    if (cap[pool_index(pool_for(s.protocol))] == 0) {
      return std::unexpected(std::format(
          "listener '{}': no connection budget left for its '{}' pool "
          "(raise max_connections_per_worker or set [runtime.pools])",
          s.name, to_string(pool_for(s.protocol))));
    }
  }
  return {};
}

} // namespace Coring5
