#include "coring4/ManageService.hpp"
#include "coring4/routing/PathNormalizer.hpp"
#include "coring4/routing/RouteTable.hpp"
#include "coring4/routing/RoutingControl.hpp"
#include "coring4/routing/WorkerRouting.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace Coring4;

static int g_failed = 0;
static int g_checks = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    ++g_checks;                                                                \
    if (!(cond)) {                                                             \
      ++g_failed;                                                              \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__,    \
                   #cond);                                                     \
    }                                                                          \
  } while (0)

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static LocationConfig loc(std::string path, MatchKind kind, ActionConfig a) {
  return LocationConfig{std::move(path), kind, std::move(a)};
}
static ActionConfig respond(uint16_t status, std::string body) {
  return RespondActionConfig{status, std::move(body)};
}
static ActionConfig static_dir(std::string root) {
  return StaticActionConfig{std::move(root), "index.html"};
}

static std::string norm(std::string_view in) {
  std::string out(in.size() + 1, '\0');
  auto n = normalize_path(in, out.data());
  if (!n) {
    return "<bad>";
  }
  out.resize(*n);
  return out;
}

struct Fixture {
  std::shared_ptr<const RouteTable> table;
  RouteSetId set;

  Fixture(RoutingConfig cfg, std::string set_name = "main") {
    CompileOptions opt;
    opt.seed = 12345;
    opt.resolve_upstream = [](std::string_view n) -> std::optional<UpstreamId> {
      if (n == "backend") {
        return UpstreamId{7};
      }
      return std::nullopt;
    };
    auto t = RouteTable::compile(cfg, opt);
    if (!t) {
      std::fprintf(stderr, "compile failed: %s\n", t.error().c_str());
      std::abort();
    }
    table = *t;
    set = table->find_route_set(set_name);
  }

  // Returns the body of a Respond action, or "<status>" otherwise.
  std::string run(std::string_view host, std::string_view target,
                  std::string *rest = nullptr) const {
    MatchScratch scratch;
    Match m = table->match(set, host, target, scratch);
    if (!m) {
      switch (m.status) {
      case MatchStatus::NotFound:
        return "<404>";
      case MatchStatus::BadPath:
        return "<bad-path>";
      case MatchStatus::BadHost:
        return "<bad-host>";
      case MatchStatus::PathTooLong:
        return "<too-long>";
      default:
        return "<unavailable>";
      }
    }
    if (rest != nullptr) {
      *rest = std::string(m.rest);
    }
    return m.action->kind == ActionKind::Respond ? m.action->text : "<action>";
  }
};

static RoutingConfig make_basic() {
  RoutingConfig cfg;
  RouteSetConfig set;
  set.name = "main";

  VHostConfig example;
  example.hosts = {"example.com", "www.example.com"};
  example.locations = {
      loc("/", MatchKind::Prefix, respond(200, "ex-root")),
      loc("/api", MatchKind::Prefix, respond(200, "ex-api")),
      loc("/api/v2", MatchKind::Prefix, respond(200, "ex-api-v2")),
      loc("/api/v2/health", MatchKind::Exact, respond(200, "ex-health")),
      loc("/static/", MatchKind::Prefix, static_dir("/srv/www/")),
      loc("/ex", MatchKind::Exact, respond(200, "ex-exact")),
  };

  VHostConfig wild;
  wild.hosts = {"*.example.com"};
  wild.locations = {loc("/", MatchKind::Prefix, respond(200, "wild"))};

  VHostConfig wild_deep;
  wild_deep.hosts = {"*.eu.example.com"};
  wild_deep.locations = {loc("/", MatchKind::Prefix, respond(200, "wild-eu"))};

  VHostConfig fallback;
  fallback.is_default = true;
  fallback.locations = {loc("/", MatchKind::Prefix, respond(404, "default"))};

  set.vhosts = {example, wild, wild_deep, fallback};
  cfg.route_sets = {set};
  return cfg;
}

// ---------------------------------------------------------------------------
// tests
// ---------------------------------------------------------------------------

static void test_normalize() {
  CHECK(norm("/") == "/");
  CHECK(norm("/a/b") == "/a/b");
  CHECK(norm("/a/b/") == "/a/b/");
  CHECK(norm("//a///b") == "/a/b");
  CHECK(norm("/a/./b") == "/a/b");
  CHECK(norm("/a/b/..") == "/a/");
  CHECK(norm("/a/b/../c") == "/a/c");
  CHECK(norm("/a/b/.") == "/a/b/");
  CHECK(norm("/a/../..") == "<bad>");
  CHECK(norm("/..") == "<bad>");
  CHECK(norm("/%61dmin") == "/admin");
  CHECK(norm("/%2e%2e/etc") == "<bad>");
  CHECK(norm("/a/%2e%2E/b") == "/b");
  CHECK(norm("/a%2fb") == "/a%2Fb"); // encoded slash stays encoded
  CHECK(norm("/a%2Fb") == "/a%2Fb");
  CHECK(norm("/%D0%BF") == "/\xD0\xBF"); // non-ASCII is decoded
  CHECK(norm("/%00") == "<bad>");
  CHECK(norm("/%zz") == "<bad>");
  CHECK(norm("/%4") == "<bad>");
  CHECK(norm("/a b") == "<bad>");
  CHECK(norm("/a\tb") == "<bad>");
  CHECK(norm("/a\x7f") == "<bad>");
  CHECK(norm("a") == "<bad>");
  CHECK(norm("") == "<bad>");
  CHECK(norm("/.hidden") == "/.hidden");
  CHECK(norm("/a/.../b") == "/a/.../b");

  // is_canonical_path must agree with normalize_path being the identity.
  const char *samples[] = {"/",
                           "/a",
                           "/a/",
                           "/a/b",
                           "//",
                           "/a//b",
                           "/./",
                           "/..",
                           "/a/..",
                           "/a/.b",
                           "/.well-known/x",
                           "/%41",
                           "/%2F",
                           "/%2f",
                           "/%D0%BF",
                           "/\xD0\xBF",
                           "/a b",
                           "/a%",
                           "/a%2",
                           "/abcdefghijklmnop/qrstuvwxyz/0123456789",
                           "/abcdefgh/./x",
                           "/abcdefg//x",
                           "/abcdefgh//x",
                           "/a-b_c~d.e"};
  for (const char *s : samples) {
    std::string out(std::string_view(s).size() + 1, '\0');
    auto n = normalize_path(s, out.data());
    const bool identity = n && std::string_view(out.data(), *n) == s;
    CHECK(is_canonical_path(s) == identity);
    CHECK(is_canonical_path_exact(s) == identity);
  }

  // Randomised agreement: SWAR / exact check vs. normalize identity.
  std::srand(1);
  const char alphabet[] = "ab./%2Fe A\t";
  for (int i = 0; i < 20000; ++i) {
    std::string s = "/";
    const int len = std::rand() % 24;
    for (int j = 0; j < len; ++j) {
      s += alphabet[std::rand() % (sizeof(alphabet) - 1)];
    }
    std::string out(s.size() + 1, '\0');
    auto n = normalize_path(s, out.data());
    const bool identity = n && std::string_view(out.data(), *n) == s;
    CHECK(is_canonical_path(s) == identity);
    // idempotent
    if (n) {
      std::string again(*n + 1, '\0');
      auto n2 = normalize_path(std::string_view(out.data(), *n), again.data());
      CHECK(n2 && std::string_view(again.data(), *n2) ==
                      std::string_view(out.data(), *n));
    }
  }

  char buf[64];
  auto d = decode_for_filesystem("/a%20b", buf);
  CHECK(d && std::string_view(buf, *d) == "/a b");
  CHECK(!decode_for_filesystem("/a%2Fb", buf));
  CHECK(!decode_for_filesystem("/a%00b", buf));
}

static void test_hosts() {
  Fixture f(make_basic());
  CHECK(f.run("example.com", "/") == "ex-root");
  CHECK(f.run("EXAMPLE.com", "/") == "ex-root");
  CHECK(f.run("example.com:8080", "/") == "ex-root");
  CHECK(f.run("example.com.", "/") == "ex-root");
  CHECK(f.run("www.example.com", "/") == "ex-root");
  CHECK(f.run("api.example.com", "/") == "wild");
  CHECK(f.run("a.b.example.com", "/") == "wild");     // wildcard spans labels
  CHECK(f.run("x.eu.example.com", "/") == "wild-eu"); // longest suffix wins
  CHECK(f.run("eu.example.com", "/") == "wild");      // not "*.eu.example.com"
  CHECK(f.run("other.org", "/") == "default");
  CHECK(f.run("", "/") == "default");             // no Host header
  CHECK(f.run("xexample.com", "/") == "default"); // not a suffix on a dot
  CHECK(f.run("notexample.com", "/") == "default");
  CHECK(f.run("a..b", "/") == "default"); // unknown, but not a crash
  CHECK(f.run(":80", "/") == "<bad-host>");
}

static void test_paths() {
  Fixture f(make_basic());
  std::string rest;
  CHECK(f.run("example.com", "/") == "ex-root");
  CHECK(f.run("example.com", "/nothing", &rest) == "ex-root");
  CHECK(rest == "/nothing");
  CHECK(f.run("example.com", "/api", &rest) == "ex-api");
  CHECK(rest == "");
  CHECK(f.run("example.com", "/api/", &rest) == "ex-api");
  CHECK(rest == "/");
  CHECK(f.run("example.com", "/api/users/1", &rest) == "ex-api");
  CHECK(rest == "/users/1");
  CHECK(f.run("example.com", "/apix") == "ex-root"); // segment boundary
  CHECK(f.run("example.com", "/api/v2/x", &rest) == "ex-api-v2");
  CHECK(rest == "/x");
  CHECK(f.run("example.com", "/api/v2/health") == "ex-health"); // exact wins
  CHECK(f.run("example.com", "/api/v2/health/") == "ex-api-v2");
  CHECK(f.run("example.com", "/api/v2/healthz") == "ex-api-v2");
  CHECK(f.run("example.com", "/ex") == "ex-exact");
  CHECK(f.run("example.com", "/ex/") == "ex-root");
  CHECK(f.run("example.com", "/ex/y") == "ex-root");
  CHECK(f.run("example.com", "/static/a.css") == "<action>");
  CHECK(f.run("example.com", "/api?x=1", &rest) == "ex-api");
  CHECK(f.run("example.com", "/api/v2/health?x=1") == "ex-health");

  MatchScratch scratch;
  Match m =
      f.table->match(f.set, "example.com", "/static/a/b.css?v=3", scratch);
  CHECK(m && m.action->kind == ActionKind::Static);
  CHECK(m.action->text == "/srv/www");
  CHECK(m.rest == "/a/b.css");
  CHECK(m.query == "v=3");
}

static void test_path_security() {
  Fixture f(make_basic());
  // Everything below must be routed exactly like the canonical spelling.
  CHECK(f.run("example.com", "/%61pi/v2/health") == "ex-health");
  CHECK(f.run("example.com", "/api/./v2/health") == "ex-health");
  CHECK(f.run("example.com", "//api//v2//health") == "ex-health");
  CHECK(f.run("example.com", "/static/../api/v2/health") == "ex-health");
  CHECK(f.run("example.com", "/api/v2/health/..") == "ex-api-v2");
  CHECK(f.run("example.com", "/%2e%2e/api") == "<bad-path>");
  CHECK(f.run("example.com", "/api/%00") == "<bad-path>");
  CHECK(f.run("example.com", "/a b") == "<bad-path>");
  // Encoded slash must NOT become a separator.
  CHECK(f.run("example.com", "/api%2Fv2/health") == "ex-root");

  std::string big(kMaxTargetLen + 10, 'a');
  big[0] = '/';
  CHECK(f.run("example.com", big) == "<too-long>");
}

static void test_defaults_and_missing() {
  // No default vhost and no root location -> 404.
  RoutingConfig cfg;
  RouteSetConfig s;
  s.name = "main";
  VHostConfig v;
  v.hosts = {"a.test"};
  v.locations = {loc("/x", MatchKind::Prefix, respond(200, "x"))};
  s.vhosts = {v};
  cfg.route_sets = {s};
  Fixture f(cfg);
  CHECK(f.run("a.test", "/x") == "x");
  CHECK(f.run("a.test", "/y") == "<404>");
  CHECK(f.run("b.test", "/x") == "<404>");
  CHECK(f.run("", "/x") == "<404>");
}

static void test_route_set_isolation() {
  RoutingConfig cfg;
  for (const char *name : {"one", "two"}) {
    RouteSetConfig s;
    s.name = name;
    VHostConfig v;
    v.hosts = {"same.test"};
    v.locations = {loc("/p", MatchKind::Prefix, respond(200, name))};
    s.vhosts = {v};
    cfg.route_sets.push_back(s);
  }
  Fixture f1(cfg, "one");
  Fixture f2(cfg, "two");
  CHECK(f1.run("same.test", "/p") == "one");
  CHECK(f2.run("same.test", "/p") == "two");

  // A RouteSetId from another table is rejected, not misinterpreted.
  MatchScratch scratch;
  Match m = f1.table->match(f2.set, "same.test", "/p", scratch);
  CHECK(m.status == MatchStatus::Unavailable);
  CHECK(!f1.table->find_route_set("nope").is_valid());
}

static void test_compile_errors() {
  auto try_compile = [](RoutingConfig c, CompileOptions o = {}) {
    return RouteTable::compile(c, o).has_value();
  };
  auto base = [] {
    RoutingConfig cfg;
    RouteSetConfig s;
    s.name = "main";
    VHostConfig v;
    v.hosts = {"a.test"};
    v.locations = {loc("/", MatchKind::Prefix, respond(200, "ok"))};
    s.vhosts = {v};
    cfg.route_sets = {s};
    return cfg;
  };

  CHECK(try_compile(base()));

  {
    auto c = base();
    c.route_sets[0].name = "";
    CHECK(!try_compile(c));
  }
  {
    auto c = base();
    c.route_sets.push_back(c.route_sets[0]);
    CHECK(!try_compile(c));
  }
  {
    auto c = base();
    c.route_sets[0].vhosts[0].hosts = {"a.test:80"};
    CHECK(!try_compile(c));
  }
  {
    auto c = base();
    c.route_sets[0].vhosts[0].hosts = {"a..test"};
    CHECK(!try_compile(c));
  }
  {
    auto c = base();
    c.route_sets[0].vhosts[0].hosts = {
        "a.test.",
    };
    CHECK(!try_compile(c));
  }
  {
    auto c = base();
    c.route_sets[0].vhosts[0].hosts = {"*"};
    CHECK(!try_compile(c));
  }
  {
    auto c = base();
    c.route_sets[0].vhosts[0].hosts = {"a.test", "A.TEST"};
    CHECK(!try_compile(c));
  }
  {
    auto c = base();
    c.route_sets[0].vhosts[0].hosts.clear();
    CHECK(!try_compile(c));
  }
  {
    auto c = base();
    c.route_sets[0].vhosts[0].locations.clear();
    CHECK(!try_compile(c));
  }
  {
    auto c = base();
    c.route_sets[0].vhosts[0].locations[0].path = "api";
    CHECK(!try_compile(c));
  }
  {
    auto c = base();
    c.route_sets[0].vhosts[0].locations[0].path = "/a/../b";
    CHECK(!try_compile(c));
  }
  {
    auto c = base();
    c.route_sets[0].vhosts[0].locations[0].path = "/a?b";
    CHECK(!try_compile(c));
  }
  {
    auto c = base();
    c.route_sets[0].vhosts[0].locations.push_back(
        c.route_sets[0].vhosts[0].locations[0]);
    CHECK(!try_compile(c));
  }
  {
    auto c = base();
    c.route_sets[0].vhosts[0].is_default = true;
    auto v2 = c.route_sets[0].vhosts[0];
    v2.hosts = {"b.test"};
    c.route_sets[0].vhosts.push_back(v2);
    CHECK(!try_compile(c));
  } // two defaults
  {
    auto c = base();
    c.route_sets[0].vhosts[0].locations[0].action =
        StaticActionConfig{"relative", "index.html"};
    CHECK(!try_compile(c));
  }
  {
    auto c = base();
    c.route_sets[0].vhosts[0].locations[0].action =
        RedirectActionConfig{200, "/x", false};
    CHECK(!try_compile(c));
  }
  {
    auto c = base();
    c.route_sets[0].vhosts[0].locations[0].action =
        ProxyActionConfig{"missing", false};
    CompileOptions o;
    o.resolve_upstream = [](std::string_view) { return std::nullopt; };
    CHECK(!try_compile(c, o));
    CHECK(!try_compile(c)); // no resolver at all
  }
  {
    CompileOptions o;
    o.required_route_sets = {"main", "gone"};
    CHECK(!try_compile(base(), o));
  }

  // Proxy resolution happens at compile time.
  auto c = base();
  c.route_sets[0].vhosts[0].locations[0].action =
      ProxyActionConfig{"backend", true};
  Fixture f(c);
  MatchScratch scratch;
  Match m = f.table->match(f.set, "a.test", "/anything", scratch);
  CHECK(m && m.action->kind == ActionKind::Proxy &&
        m.action->upstream.value == 7 && m.action->flag);
}

// Force massive collisions: if verification were missing, routes would mix.
static void test_many_routes_and_collision_safety() {
  RoutingConfig cfg;
  RouteSetConfig s;
  s.name = "main";
  constexpr int kHosts = 2000;
  for (int i = 0; i < kHosts; ++i) {
    VHostConfig v;
    v.hosts = {"h" + std::to_string(i) + ".bench.test"};
    for (int j = 0; j < 5; ++j) {
      v.locations.push_back(
          loc("/p" + std::to_string(j) + "/q", MatchKind::Prefix,
              respond(200, std::to_string(i) + ":" + std::to_string(j))));
    }
    v.locations.push_back(
        loc("/", MatchKind::Prefix, respond(200, std::to_string(i) + ":root")));
    s.vhosts.push_back(std::move(v));
  }
  cfg.route_sets = {s};
  Fixture f(cfg);
  for (int i = 0; i < kHosts; i += 7) {
    const std::string host = "h" + std::to_string(i) + ".bench.test";
    for (int j = 0; j < 5; ++j) {
      CHECK(f.run(host, "/p" + std::to_string(j) + "/q/x") ==
            std::to_string(i) + ":" + std::to_string(j));
    }
    CHECK(f.run(host, "/zzz") == std::to_string(i) + ":root");
  }
  CHECK(f.run("h2000.bench.test", "/") == "<404>");
  CHECK(f.run("bench.test", "/") == "<404>");
}

static void test_worker_routing_swap() {
  auto make = [](const char *body) {
    RoutingConfig cfg;
    RouteSetConfig s;
    s.name = "main";
    VHostConfig v;
    v.is_default = true;
    v.locations = {loc("/", MatchKind::Prefix, respond(200, body))};
    s.vhosts = {v};
    cfg.route_sets = {s};
    return *RouteTable::compile(cfg);
  };

  WorkerRouting wr;
  RouteBinding b = wr.bind("main");
  CHECK(wr.match(b, "x", "/").status == MatchStatus::Unavailable); // no table

  CHECK(wr.install(make("v1")).has_value());
  auto m1 = wr.match(b, "x", "/");
  CHECK(m1 && m1.action->text == "v1");

  CHECK(wr.install(make("v2")).has_value());
  auto m2 = wr.match(b, "x", "/"); // same binding, new table
  CHECK(m2 && m2.action->text == "v2");

  // A table that lacks a bound set is rejected; the old one stays.
  RoutingConfig other;
  RouteSetConfig os;
  os.name = "other";
  VHostConfig ov;
  ov.is_default = true;
  ov.locations = {loc("/", MatchKind::Prefix, respond(200, "other"))};
  os.vhosts = {ov};
  other.route_sets = {os};
  CHECK(!wr.install(*RouteTable::compile(other)).has_value());
  auto m3 = wr.match(b, "x", "/");
  CHECK(m3 && m3.action->text == "v2");

  // Binding made after a table exists resolves at once.
  RouteBinding b2 = wr.bind("main");
  CHECK(b2.value == b.value);
  CHECK(!wr.bind("unknown").is_valid() ==
        false); // bind() always returns a slot
  CHECK(wr.match(wr.bind("unknown"), "x", "/").status ==
        MatchStatus::Unavailable);
}

// Real rollout through ManageService: control thread + workers swapping tables
// while "requests" are matched. Run under -fsanitize=thread to check races.
static void test_rollout_concurrent() {
  constexpr int kWorkers = 4;
  constexpr int kUpdates = 200;

  auto make_cfg = [](int version) {
    RoutingConfig cfg;
    RouteSetConfig s;
    s.name = "main";
    VHostConfig v;
    v.is_default = true;
    v.locations = {loc("/", MatchKind::Prefix,
                       respond(200, "v" + std::to_string(version)))};
    s.vhosts = {v};
    cfg.route_sets = {s};
    return cfg;
  };

  Manage::ManageService manage;
  RoutingControl control(manage);
  control.require_route_set("main");

  struct W {
    std::shared_ptr<Manage::WorkerChannel> ch =
        std::make_shared<Manage::WorkerChannel>();
    WorkerRouting routing;
    RouteBinding binding;
    std::atomic<int> last_version{0};
    std::atomic<bool> bad{false};
  };
  std::vector<std::unique_ptr<W>> workers;
  for (int i = 0; i < kWorkers; ++i) {
    auto w = std::make_unique<W>();
    w->ch->ev_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    w->binding = w->routing.bind("main");
    workers.push_back(std::move(w));
  }

  CHECK(control.apply(make_cfg(1)).has_value()); // before any worker attaches
  for (auto &w : workers) {
    control.attach_worker(w->ch); // gets the current table
  }

  std::atomic<bool> stop{false};
  std::vector<std::thread> threads;
  for (auto &wp : workers) {
    threads.emplace_back([&stop, w = wp.get()] {
      // The worker loop: drain manage requests, then serve "requests".
      while (!stop.load(std::memory_order_relaxed)) {
        for (auto &req : w->ch->pop_all()) {
          if (auto *u =
                  std::get_if<Manage::UpdateRoutesRequest>(&req.request)) {
            auto r = w->routing.install(u->table);
            if (u->ack) {
              u->ack->complete(r);
            }
          }
        }
        int prev = w->last_version.load(std::memory_order_relaxed);
        for (int i = 0; i < 64; ++i) {
          auto m = w->routing.match(w->binding, "h", "/x");
          if (m) {
            const int v = std::stoi(m.action->text.substr(1));
            if (v < prev) { // versions must never go backwards
              w->bad = true;
            }
            prev = v;
          }
        }
        w->last_version.store(prev, std::memory_order_relaxed);
      }
    });
  }

  for (int v = 2; v <= kUpdates; ++v) {
    auto ack = control.apply(make_cfg(v));
    CHECK(ack.has_value());
    CHECK((*ack)->wait_for(std::chrono::seconds(5)));
    CHECK((*ack)->failed() == 0);
  }

  // Invalid configs never reach workers and do not disturb the running table.
  RoutingConfig broken = make_cfg(999);
  broken.route_sets[0].vhosts[0].locations[0].path = "no-slash";
  CHECK(!control.apply(broken).has_value());
  RoutingConfig dropped; // would drop the required set "main"
  CHECK(!control.apply(dropped).has_value());

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  stop = true;
  for (auto &t : threads) {
    t.join();
  }
  for (auto &w : workers) {
    CHECK(!w->bad.load());
    CHECK(w->last_version.load() == kUpdates);
    ::close(w->ch->ev_fd);
  }
}

int main() {
  test_normalize();
  test_hosts();
  test_paths();
  test_path_security();
  test_defaults_and_missing();
  test_route_set_isolation();
  test_compile_errors();
  test_many_routes_and_collision_safety();
  test_worker_routing_swap();
  test_rollout_concurrent();

  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
