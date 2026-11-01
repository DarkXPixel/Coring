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
