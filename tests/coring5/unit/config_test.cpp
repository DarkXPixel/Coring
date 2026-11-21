#include "coring5/config/Diff.hpp"
#include "coring5/config/Loader.hpp"
#include <gtest/gtest.h>

using namespace Coring5;

namespace {
constexpr const char *kGood = R"(
[runtime]
workers = 2
buffer_count = 256
buffer_size = 4096

[limits]
idle_timeout_ms = 5000

[[listener]]
name = "echo"
address = "127.0.0.1:0"
protocol = "echo"

[[listener]]
name = "echo2"
address = "127.0.0.1:18081"
)";
} // namespace

TEST(Config, ParsesAndValidates) {
  auto p = parse_config(kGood);
  ASSERT_TRUE(p) << p.error();
  EXPECT_EQ(p->runtime.workers, 2U);
  EXPECT_EQ(p->limits.idle_timeout, Millis{5000});
  ASSERT_EQ(p->listeners.size(), 2U);
  EXPECT_EQ(p->listeners[1].endpoint.port(), 18081);
  EXPECT_TRUE(validate_config(*p));
}

TEST(Config, EmptyDocumentIsValidWithDefaults) {
  auto p = parse_config("");
  ASSERT_TRUE(p);
  EXPECT_TRUE(validate_config(*p));
  EXPECT_TRUE(p->listeners.empty());
}

TEST(Config, SyntaxAndTypeErrorsAreReported) {
  EXPECT_FALSE(parse_config("[runtime\nworkers = 1"));
  EXPECT_FALSE(parse_config("[runtime]\nworkers = \"two\""));
  EXPECT_FALSE(parse_config("[runtime]\nworkers = -1"));
  EXPECT_FALSE(parse_config("runtime = 3"));
  EXPECT_FALSE(
      parse_config("[[listener]]\naddress = \"1.2.3.4:80\"")); // no name
  EXPECT_FALSE(parse_config(
      "[[listener]]\nname = \"a\"\naddress = \"bad\"")); // bad addr
  EXPECT_FALSE(parse_config(
      "[[listener]]\nname = \"a\"\naddress = \":80\"\nprotocol = \"gopher\""));
}

TEST(Config, SemanticValidation) {
  auto bad = [](const char *text) {
    auto p = parse_config(text);
    return p && !validate_config(*p);
  };
  EXPECT_TRUE(bad("[runtime]\nworkers = 0"));
  EXPECT_TRUE(bad("[runtime]\nbuffer_count = 100")); // not a power of two
  EXPECT_TRUE(bad("[runtime]\nbuffer_size = 10"));
  EXPECT_TRUE(bad("[runtime]\nworkers = 4\ncpus = [0,1]"));
  EXPECT_TRUE(
      bad("[limits]\nqueue_low_watermark = 100\nqueue_high_watermark = 100"));
  EXPECT_TRUE(bad("[[listener]]\nname=\"a\"\naddress=\":80\"\n[[listener]]"
                  "\nname=\"a\"\naddress=\":81\""));
  EXPECT_TRUE(bad("[[listener]]\nname=\"a\"\naddress=\":80\"\n[[listener]]"
                  "\nname=\"b\"\naddress=\":80\""));
  // http2 etc. are accepted by the parser but rejected until implemented:
  EXPECT_TRUE(
      bad("[[listener]]\nname=\"a\"\naddress=\":80\"\nprotocol=\"http2\""));
}

TEST(Config, MissingFile) {
  EXPECT_FALSE(load_config_file("/nonexistent/coring.toml"));
}

// ---- M4.1 S0: session limits ------------------------------------------------

TEST(Config, SessionLimitDefaults) {
  auto p = parse_config("");
  ASSERT_TRUE(p) << p.error();
  EXPECT_EQ(p->limits.max_requests, 1000U);
  EXPECT_EQ(p->limits.max_request_body_bytes, 0U);
}

TEST(Config, SessionLimitsParse) {
  auto p = parse_config("[limits]\nmax_requests = 0\n"
                        "max_request_body_bytes = 1048576\n");
  ASSERT_TRUE(p) << p.error();
  EXPECT_EQ(p->limits.max_requests, 0U); // 0 = unlimited
  EXPECT_EQ(p->limits.max_request_body_bytes, 1048576U);
  EXPECT_TRUE(validate_config(*p));

  // 0 = unlimited is a valid, meaningful value for both keys.
  auto z = parse_config("[limits]\nmax_requests = 0\n"
                        "max_request_body_bytes = 0\n");
  ASSERT_TRUE(z) << z.error();
  EXPECT_EQ(z->limits.max_requests, 0U);
  EXPECT_EQ(z->limits.max_request_body_bytes, 0U);
  EXPECT_TRUE(validate_config(*z));
}

TEST(Config, SessionLimitsRejectBadTypes) {
  EXPECT_FALSE(parse_config("[limits]\nmax_requests = -1"));
  EXPECT_FALSE(parse_config("[limits]\nmax_requests = \"many\""));
  EXPECT_FALSE(parse_config("[limits]\nmax_request_body_bytes = -1"));
  // max_request_body_bytes is 64-bit; a value beyond uint32 still parses.
  auto big = parse_config("[limits]\nmax_request_body_bytes = 4294967296");
  ASSERT_TRUE(big) << big.error();
  EXPECT_EQ(big->limits.max_request_body_bytes, 4294967296ULL);
}

TEST(Config, QueueHighWatermarkMustCoverBufferSize) {
  auto bad = [](const char *text) {
    auto p = parse_config(text);
    return p && !validate_config(*p);
  };
  // high watermark below buffer_size is rejected (the session stage chunk
  // needs at least one full buffer).
  EXPECT_TRUE(bad("[runtime]\nbuffer_size = 4096\n[limits]\n"
                  "queue_high_watermark = 2048\nqueue_low_watermark = 1024"));
  // equal to buffer_size is the boundary and is accepted.
  auto ok = parse_config("[runtime]\nbuffer_size = 4096\n[limits]\n"
                         "queue_high_watermark = 4096\n"
                         "queue_low_watermark = 1024");
  ASSERT_TRUE(ok) << ok.error();
  EXPECT_TRUE(validate_config(*ok));
}

TEST(Config, StageChunkSizeCap) {
  auto bad = [](const char *text) {
    auto p = parse_config(text);
    return p && !validate_config(*p);
  };
  // max_head_bytes + buffer_size must be <= 8 MiB (the stage chunk size).
  EXPECT_TRUE(bad("[runtime]\nbuffer_size = 1048576\n[limits]\n"
                  "max_head_bytes = 8388608\n"
                  "queue_high_watermark = 1048576\n"
                  "queue_low_watermark = 1024"));
  // exactly 8 MiB is the boundary and is accepted.
  auto ok = parse_config("[runtime]\nbuffer_size = 1048576\n[limits]\n"
                         "max_head_bytes = 7340032\n"
                         "queue_high_watermark = 1048576\n"
                         "queue_low_watermark = 1024");
  ASSERT_TRUE(ok) << ok.error();
  EXPECT_TRUE(validate_config(*ok));
}

TEST(Diff, SessionLimitsChangeIsDetected) {
  auto mk = [](uint32_t max_requests, uint64_t max_body) {
    ConfigSnapshot s;
    s.limits.max_requests = max_requests;
    s.limits.max_request_body_bytes = max_body;
    return s;
  };
  EXPECT_TRUE(diff_snapshots(mk(1000, 0), mk(1000, 0)).empty());
  EXPECT_TRUE(diff_snapshots(mk(1000, 0), mk(0, 0)).limits_changed);
  EXPECT_TRUE(diff_snapshots(mk(1000, 0), mk(1000, 4096)).limits_changed);
}

TEST(Diff, AddedRemovedKeptAndLimits) {
  auto mk = [](std::initializer_list<uint32_t> ids, uint32_t idle_ms) {
    ConfigSnapshot s;
    s.limits.idle_timeout = Millis{idle_ms};
    for (auto id : ids) {
      ListenerSpec l;
      l.id = ListenerId{id};
      s.listeners.push_back(l);
    }
    return s;
  };
  const auto d = diff_snapshots(mk({1, 2, 3}, 1000), mk({2, 3, 4}, 2000));
  EXPECT_EQ(d.added, (std::vector<ListenerId>{ListenerId{4}}));
  EXPECT_EQ(d.removed, (std::vector<ListenerId>{ListenerId{1}}));
  EXPECT_EQ(d.kept.size(), 2U);
  EXPECT_TRUE(d.limits_changed);
  EXPECT_FALSE(d.empty());

  EXPECT_TRUE(diff_snapshots(mk({1}, 1000), mk({1}, 1000)).empty());
}
