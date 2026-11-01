#include "coring5/base/Ack.hpp"
#include "coring5/base/Endpoint.hpp"
#include "coring5/base/Result.hpp"
#include <gtest/gtest.h>
#include <thread>

using namespace Coring5;

TEST(Endpoint, ParsesV4V6AndWildcard) {
  auto a = Endpoint::parse("127.0.0.1:8080");
  ASSERT_TRUE(a);
  EXPECT_EQ(a->port(), 8080);
  EXPECT_EQ(a->to_string(), "127.0.0.1:8080");

  auto b = Endpoint::parse("[::1]:443");
  ASSERT_TRUE(b);
  EXPECT_EQ(b->family(), AF_INET6);
  EXPECT_EQ(b->to_string(), "[::1]:443");

  auto c = Endpoint::parse(":9000");
  ASSERT_TRUE(c);
  EXPECT_EQ(c->to_string(), "0.0.0.0:9000");
}

TEST(Endpoint, RejectsGarbage) {
  for (auto bad :
       {"", "1.2.3.4", "1.2.3.4:", "1.2.3.4:70000", "1.2.3.4:-1", "1.2.3.4:80x",
        "[::1]", "[::1:80", "[::1]80", "example.com:80", "300.1.1.1:80",
        "::1:80", "[1.2.3.4]:80", ":", "[]:80"}) {
    EXPECT_FALSE(Endpoint::parse(bad)) << "accepted: '" << bad << "'";
  }
}

TEST(Result, ErrcHasNames) {
  for (std::size_t i = 0; i < kErrcCount; ++i) {
    EXPECT_NE(to_string(static_cast<Errc>(i)), "unknown");
  }
  EXPECT_EQ(sizeof(Error), 8U);
}

TEST(ReloadAck, CompletesAcrossThreads) {
  ReloadAck ack(4);
  EXPECT_FALSE(ack.done());
  std::vector<std::thread> ts;
  for (int i = 0; i < 4; ++i) {
    ts.emplace_back([&, i] { ack.complete(i != 2); });
  }
  EXPECT_TRUE(ack.wait_for(std::chrono::seconds(5)));
  for (auto &t : ts) {
    t.join();
  }
  EXPECT_EQ(ack.failures(), 1U);
}

TEST(ReloadAck, TimesOut) {
  ReloadAck ack(1);
  EXPECT_FALSE(ack.wait_for(std::chrono::milliseconds(20)));
}
