#include "coring5/obs/Metrics.hpp"
#include "coring5/obs/SpscRing.hpp"
#include <gtest/gtest.h>
#include <thread>

using namespace Coring5;

TEST(Histogram, QuantilesAreBucketUpperBounds) {
  Log2Histogram h;
  EXPECT_EQ(h.quantile_upper_bound(0.5), 0U);
  for (int i = 0; i < 99; ++i) {
    h.record(1000); // bucket [512,1024)
  }
  h.record(10'000'000); // one outlier ~10ms
  EXPECT_EQ(h.count(), 100U);
  EXPECT_EQ(h.quantile_upper_bound(0.5), 1024U);
  EXPECT_GE(h.quantile_upper_bound(0.999), 10'000'000U);
}

TEST(Histogram, HugeValuesClampToLastBucket) {
  Log2Histogram h;
  h.record(UINT64_MAX);
  EXPECT_EQ(h.count(), 1U);
}

TEST(Metrics, RenderAggregatesShards) {
  std::vector<WorkerMetrics> shards(3);
  for (auto &s : shards) {
    inc(s.accepts, 5);
    inc(s.closes[static_cast<std::size_t>(Errc::Timeout)], 2);
  }
  const auto text = render_metrics(shards);
  EXPECT_NE(text.find("coring_accepts_total 15"), std::string::npos) << text;
  EXPECT_NE(text.find("reason=\"timeout\"} 6"), std::string::npos) << text;
  EXPECT_NE(text.find("coring_stale_events_total 0"), std::string::npos)
      << text;
}

TEST(SpscRing, FullRingDropsInsteadOfBlocking) {
  SpscRing<int, 4> r;
  for (int i = 0; i < 4; ++i) {
    EXPECT_TRUE(r.push(i));
  }
  EXPECT_FALSE(r.push(99));
  EXPECT_EQ(*r.pop(), 0);
  EXPECT_TRUE(r.push(99));
}

TEST(SpscRing, ProducerConsumerPreservesOrder) {
  SpscRing<uint32_t, 1024> r;
  constexpr uint32_t N = 200000;
  std::thread prod([&] {
    for (uint32_t i = 0; i < N;) {
      if (r.push(i)) {
        ++i;
      }
    }
  });
  uint32_t expected = 0;
  while (expected < N) {
    if (auto v = r.pop()) {
      ASSERT_EQ(*v, expected);
      ++expected;
    }
  }
  prod.join();
}
