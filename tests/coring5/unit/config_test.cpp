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
  // http1 etc. are accepted by the parser but rejected until implemented:
  EXPECT_TRUE(
      bad("[[listener]]\nname=\"a\"\naddress=\":80\"\nprotocol=\"http1\""));
}

TEST(Config, MissingFile) {
  EXPECT_FALSE(load_config_file("/nonexistent/coring.toml"));
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
