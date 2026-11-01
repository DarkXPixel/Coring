#pragma once

// Immutable, lookup-optimised routing table.
//
//   RoutingConfig  --compile()-->  RouteTable  --match()-->  Match
//   (control plane, plain data)    (immutable)               (hot path)
//
// A RouteTable never changes after compile(). Runtime changes are done by
// compiling a NEW table and swapping the pointer (see WorkerRouting), so
// readers need no locks, no atomics and no per-request reference counting.
//
// Hot path (match()) properties:
//   * no heap allocation, no exceptions, no locks, no atomics;
//   * no std::string / std::unordered_map: only flat arrays + one open-
//     addressing index per kind of key;
//   * O(host length + path length), independent of the number of routes;
//   * a route set with a single catch-all location costs zero hash probes.
//
// Lookup order (same semantics as nginx `server_name` / `location`):
//   host:  exact name  >  longest wildcard suffix  >  default vhost
//   path:  exact match >  longest prefix (on segment boundaries)
//
// Every hash hit is verified byte-for-byte, so a 64-bit collision can never
// route a request to the wrong vhost / location. The hash seed is random per
// table, which defeats hash-flooding with attacker-chosen hosts and paths.

#include "coring4/core/Types.hpp"
#include "coring4/routing/FlatIndex.hpp"
#include "coring4/routing/PathNormalizer.hpp"
#include "coring4/routing/RouteHash.hpp"
#include "coring4/routing/RouteTypes.hpp"
#include "coring4/routing/RoutingConfig.hpp"
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Coring4 {

enum class ActionKind : uint8_t { Static, Proxy, Redirect, Respond };

// Compiled action. Lives inside the RouteTable; a pointer to it is valid for
// as long as the table (or a WorkerRouting::Pin on it) is alive.
struct RouteAction {
  ActionKind kind{ActionKind::Respond};
  // Proxy: strip the matched prefix. Redirect: append the rest of the path.
  bool flag{false};
  // Redirect / Respond status code.
  uint16_t status{404};
  // Proxy target, already resolved: no name lookup on the request path.
  UpstreamId upstream{0};
  // Static: document root WITHOUT trailing '/', so `root + rest` is the file
  // (if `rest` is empty or ends with '/', append `index`).
  // Redirect: target. Respond: body.
  std::string text;
  // Static: index file name.
  std::string index;
};

enum class MatchStatus : uint8_t {
  Ok,
  NotFound,    // no vhost / no location: 404
  BadPath,     // malformed or non-canonicalisable path: 400
  BadHost,     // malformed Host: 400
  PathTooLong, // 414
  Unavailable, // no table installed or stale RouteSetId: 503
};

struct Match {
  MatchStatus status{MatchStatus::NotFound};
  const RouteAction *action{nullptr};
  // Canonical path (see PathNormalizer.hpp). Points into the request buffer
  // or into the MatchScratch, whichever the caller keeps alive longer.
  std::string_view path;
  // Part of `path` after the matched prefix: "" or starts with '/'. Always ""
  // for exact matches. Still percent-encoded: run decode_for_filesystem()
  // before touching the file system.
  std::string_view rest;
  // Raw query string without '?'.
  std::string_view query;

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == MatchStatus::Ok;
  }
};

// Scratch memory for paths that need rewriting. One per worker, reused.
struct MatchScratch {
  alignas(64) char buf[kMaxTargetLen];
};

struct CompileOptions {
  // Resolves an upstream pool name to its id. A Proxy action whose upstream
  // cannot be resolved makes the whole compile fail, so a broken config is
  // never installed.
  std::function<std::optional<UpstreamId>(std::string_view)> resolve_upstream;
  // Route sets that must exist in the config (typically: every route name
  // referenced by a live listener). Prevents removing a route set that is
  // still in use.
  std::vector<std::string> required_route_sets;
  // Fixed hash seed, for tests and benchmarks. Random if unset.
  std::optional<uint64_t> seed;
};

class RouteTable {
public:
  struct Stats {
    uint32_t route_sets{0};
    uint32_t vhosts{0};
    uint32_t locations{0};
    uint32_t host_entries{0};
    std::size_t index_bytes{0};
  };

  RouteTable(const RouteTable &) = delete;
  RouteTable &operator=(const RouteTable &) = delete;

  // Validates `config` and builds a table. On error nothing is produced and
  // the message names the offending route set / vhost / location.
  [[nodiscard]] static std::expected<std::shared_ptr<const RouteTable>,
                                     std::string>
  compile(const RoutingConfig &config, const CompileOptions &options = {});

  // Resolves a route set name (bind time, NOT for the request path). The
  // returned id is only valid for this table.
  [[nodiscard]] RouteSetId
  find_route_set(std::string_view name) const noexcept {
    const auto it = set_names_.find(name);
    if (it == set_names_.end()) {
      return {};
    }
    return {it->second, generation_};
  }

  [[nodiscard]] uint32_t generation() const noexcept { return generation_; }
  [[nodiscard]] Stats stats() const noexcept;

  // Hot path. `host` is the raw Host header value (may be empty, may carry a
  // port); `target` is the raw request-target (path + optional "?query").
  [[nodiscard]] Match match(RouteSetId set_id, std::string_view host,
                            std::string_view target,
                            MatchScratch &scratch) const noexcept {
    Match m;
    if (set_id.generation != generation_ || set_id.index >= sets_.size())
        [[unlikely]] {
      m.status = MatchStatus::Unavailable;
      return m;
    }
    const RouteSet &set = sets_[set_id.index];

    std::string_view path = target;
    if (const char *q =
            RouteDetail::find_byte(target.data(), target.size(), '?');
        q != nullptr) {
      const auto qpos = static_cast<std::size_t>(q - target.data());
      path = target.substr(0, qpos);
      m.query = target.substr(qpos + 1);
    }
    if (path.size() > kMaxTargetLen) [[unlikely]] {
      m.status = MatchStatus::PathTooLong;
      return m;
    }
    if (!is_canonical_path(path)) {
      const auto n = normalize_path(path, scratch.buf);
      if (!n) {
        m.status = MatchStatus::BadPath;
        return m;
      }
      path = std::string_view(scratch.buf, *n);
    }
    m.path = path;

    uint32_t vhost = set.default_vhost;
    if (set.fixed_vhost != kNone) {
      vhost = set.fixed_vhost;
    } else if (set.host_count != 0 && !host.empty()) {
      bool bad = false;
      const uint32_t found = find_vhost(set_id.index, set, host, bad);
      if (bad) [[unlikely]] {
        m.status = MatchStatus::BadHost;
        return m;
      }
      if (found != kNone) {
        vhost = found;
      }
    }
    if (vhost == kNone) {
      m.status = MatchStatus::NotFound;
      return m;
    }

    match_path(vhost, path, m);
    return m;
  }

private:
  static constexpr uint32_t kNone = FlatIndex::kNone;
  static constexpr uint64_t kWildTag = 0xa0761d6478bd642full;
  static constexpr uint64_t kExactTag = 0xe7037ed1a0b428dbull;
  static constexpr unsigned kMaxLabels = RouteDetail::kMaxHostLabels;

  struct Loc {
    uint32_t action;
    uint32_t vhost;
    uint32_t path_off;
    uint32_t path_len;
    bool exact;
  };

  struct VHost {
    uint64_t salt{0};
    uint64_t prefix_mask{0}; // depth_bit(d) for every prefix location
    uint64_t exact_mask{0};  // depth_bit(steps) for every exact location
    uint32_t max_prefix_depth{0};
    uint32_t max_exact_depth{0}; // in chain steps (a trailing '/' counts)
    uint32_t root_prefix{kNone}; // action of prefix "/"
    uint32_t root_exact{kNone};  // action of exact "/"
  };

  struct RouteSet {
    uint64_t salt{0};
    uint64_t wildcard_mask{0}; // label_bit(k) for every "*.<k labels>"
    uint32_t default_vhost{kNone};
    // Set when the route set has exactly one vhost and it is the default:
    // the Host header cannot change the outcome, so it is not even looked at.
    uint32_t fixed_vhost{kNone};
    uint32_t host_count{0};
  };

  struct HostEntry {
    uint32_t set;
    uint32_t vhost;
    uint32_t str_off;
    uint32_t str_len;
    bool wildcard;
  };

  RouteTable() = default;

  // Returns the vhost index or kNone; sets `bad` for malformed hosts.
  [[nodiscard]] uint32_t find_vhost(uint32_t set_idx, const RouteSet &set,
                                    std::string_view host,
                                    bool &bad) const noexcept {
    host = RouteDetail::strip_host_port(host);
    if (host.empty() || host.size() > RouteDetail::kMaxHostLen) [[unlikely]] {
      bad = true;
      return kNone;
    }
    char lower[RouteDetail::kMaxHostLen];
    RouteDetail::lower_copy(lower, host.data(), host.size());
    const std::string_view h(lower, host.size());

    const char *str_base = host_strings_.data();

    // 1. exact name: one hash over the whole (lower-cased) host.
    const uint64_t exact_key =
        RouteDetail::hash_bytes(h.data(), h.size(), set.salt);
    uint32_t e = host_index_.find(exact_key, [&](uint32_t v) {
      const HostEntry &he = hosts_[v];
      return !he.wildcard && he.set == set_idx && he.str_len == h.size() &&
             std::memcmp(str_base + he.str_off, h.data(), h.size()) == 0;
    });
    if (e != kNone) {
      return hosts_[e].vhost;
    }

    // 2. wildcards, longest suffix first. Only entered if the route set has
    //    wildcards at all. "*.x.y" matches "<one or more labels>.x.y"; walk
    //    the dots left to right, so the first hit is the longest suffix.
    if (set.wildcard_mask == 0) {
      return kNone;
    }
    unsigned labels = 1;
    for (const char c : h) {
      labels += c == '.';
    }
    const std::size_t shortest =
        static_cast<std::size_t>(std::countr_zero(set.wildcard_mask)) + 1;
    const uint64_t wild_salt = set.salt ^ kWildTag;
    std::size_t pos = 0;
    while (labels > shortest) {
      const char *dot =
          RouteDetail::find_byte(h.data() + pos, h.size() - pos, '.');
      if (dot == nullptr) {
        break;
      }
      pos = static_cast<std::size_t>(dot - h.data()) + 1;
      --labels; // labels in h[pos..]
      if ((set.wildcard_mask & RouteDetail::label_bit(labels)) == 0) {
        continue;
      }
      const std::size_t suffix_len = h.size() - pos;
      const uint64_t key =
          RouteDetail::hash_bytes(h.data() + pos, suffix_len, wild_salt);
      e = host_index_.find(key, [&](uint32_t v) {
        const HostEntry &he = hosts_[v];
        return he.wildcard && he.set == set_idx && he.str_len == suffix_len &&
               std::memcmp(str_base + he.str_off, h.data() + pos, suffix_len) ==
                   0;
      });
      if (e != kNone) {
        return hosts_[e].vhost;
      }
    }
    return kNone;
  }

  void match_path(uint32_t vhost_idx, std::string_view path,
                  Match &m) const noexcept {
    const VHost &vh = vhosts_[vhost_idx];
    const char *blob = path_blob_.data();
    const RouteAction *act = nullptr;
    std::size_t prefix_end = 0;

    if (vh.root_prefix != kNone) {
      act = &actions_[vh.root_prefix];
    }

    if (path.size() == 1) {
      if (vh.root_exact != kNone) {
        m.action = &actions_[vh.root_exact];
        m.rest = {};
        m.status = MatchStatus::Ok;
        return;
      }
    } else if ((vh.prefix_mask | vh.exact_mask) != 0) {
      RouteDetail::PathCursor cur(path, seed_, vh.salt);
      const bool has_exact = vh.exact_mask != 0;
      // Nothing can match deeper than the deepest location: stop hashing.
      const unsigned limit =
          has_exact ? std::max(vh.max_prefix_depth, vh.max_exact_depth)
                    : vh.max_prefix_depth;
      bool complete = true; // whole path hashed -> an exact probe is possible
      std::size_t end = 0;
      while (!cur.exhausted()) {
        if (cur.depth() >= limit) {
          complete = false;
          break;
        }
        cur.next(end);
        if ((vh.prefix_mask & RouteDetail::depth_bit(cur.depth())) != 0) {
          const uint32_t li = path_index_.find(cur.hash(), [&](uint32_t v) {
            const Loc &l = locs_[v];
            return !l.exact && l.vhost == vhost_idx && l.path_len == end &&
                   std::memcmp(blob + l.path_off, path.data(), end) == 0;
          });
          if (li != kNone) {
            act = &actions_[locs_[li].action];
            prefix_end = end;
          }
        }
      }
      if (complete && has_exact) {
        cur.finish();
        if ((vh.exact_mask & RouteDetail::depth_bit(cur.depth())) != 0) {
          const uint32_t li =
              path_index_.find(cur.hash() ^ kExactTag, [&](uint32_t v) {
                const Loc &l = locs_[v];
                return l.exact && l.vhost == vhost_idx &&
                       l.path_len == path.size() &&
                       std::memcmp(blob + l.path_off, path.data(),
                                   path.size()) == 0;
              });
          if (li != kNone) {
            m.action = &actions_[locs_[li].action];
            m.rest = {};
            m.status = MatchStatus::Ok;
            return;
          }
        }
      }
    }

    if (act == nullptr) {
      m.status = MatchStatus::NotFound;
      return;
    }
    m.action = act;
    m.rest = path.substr(prefix_end);
    m.status = MatchStatus::Ok;
  }

  uint64_t seed_{0};
  uint32_t generation_{0};

  std::vector<RouteSet> sets_;
  std::vector<VHost> vhosts_;
  std::vector<HostEntry> hosts_;
  std::vector<Loc> locs_;
  std::vector<RouteAction> actions_;
  std::string host_strings_;
  std::string path_blob_;
  FlatIndex host_index_;
  FlatIndex path_index_;
  std::map<std::string, uint32_t, std::less<>> set_names_;
};

} // namespace Coring4
