#include "coring4/routing/RouteTable.hpp"

#include <atomic>
#include <chrono>
#include <format>
#include <set>
#include <sys/random.h>
#include <type_traits>

namespace Coring4 {
namespace {

using Err = std::unexpected<std::string>;

uint64_t random_seed() noexcept {
  uint64_t s = 0;
  if (::getrandom(&s, sizeof(s), 0) != static_cast<ssize_t>(sizeof(s))) {
    s = static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()) ^
        reinterpret_cast<uintptr_t>(&s);
  }
  return s;
}

uint32_t next_generation() noexcept {
  static std::atomic<uint32_t> counter{0};
  uint32_t v = counter.fetch_add(1, std::memory_order_relaxed) + 1;
  if (v == 0) { // wrapped; 0 is reserved for "invalid"
    v = counter.fetch_add(1, std::memory_order_relaxed) + 1;
  }
  return v;
}

uint64_t mix(uint64_t a, uint64_t b) noexcept {
  return RouteDetail::mum(a ^ RouteDetail::kS0, b ^ RouteDetail::kS1);
}

bool has_ctl(std::string_view s) noexcept {
  for (const char c : s) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7f) {
      return true;
    }
  }
  return false;
}

std::string lower(std::string_view s) {
  std::string r(s);
  RouteDetail::lower_copy(r.data(), s.data(), s.size());
  return r;
}

unsigned count_labels(std::string_view host) noexcept {
  unsigned n = 1;
  for (const char c : host) {
    n += c == '.';
  }
  return n;
}

} // namespace

std::expected<std::shared_ptr<const RouteTable>, std::string>
RouteTable::compile(const RoutingConfig &config,
                    const CompileOptions &options) {
  std::shared_ptr<RouteTable> t(new RouteTable());
  t->seed_ = options.seed.value_or(random_seed());
  t->generation_ = next_generation();

  std::vector<std::pair<uint64_t, uint32_t>> host_items;
  std::vector<std::pair<uint64_t, uint32_t>> path_items;

  // --- actions -------------------------------------------------------------
  auto compile_action =
      [&](const ActionConfig &cfg,
          std::string_view where) -> std::expected<RouteAction, std::string> {
    RouteAction a;
    auto fail =
        [&](std::string_view msg) -> std::expected<RouteAction, std::string> {
      return Err(std::format("{}: {}", where, msg));
    };
    if (const auto *s = std::get_if<StaticActionConfig>(&cfg)) {
      if (s->root.empty() || s->root.front() != '/') {
        return fail("static root must be an absolute path");
      }
      if (has_ctl(s->root)) {
        return fail("static root contains control characters");
      }
      if (s->index.find('/') != std::string::npos || has_ctl(s->index)) {
        return fail("static index must be a plain file name");
      }
      a.kind = ActionKind::Static;
      a.text = s->root;
      while (!a.text.empty() && a.text.back() == '/') {
        a.text.pop_back();
      }
      a.index = s->index.empty() ? "index.html" : s->index;
    } else if (const auto *p = std::get_if<ProxyActionConfig>(&cfg)) {
      if (p->upstream.empty()) {
        return fail("proxy upstream name is empty");
      }
      if (!options.resolve_upstream) {
        return fail("proxy action used but no upstream resolver was provided");
      }
      const auto id = options.resolve_upstream(p->upstream);
      if (!id) {
        return fail(std::format("unknown upstream '{}'", p->upstream));
      }
      a.kind = ActionKind::Proxy;
      a.upstream = *id;
      a.flag = p->strip_prefix;
      a.text = p->upstream;
    } else if (const auto *r = std::get_if<RedirectActionConfig>(&cfg)) {
      switch (r->status) {
      case 301:
      case 302:
      case 303:
      case 307:
      case 308:
        break;
      default:
        return fail("redirect status must be 301, 302, 303, 307 or 308");
      }
      if (r->target.empty() || has_ctl(r->target)) {
        return fail("redirect target is empty or contains control characters");
      }
      a.kind = ActionKind::Redirect;
      a.status = r->status;
      a.flag = r->append_rest;
      a.text = r->target;
    } else if (const auto *rs = std::get_if<RespondActionConfig>(&cfg)) {
      if (rs->status < 100 || rs->status > 599) {
        return fail("respond status must be in 100..599");
      }
      a.kind = ActionKind::Respond;
      a.status = rs->status;
      a.text = rs->body;
    }
    return a;
  };

  std::set<std::string, std::less<>> seen_sets;
  uint32_t vhost_counter = 0;

  for (const RouteSetConfig &set_cfg : config.route_sets) {
    if (set_cfg.name.empty()) {
      return Err("route set with an empty name");
    }
    if (!seen_sets.insert(set_cfg.name).second) {
      return Err(std::format("duplicate route set '{}'", set_cfg.name));
    }

    const auto set_idx = static_cast<uint32_t>(t->sets_.size());
    t->set_names_.emplace(set_cfg.name, set_idx);
    RouteSet set;
    set.salt = mix(t->seed_, 2ull * set_idx + 1);

    std::set<std::string> seen_hosts;

    for (std::size_t vi = 0; vi < set_cfg.vhosts.size(); ++vi) {
      const VHostConfig &vh_cfg = set_cfg.vhosts[vi];
      const std::string vh_where =
          std::format("route set '{}', vhost #{}", set_cfg.name, vi);

      if (vh_cfg.hosts.empty() && !vh_cfg.is_default) {
        return Err(std::format("{}: no hosts and not default", vh_where));
      }
      if (vh_cfg.locations.empty()) {
        return Err(std::format("{}: no locations", vh_where));
      }

      const auto vh_idx = static_cast<uint32_t>(t->vhosts_.size());
      VHost vh;
      vh.salt = mix(t->seed_ ^ RouteDetail::kS3, ++vhost_counter);

      if (vh_cfg.is_default) {
        if (set.default_vhost != kNone) {
          return Err(std::format("{}: more than one default vhost", vh_where));
        }
        set.default_vhost = vh_idx;
      }

      // hosts
      for (const std::string &raw : vh_cfg.hosts) {
        std::string host = lower(raw);
        bool wildcard = false;
        std::string_view key = host;
        if (key.starts_with("*.")) {
          wildcard = true;
          key.remove_prefix(2);
        }
        if (!RouteDetail::valid_host(key) || key.front() == '[') {
          return Err(
              std::format("{}: invalid host '{}' (no port, no trailing dot, "
                          "LDH labels only; use punycode for IDN)",
                          vh_where, raw));
        }
        const unsigned labels = count_labels(key);
        if (labels > RouteDetail::kMaxHostLabels) {
          return Err(
              std::format("{}: host '{}' has too many labels", vh_where, raw));
        }
        if (!seen_hosts.insert(host).second) {
          return Err(std::format("route set '{}': duplicate host '{}'",
                                 set_cfg.name, raw));
        }

        // Must mirror RouteTable::find_vhost(): exact names are hashed whole
        // with the set salt, wildcards hash their suffix with a tagged salt.
        uint64_t hv = RouteDetail::hash_bytes(
            key.data(), key.size(), wildcard ? set.salt ^ kWildTag : set.salt);
        HostEntry he{};
        he.set = set_idx;
        he.vhost = vh_idx;
        he.wildcard = wildcard;
        he.str_off = static_cast<uint32_t>(t->host_strings_.size());
        he.str_len = static_cast<uint32_t>(key.size());
        t->host_strings_.append(key);
        if (wildcard) {
          set.wildcard_mask |= RouteDetail::label_bit(labels);
        }
        host_items.emplace_back(hv, static_cast<uint32_t>(t->hosts_.size()));
        t->hosts_.push_back(he);
        ++set.host_count;
      }

      // locations
      std::set<std::pair<std::string, bool>> seen_locs;
      for (std::size_t li = 0; li < vh_cfg.locations.size(); ++li) {
        const LocationConfig &loc = vh_cfg.locations[li];
        const std::string where =
            std::format("{}, location '{}'", vh_where, loc.path);
        std::string_view path = loc.path;

        if (path.empty() || path.front() != '/' ||
            path.size() > kMaxTargetLen) {
          return Err(std::format("{}: path must start with '/'", where));
        }
        if (path.find_first_of("?#") != std::string_view::npos ||
            !is_canonical_path_exact(path)) {
          return Err(std::format(
              "{}: path must be canonical (no '//', '.', '..', '?', '#', "
              "spaces, or %-escapes of unreserved characters; hex digits "
              "upper-case)",
              where));
        }
        const bool exact = loc.match == MatchKind::Exact;
        if (!exact && path.size() > 1 && path.back() == '/') {
          path.remove_suffix(1); // prefix "/api/" == prefix "/api"
        }
        if (!seen_locs.emplace(std::string(path), exact).second) {
          return Err(std::format("{}: duplicate location", where));
        }

        auto action = compile_action(loc.action, where);
        if (!action) {
          return Err(action.error());
        }
        const auto action_idx = static_cast<uint32_t>(t->actions_.size());
        t->actions_.push_back(std::move(*action));

        if (path == "/") {
          (exact ? vh.root_exact : vh.root_prefix) = action_idx;
          continue;
        }

        RouteDetail::PathCursor cur(path, t->seed_, vh.salt);
        std::size_t end = 0;
        while (cur.next(end)) {
        }
        uint64_t key;
        if (exact) {
          cur.finish();
          key = cur.hash() ^ kExactTag;
          vh.exact_mask |= RouteDetail::depth_bit(cur.depth());
          vh.max_exact_depth = std::max(vh.max_exact_depth, cur.depth());
        } else {
          key = cur.hash();
          vh.prefix_mask |= RouteDetail::depth_bit(cur.depth());
          vh.max_prefix_depth = std::max(vh.max_prefix_depth, cur.depth());
        }

        if (t->path_blob_.size() + path.size() >= 0xFFFFFFFFull) {
          return Err("route table too large");
        }
        Loc l{};
        l.action = action_idx;
        l.vhost = vh_idx;
        l.path_off = static_cast<uint32_t>(t->path_blob_.size());
        l.path_len = static_cast<uint32_t>(path.size());
        l.exact = exact;
        t->path_blob_.append(path);
        path_items.emplace_back(key, static_cast<uint32_t>(t->locs_.size()));
        t->locs_.push_back(l);
      }

      t->vhosts_.push_back(vh);
    }
    if (set_cfg.vhosts.size() == 1 && set.default_vhost != kNone) {
      set.fixed_vhost = set.default_vhost; // Host cannot change the outcome
    }
    t->sets_.push_back(set);
  }

  for (const std::string &name : options.required_route_sets) {
    if (!t->set_names_.contains(name)) {
      return Err(std::format(
          "route set '{}' is still referenced but missing from the config",
          name));
    }
  }

  t->host_index_.build(host_items);
  t->path_index_.build(path_items);
  return std::shared_ptr<const RouteTable>(std::move(t));
}

RouteTable::Stats RouteTable::stats() const noexcept {
  Stats s;
  s.route_sets = static_cast<uint32_t>(sets_.size());
  s.vhosts = static_cast<uint32_t>(vhosts_.size());
  s.locations = static_cast<uint32_t>(actions_.size());
  s.host_entries = static_cast<uint32_t>(hosts_.size());
  s.index_bytes = host_index_.memory_bytes() + path_index_.memory_bytes();
  return s;
}

} // namespace Coring4
