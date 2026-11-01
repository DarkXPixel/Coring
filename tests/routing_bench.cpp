#include "coring4/routing/RouteTable.hpp"
#include <chrono>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

using namespace Coring4;

static LocationConfig loc(std::string path, MatchKind kind, std::string body) {
  return LocationConfig{std::move(path), kind,
                        RespondActionConfig{200, std::move(body)}};
}

struct Req {
  std::string host;
  std::string target;
};

static std::shared_ptr<const RouteTable> build(int hosts) {
  RoutingConfig cfg;
  RouteSetConfig s;
  s.name = "main";
  for (int i = 0; i < hosts; ++i) {
    VHostConfig v;
    v.hosts = {"site" + std::to_string(i) + ".example.com"};
    v.locations = {
        loc("/", MatchKind::Prefix, "root"),
        loc("/api", MatchKind::Prefix, "api"),
        loc("/api/v1/users", MatchKind::Prefix, "users"),
        loc("/static", MatchKind::Prefix, "static"),
        loc("/health", MatchKind::Exact, "health"),
    };
    s.vhosts.push_back(std::move(v));
  }
  VHostConfig wild;
  wild.hosts = {"*.wild.example.com"};
  wild.locations = {loc("/", MatchKind::Prefix, "wild")};
  s.vhosts.push_back(std::move(wild));
  cfg.route_sets = {s};
  return *RouteTable::compile(cfg);
}

static void run(const char *name, const RouteTable &table, RouteSetId id,
                const std::vector<Req> &reqs) {
  MatchScratch scratch;
  constexpr std::size_t kIters = 20'000'000;
  const std::size_t mask = reqs.size() - 1; // size is a power of two
  std::size_t sink = 0;
  const auto t0 = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < kIters; ++i) {
    const Req &r = reqs[i & mask];
    const Match m = table.match(id, r.host, r.target, scratch);
    sink += static_cast<std::size_t>(m.status) +
            (m.action != nullptr ? m.action->text.size() : 0) + m.rest.size();
  }
  const auto t1 = std::chrono::steady_clock::now();
  std::printf("%-52s %6.1f ns/op  (%zu)\n", name,
              std::chrono::duration<double, std::nano>(t1 - t0).count() /
                  static_cast<double>(kIters),
              sink % 10);
}

static std::vector<Req> make_reqs(int hosts, std::size_t n,
                                  const char *host_sfx,
                                  std::vector<const char *> paths) {
  std::vector<Req> r;
  for (std::size_t i = 0; i < n; ++i) {
    r.push_back({"site" + std::to_string((i * 7919) % hosts) + host_sfx,
                 paths[i % paths.size()]});
  }
  return r;
}

int main() {
  constexpr int kHosts = 1000;
  auto big = build(kHosts);
  auto small = build(1);
  const auto big_id = big->find_route_set("main");
  const auto small_id = small->find_route_set("main");
  const auto st = big->stats();
  std::printf("table: %u vhosts, %u locations, index %zu KiB\n\n", st.vhosts,
              st.locations, st.index_bytes / 1024);

  const std::vector<const char *> mixed = {"/", "/api/v1/users/42?x=1",
                                           "/static/css/app.css", "/health",
                                           "/nope/deep/path/here"};

  // Hot: one vhost, everything in L1.
  run("1 vhost,    '/'                (L1-hot)", *small, small_id,
      make_reqs(1, 4096, ".example.com", {"/"}));
  run("1 vhost,    mixed paths        (L1-hot)", *small, small_id,
      make_reqs(1, 4096, ".example.com", mixed));
  run("1 vhost,    mixed, Host with :port", *small, small_id,
      make_reqs(1, 4096, ".example.com:8443", mixed));

  // Cold: 1000 vhosts, random order, working set > L1.
  run("1000 vhosts, '/'               (random)", *big, big_id,
      make_reqs(kHosts, 4096, ".example.com", {"/"}));
  run("1000 vhosts, mixed paths       (random)", *big, big_id,
      make_reqs(kHosts, 4096, ".example.com", mixed));
  run("1000 vhosts, mixed, Host with :port", *big, big_id,
      make_reqs(kHosts, 4096, ".example.com:8443", mixed));
  run("1000 vhosts, wildcard host", *big, big_id,
      {{"a.b.wild.example.com", "/x"}, {"c.wild.example.com", "/y"}});

  // Reference: the shape of the old Router (string map host -> string map
  // path).
  std::unordered_map<std::string, std::unordered_map<std::string, int>> base;
  for (int i = 0; i < kHosts; ++i) {
    base["site" + std::to_string(i) + ".example.com:8443"]["/"] = 1;
  }
  const auto reqs = make_reqs(kHosts, 4096, ".example.com:8443", {"/"});
  std::size_t sink = 0;
  constexpr std::size_t kIters = 20'000'000;
  const auto t0 = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < kIters; ++i) {
    const Req &r = reqs[i & 4095];
    auto it = base.find(r.host);
    if (it != base.end()) {
      auto it2 = it->second.find("/");
      sink += it2 != it->second.end() ? it2->second : 0;
    }
  }
  const auto t1 = std::chrono::steady_clock::now();
  std::printf("\n%-52s %6.1f ns/op  (%zu)\n",
              "reference: unordered_map<string> host+'/' only",
              std::chrono::duration<double, std::nano>(t1 - t0).count() /
                  static_cast<double>(kIters),
              sink % 10);
  return 0;
}
