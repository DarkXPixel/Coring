#include "support/TestServer.hpp"
#include "coring5/net/ListenSocket.hpp"
#include <atomic>
#include <chrono>
#include <thread>

using namespace Coring5;
using namespace Coring5::testing;
using namespace std::chrono_literals;

namespace {
// Free port: bind port 0, read it, close. Small race window, fine for tests.
uint16_t pick_port() {
  auto ep = Endpoint::parse("127.0.0.1:0");
  auto fd = create_tcp_listener(*ep, ListenOptions{.reuse_port = false});
  auto b = local_endpoint(fd->get());
  return b->port();
}

bool echoes(uint16_t port) {
  Client c(port);
  return c.connected() && c.send_all("ping") && c.recv_n(4) == "ping";
}
} // namespace

TEST(Reload, AddsAndRemovesListenersWithoutRestart) {
  CORING5_REQUIRE_RING();
  const auto pa = pick_port();
  const auto pb = pick_port();
  const auto A = "127.0.0.1:" + std::to_string(pa);
  const auto B = "127.0.0.1:" + std::to_string(pb);

  Stack s;
  ASSERT_EQ(s.start(make_config(2, {{"a", A.c_str()}})), "");
  EXPECT_TRUE(echoes(pa));
  {
    Client c(pb);
    EXPECT_FALSE(c.connected());
  }

  // add b
  ASSERT_EQ(s.reload(make_config(2, {{"a", A.c_str()}, {"b", B.c_str()}})), "");
  EXPECT_TRUE(echoes(pa));
  EXPECT_TRUE(echoes(pb));

  // remove a (kept connections on a would drain; new ones are refused)
  ASSERT_EQ(s.reload(make_config(2, {{"b", B.c_str()}})), "");
  EXPECT_TRUE(echoes(pb));
  {
    Client c(pa);
    EXPECT_FALSE(c.connected()) << "removed listener still accepting";
  }
  EXPECT_EQ(s.sum(&WorkerMetrics::stale_events), 0U);
}

TEST(Reload, BadConfigIsRejectedAndOldOneKeepsServing) {
  CORING5_REQUIRE_RING();
  const auto pa = pick_port();
  const auto A = "127.0.0.1:" + std::to_string(pa);
  Stack s;
  ASSERT_EQ(s.start(make_config(1, {{"a", A.c_str()}})), "");
  const auto gen = s.config->current()->generation;

  EXPECT_NE(s.reload("[runtime\nbroken"), ""); // syntax
  EXPECT_NE(
      s.reload(make_config(1, {{"x", "127.0.0.1:1"}, {"x", "127.0.0.1:2"}})),
      ""); // dup name
  EXPECT_NE(s.reload(make_config(1, {{"a", "not-an-address"}})),
            ""); // parse error
  EXPECT_NE(s.reload(make_config(3, {{"a", A.c_str()}})),
            ""); // runtime changed

  EXPECT_EQ(s.config->current()->generation, gen)
      << "failed reload must not publish";
  EXPECT_TRUE(echoes(pa));
}

TEST(Reload, UnbindableListenerFailsBeforeAnyWorkerIsTouched) {
  CORING5_REQUIRE_RING();
  const auto pa = pick_port();
  const auto A = "127.0.0.1:" + std::to_string(pa);
  Stack s;
  ASSERT_EQ(s.start(make_config(2, {{"a", A.c_str()}})), "");
  const auto gen = s.config->current()->generation;
  // 192.0.2.1 (TEST-NET-1) is not a local address: bind() fails on the control
  // thread.
  EXPECT_NE(
      s.reload(make_config(2, {{"a", A.c_str()}, {"b", "192.0.2.1:9999"}})),
      "");
  EXPECT_EQ(s.config->current()->generation, gen);
  EXPECT_TRUE(echoes(pa));
}

// The headline guarantee (docs/CONFIG_AND_RELOAD.md): reloading under load does
// not fail a single request on listeners that are kept.
TEST(Reload, UnderLoadDoesNotFailRequestsOnKeptListeners) {
  CORING5_REQUIRE_RING();
  const auto pa = pick_port();
  const auto pb = pick_port();
  const auto A = "127.0.0.1:" + std::to_string(pa);
  const auto B = "127.0.0.1:" + std::to_string(pb);

  Stack s;
  ASSERT_EQ(s.start(make_config(2, {{"a", A.c_str()}})), "");

  std::atomic<bool> stop{false};
  std::atomic<int> ok{0}, failed{0};
  std::vector<std::thread> load;
  for (int i = 0; i < 4; ++i) {
    load.emplace_back([&] {
      while (!stop.load()) {
        (echoes(pa) ? ok : failed)++;
      }
    });
  }
  for (int round = 0; round < 25; ++round) {
    const bool with_b = round % 2 == 0;
    const auto cfg = with_b
                         ? make_config(2, {{"a", A.c_str()}, {"b", B.c_str()}},
                                       60000 + round)
                         : make_config(2, {{"a", A.c_str()}}, 60000 + round);
    ASSERT_EQ(s.reload(cfg), "") << "round " << round;
  }
  stop = true;
  for (auto &t : load) {
    t.join();
  }
  EXPECT_GT(ok.load(), 100);
  EXPECT_EQ(failed.load(), 0) << "requests failed during reload";
  EXPECT_EQ(s.sum(&WorkerMetrics::stale_events), 0U);
}

TEST(Reload, PublishesRouteOnlyChangesAndRejectsDanglingBindings) {
  CORING5_REQUIRE_RING();
  const std::string listener = make_config(1, {}) + R"(
[[listener]]
name = "web"
address = "127.0.0.1:0"
protocol = "http1"
route = "site"
)";
  const std::string routes = R"(
[[route_set]]
name = "site"
[[route_set.vhost]]
default = true
location = [{path="/", action="respond", body="first"}]
)";
  Stack stack;
  ASSERT_EQ(stack.start(listener + routes), "");
  const auto old = stack.config->current();
  const auto id = old->listeners.front().id;
  const auto port = stack.port();
  ASSERT_NE(port, 0);
  Client client(port);
  ASSERT_TRUE(client.connected());
  const std::string request = "GET / HTTP/1.1\r\nHost: site\r\n\r\n";
  constexpr std::string_view first =
      "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nfirst";
  ASSERT_TRUE(client.send_all(request));
  ASSERT_EQ(client.recv_n(first.size()), first);
  auto changed = listener + routes;
  changed.replace(changed.find("first"), 5, "second");
  auto parsed = parse_config(changed);
  ASSERT_TRUE(parsed) << parsed.error();
  auto report = stack.config->apply(std::move(*parsed));
  ASSERT_TRUE(report) << report.error();
  EXPECT_TRUE(report->workers_acked);
  EXPECT_TRUE(report->diff.routes_changed);
  EXPECT_EQ(report->diff.kept, (std::vector<ListenerId>{id}));
  EXPECT_TRUE(report->diff.added.empty());
  EXPECT_TRUE(report->diff.removed.empty());
  EXPECT_EQ(stack.port(), port);
  constexpr std::string_view second =
      "HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nsecond";
  ASSERT_TRUE(client.send_all(request));
  EXPECT_EQ(client.recv_n(second.size()), second);
  const auto current = stack.config->current();
  EXPECT_EQ(current->generation, old->generation + 1);
  std::array<char, 8> scratch;
  const auto before = old->match(id, "", "/", scratch);
  const auto after = current->match(id, "", "/", scratch);
  ASSERT_NE(before.action, nullptr);
  ASSERT_NE(after.action, nullptr);
  EXPECT_EQ(std::get<RespondRoute>(*before.action).body(), "first");
  EXPECT_EQ(std::get<RespondRoute>(*after.action).body(), "second");

  EXPECT_NE(stack.reload(listener).find("unknown route set"), std::string::npos);
  EXPECT_EQ(stack.config->current(), current);
}
