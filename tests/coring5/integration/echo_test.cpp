#include "support/TestServer.hpp"
#include <chrono>
#include <thread>
#include <vector>

using namespace Coring5;
using namespace Coring5::testing;
using namespace std::chrono_literals;

namespace {

template <class Pred>
bool wait_until(Pred p, std::chrono::milliseconds limit = 5s) {
  const auto end = std::chrono::steady_clock::now() + limit;
  while (!p()) {
    if (std::chrono::steady_clock::now() > end) {
      return false;
    }
    std::this_thread::sleep_for(5ms);
  }
  return true;
}

} // namespace

TEST(Echo, RoundTrip) {
  CORING5_REQUIRE_RING();
  Stack s;
  ASSERT_EQ(s.start(make_config(2, {{"e", "127.0.0.1:0"}})), "");
  Client c(s.port());
  ASSERT_TRUE(c.connected());
  ASSERT_TRUE(c.send_all("hello coring"));
  EXPECT_EQ(c.recv_n(12), "hello coring");
}

TEST(Echo, LargePayloadSurvivesShortWritesAndMultipleBuffers) {
  CORING5_REQUIRE_RING();
  Stack s;
  ASSERT_EQ(s.start(make_config(1, {{"e", "127.0.0.1:0"}})), "");
  std::string payload(1 << 20, '\0');
  for (std::size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<char>(i * 31 + 7);
  }
  Client c(s.port());
  ASSERT_TRUE(c.connected());
  std::thread writer([&] { EXPECT_TRUE(c.send_all(payload)); });
  const auto got = c.recv_n(payload.size());
  writer.join();
  EXPECT_EQ(got.size(), payload.size()) << c.last_recv();
  EXPECT_TRUE(got == payload);
}

TEST(Echo, ManyConcurrentClientsAcrossWorkers) {
  CORING5_REQUIRE_RING();
  Stack s;
  ASSERT_EQ(s.start(make_config(4, {{"e", "127.0.0.1:0"}})), "");
  constexpr int kClients = 64;
  std::vector<std::thread> ts;
  std::atomic<int> ok{0};
  for (int i = 0; i < kClients; ++i) {
    ts.emplace_back([&, i] {
      Client c(s.port());
      const std::string msg = "client-" + std::to_string(i);
      if (c.connected() && c.send_all(msg) && c.recv_n(msg.size()) == msg) {
        ++ok;
      }
    });
  }
  for (auto &t : ts) {
    t.join();
  }
  EXPECT_EQ(ok.load(), kClients);
  EXPECT_EQ(s.sum(&WorkerMetrics::stale_events), 0U) << "invariant I4 violated";
}

TEST(Echo, ClientCloseReleasesTheConnectionSlot) {
  CORING5_REQUIRE_RING();
  Stack s;
  ASSERT_EQ(s.start(make_config(1, {{"e", "127.0.0.1:0"}})), "");
  for (int i = 0; i < 200;
       ++i) { // more than max_connections_per_worker / 2 over time
    Client c(s.port());
    ASSERT_TRUE(c.connected());
    ASSERT_TRUE(c.send_all("x"));
    ASSERT_EQ(c.recv_n(1), "x");
    if (i % 2 == 0) {
      c.reset(); // RST instead of FIN
    }
  }
  EXPECT_TRUE(wait_until([&] {
    return s.sum(&WorkerMetrics::conns_live) == 0;
  })) << "connection slots leaked";
  EXPECT_EQ(s.sum(&WorkerMetrics::stale_events), 0U);
}

TEST(Echo, PoolExhaustionShedsLoadWithoutCrashing) {
  CORING5_REQUIRE_RING();
  Stack s;
  auto cfg = make_config(1, {{"e", "127.0.0.1:0"}});
  const auto pos = cfg.find("max_connections_per_worker = 512");
  cfg.replace(pos, std::string("max_connections_per_worker = 512").size(),
              "max_connections_per_worker = 8");
  ASSERT_EQ(s.start(cfg), "");
  std::vector<std::unique_ptr<Client>> held;
  for (int i = 0; i < 32; ++i) {
    held.push_back(std::make_unique<Client>(s.port()));
  }
  EXPECT_TRUE(
      wait_until([&] { return s.sum(&WorkerMetrics::conns_rejected) >= 24; }));
  // Existing connections still work; new ones work again once slots free up.
  held.resize(0);
  EXPECT_TRUE(
      wait_until([&] { return s.sum(&WorkerMetrics::conns_live) == 0; }));
  Client c(s.port());
  ASSERT_TRUE(c.send_all("ok"));
  EXPECT_EQ(c.recv_n(2), "ok");
}

TEST(Echo, IdleTimeoutClosesTheConnection) {
  CORING5_REQUIRE_RING();
  Stack s;
  ASSERT_EQ(s.start(make_config(1, {{"e", "127.0.0.1:0"}}, /*idle_ms=*/100)),
            "");
  Client c(s.port());
  ASSERT_TRUE(c.connected());
  EXPECT_TRUE(c.wait_eof()) << "idle connection not closed by the timer wheel: " << c.last_recv();
}

TEST(Echo, GracefulShutdownDrainsAndClosesEverything) {
  CORING5_REQUIRE_RING();
  Stack s;
  ASSERT_EQ(s.start(make_config(2, {{"e", "127.0.0.1:0"}})), "");
  const auto port = s.port();
  std::vector<std::unique_ptr<Client>> cs;
  for (int i = 0; i < 16; ++i) {
    cs.push_back(std::make_unique<Client>(port));
    ASSERT_TRUE(cs.back()->send_all("a"));
    ASSERT_EQ(cs.back()->recv_n(1), "a");
  }
  const auto t0 = std::chrono::steady_clock::now();
  s.workers.shutdown(
      2000ms); // joins: must return, idle connections close at once
  EXPECT_LT(std::chrono::steady_clock::now() - t0, 1500ms);
  for (auto &c : cs) {
    EXPECT_TRUE(c->wait_eof());
  }
  Client late(port);
  EXPECT_FALSE(late.connected()) << "listener still open after shutdown";
}
