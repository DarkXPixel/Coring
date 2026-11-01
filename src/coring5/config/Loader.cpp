#include "coring5/config/Loader.hpp"

#include <algorithm>
#include <bit>
#include <format>
#include <fstream>
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

// Milestones in which each protocol becomes available (docs/ROADMAP.md).
bool protocol_implemented(ProtocolKind p) { return p == ProtocolKind::Echo; }

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
    C5_TRY(read_ms(*rt, "tick_ms", r.tick, "runtime"));
    C5_TRY(read_ms(*rt, "drain_timeout_ms", r.drain_timeout, "runtime"));
    C5_TRY(
        read_ms(*rt, "reload_ack_timeout_ms", r.reload_ack_timeout, "runtime"));
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
      out.listeners.push_back(std::move(spec));
      ++i;
    }
  } else if (root.contains("listener")) {
    return std::unexpected(
        "'listener' must be an array of tables ([[listener]])");
  }

  return out;
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

  std::set<std::string> names;
  std::set<std::string> addrs;
  for (const auto &s : cfg.listeners) {
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
  }
  return {};
}

} // namespace Coring5
