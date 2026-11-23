#include "coring5/config/Diff.hpp"
#include "coring5/config/Loader.hpp"
#include <gtest/gtest.h>

using namespace Coring5;

namespace {
constexpr ListenerId kWeb{8};
constexpr std::string_view kListener = R"(
[[listener]]
name = "web"
protocol = "http1"
address = ":0"
route = "main"
)";
constexpr std::string_view kRoutes = R"(
[[route_set]]
name = "main"
[[route_set.vhost]]
hosts = ["Example.com", "www.example.com"]
location = [
  {path="/", action="respond", body="root"},
  {path="/api", action="respond", body="api"},
  {path="/api/v2/", action="respond", body="version"},
  {path="/api/v2", match="exact", action="respond", body="exact"}
]
[[route_set.vhost]]
hosts = ["*.example.com"]
location = [{path="/", action="respond", body="wild"}]
[[route_set.vhost]]
hosts = ["*.eu.example.com"]
location = [{path="/", action="respond", body="eu"}]
[[route_set.vhost]]
default = true
location = [{path="/", action="respond", body="default"}]
)";
std::string with_listener(std::string_view routes) {
  return std::string(kListener).append(routes);
}
std::expected<ConfigSnapshot, std::string> snapshot(std::string_view config) {
  auto parsed = parse_config(config);
  if (!parsed) {
    return std::unexpected(parsed.error());
  }
  if (auto ok = validate_config(*parsed); !ok) {
    return std::unexpected(ok.error());
  }
  ConfigSnapshot s;
  s.listeners = std::move(parsed->listeners);
  if (!s.listeners.empty()) {
    s.listeners.front().id = kWeb;
  }
  s.upstreams = std::move(parsed->upstreams);
  if (!s.upstreams.empty()) {
    s.upstreams.front().id = UpstreamId{73};
  }
  s.install_routes(std::move(parsed->routes));
  return s;
}
std::string_view body(const ConfigSnapshot &s, std::string_view host,
                      std::string_view target) {
  std::array<char, 256> scratch;
  const auto m = s.match(kWeb, host, target, scratch);
  if (!m.action) {
    return {};
  }
  return std::get<RespondRoute>(*m.action).body();
}
std::string single(std::string_view location) {
  return std::string("[[route_set]]\nname='main'\n[[route_set.vhost]]\ndefault="
                     "true\nlocation=[{") +
         std::string(location) + "}]\n";
}
} // namespace

TEST(Routes, UsesHostThenPathPrecedence) {
  auto s = snapshot(with_listener(kRoutes));
  ASSERT_TRUE(s) << s.error();
  EXPECT_EQ(body(*s, "EXAMPLE.COM.:8080", "/api/v2?q"), "exact");
  EXPECT_EQ(body(*s, "www.example.com", "//api/%76%32/./x"), "version");
  EXPECT_EQ(body(*s, "example.com", "/api/v2/"), "version");
  EXPECT_EQ(body(*s, "example.com", "/api/v2ish"), "api");
  EXPECT_EQ(body(*s, "example.com", "/apix"), "root");
  EXPECT_EQ(body(*s, "example.com", "/api%2Fv2"), "root");
  EXPECT_EQ(body(*s, "x.eu.example.com", "/api/v2"), "eu");
  EXPECT_EQ(body(*s, "eu.example.com", "/api/v2"), "wild");
  EXPECT_EQ(body(*s, "other.test", "/"), "default");
  EXPECT_EQ(body(*s, "", "/"), "default");
  EXPECT_EQ(body(*s, "[::1]:80", "/"), "default");
  EXPECT_TRUE(body(*s, "example.com:x", "/").empty());
  EXPECT_TRUE(body(*s, ".example.com", "/").empty());
  EXPECT_TRUE(body(*s, "example.com", "/../x").empty());
  std::array<char, 32> scratch;
  auto match = s->match(kWeb, "example.com", "/api/x/../y?q", scratch);
  ASSERT_NE(match.action, nullptr);
  EXPECT_EQ(match.path, "/api/y");
  EXPECT_EQ(match.remainder, "/y");
  EXPECT_FALSE(s->match(ListenerId{9}, "example.com", "/", scratch).action);
  EXPECT_FALSE(s->match(kWeb, "example.com", "/api", {}).action);
}

TEST(Routes, ValidatesActionPayloadsAndResolvesSnapshotUpstream) {
  auto text = with_listener(R"(
[[upstream]]
name = "backend"
backends = ["127.0.0.1:9000"]
[[route_set]]
name = "main"
[[route_set.vhost]]
default = true
location = [
  {path="/p", action="reverse_proxy", upstream="backend"},
  {path="/s", action="static_file", root="/srv/site"},
  {path="/r", action="redirect", status=307, target="/new"}
]
)");
  auto s = snapshot(text);
  ASSERT_TRUE(s) << s.error();
  std::array<char, 32> scratch;
  auto proxy = s->match(kWeb, "", "/p", scratch);
  ASSERT_NE(proxy.action, nullptr);
  EXPECT_EQ(std::get<ProxyRoute>(*proxy.action).upstream_id, UpstreamId{73});
  auto file = s->match(kWeb, "", "/s", scratch);
  ASSERT_NE(file.action, nullptr);
  EXPECT_EQ(std::get<StaticRoute>(*file.action).root, "/srv/site");
  auto redirect = s->match(kWeb, "", "/r", scratch);
  ASSERT_NE(redirect.action, nullptr);
  EXPECT_EQ(std::get<RedirectRoute>(*redirect.action),
            (RedirectRoute{307, "/new"}));
  EXPECT_FALSE(s->match(kWeb, "", "/unmatched", scratch).action);
}

TEST(Routes, UnboundListenerHasNoMatch) {
  auto s = snapshot(
      "[[listener]]\nname='web'\nprotocol='http1'\naddress=':0'\nroute=''\n");
  ASSERT_TRUE(s) << s.error();
  EXPECT_TRUE(body(*s, "example.com", "/").empty());
}

TEST(Routes, RejectsInvalidTablesBindingsAndActions) {
  const auto rejects = [](std::string text, std::string_view reason) {
    SCOPED_TRACE(text);
    auto s = snapshot(text);
    ASSERT_FALSE(s);
    EXPECT_NE(s.error().find(reason), std::string::npos) << s.error();
  };
  rejects("route_set=3", "array of tables");
  rejects("[[route_set]]\nname='main'", "missing vhosts");
  rejects(std::string(kRoutes).append(kRoutes), "duplicate route set");
  rejects(std::string(kRoutes) + "[[route_set.vhost]]\nhosts=['example.COM']",
          "duplicate host");
  rejects(std::string(kRoutes) + "[[route_set.vhost]]\ndefault=true",
          "duplicate default");
  rejects(std::string(kRoutes) + "[[route_set.vhost]]\nhosts=['a..b']",
          "invalid host");
  rejects(std::string(kListener), "unknown route set");
  rejects("[[listener]]\nname='web'\naddress=':0'\nroute=3",
          "route must be a string");
  rejects(std::string("[[listener]]\nname='echo'\naddress=':0'\nroute='main'\n")
              .append(kRoutes),
          "requires HTTP");
  rejects(single("path='/x/..', action='respond'"), "canonical");
  rejects(single("path='/', match='regexp', action='respond'"),
          "exact or prefix");
  rejects(single("path='/', action='x'"), "route action");
  rejects(single("path='/', action='respond', root='/x'"),
          "unexpected route key");
  rejects(single("path='/', action='respond', status=204, body='x'"),
          "invalid respond");
  rejects(single("path='/', action='respond', status='200'"),
          "invalid route status");
  rejects(single("path='/', action='redirect', target='bad target'"),
          "invalid redirect");
  rejects(single("path='/', action='static_file', root='relative'"),
          "absolute directory");
  rejects(single("path='/', action='reverse_proxy', upstream='gone'"),
          "unknown proxy upstream");
  rejects(
      single("path='/api', action='respond'}, {path='/api/', action='respond'"),
      "duplicate location");
}

TEST(Routes, DiffTracksCatalogueAndKeptListenerRebindingByValue) {
  const auto text =
      with_listener(kRoutes) +
      "[[route_set]]\nname='other'\n[[route_set.vhost]]\n"
      "default=true\nlocation=[{path='/', action='respond', body='other'}]";
  auto old = snapshot(text);
  ASSERT_TRUE(old) << old.error();
  auto next = snapshot(text);
  ASSERT_TRUE(next) << next.error();
  EXPECT_TRUE(diff_snapshots(*old, *next).empty());
  // Moving the first vhost behind the second changes its internal index.
  auto reordered = text;
  const auto first = reordered.find("[[route_set.vhost]]");
  const auto second = reordered.find("[[route_set.vhost]]", first + 1);
  const auto third = reordered.find("[[route_set.vhost]]", second + 1);
  auto first_host = reordered.substr(first, second - first);
  reordered.insert(third, first_host);
  reordered.erase(first, first_host.size());
  auto equivalent = snapshot(reordered);
  ASSERT_TRUE(equivalent) << equivalent.error();
  EXPECT_TRUE(diff_snapshots(*old, *equivalent).empty());
  next->listeners[0].route = "other";
  auto d = diff_snapshots(*old, *next);
  EXPECT_TRUE(d.routes_changed);
  EXPECT_FALSE(d.empty());
  EXPECT_EQ(d.kept, std::vector<ListenerId>{kWeb});
  EXPECT_TRUE(d.added.empty());
  EXPECT_TRUE(d.removed.empty());
  EXPECT_EQ(body(*old, "example.com", "/"), "root");
  EXPECT_EQ(body(*next, "example.com", "/"), "other");
  next->listeners[0].route = "main";
  next->routes.erase("other");
  EXPECT_TRUE(diff_snapshots(*old, *next).routes_changed);
  auto changed = text;
  changed.replace(changed.find("exact\"}"), 5, "new");
  auto edited = snapshot(changed);
  ASSERT_TRUE(edited) << edited.error();
  EXPECT_TRUE(diff_snapshots(*old, *edited).routes_changed);
  EXPECT_EQ(body(*edited, "example.com", "/api/v2"), "new");
  EXPECT_EQ(body(*old, "example.com", "/api/v2"), "exact");
}
