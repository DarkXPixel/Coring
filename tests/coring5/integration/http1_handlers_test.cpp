#include "support/TestServer.hpp"
#include "coring5/net/ListenSocket.hpp"
#include "coring5/runtime/Daemon.hpp"
#include <filesystem>
#include <fstream>

using namespace Coring5;
using namespace Coring5::testing;

namespace {
std::string site(std::string action, std::string upstream = {}) {
  return make_config(1, {}) + upstream + R"(
[[listener]]
name = "web"
address = "127.0.0.1:0"
protocol = "http1"
route = "site"
[[route_set]]
name = "site"
[[route_set.vhost]]
default = true
location = [{path="/asset", )" +
         action + "}]\n";
}

std::string fetch(uint16_t port, std::string method, std::string fields = {}) {
  Client c(port);
  EXPECT_TRUE(c.connected());
  EXPECT_TRUE(c.send_all(
      method + " /asset HTTP/1.1\r\nHost: site\r\nConnection: close\r\n" +
      fields + "\r\n"));
  auto response = c.recv_n(1U << 20);
  EXPECT_TRUE(c.wait_eof()) << c.last_recv();
  return response;
}

class Http1Handlers : public ::testing::Test {
protected:
  void SetUp() override {
    char name[] = "/tmp/coring5-adapters-XXXXXX";
    ASSERT_NE(::mkdtemp(name), nullptr);
    dir = name;
  }
  void TearDown() override {
    if (!dir.empty()) {
      std::filesystem::remove_all(dir);
    }
  }
  std::filesystem::path dir;
};

TEST_F(Http1Handlers, ProxyStreamsFromStaticBackendOverRealRings) {
  CORING5_REQUIRE_RING();
  // Cross several downstream high-watermark cycles.
  std::string payload(600000, 'a');
  for (size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<char>('a' + (i % 26));
  }
  std::ofstream(dir / "asset", std::ios::binary) << payload;
  const auto files =
      site("action=\"static_file\", root=\"" + dir.string() + "\"");
  Stack backend;
  ASSERT_EQ(backend.start(files), "");
  Stack front;
  auto upstream = "[[upstream]]\nname=\"origin\"\nbackends=[\"127.0.0.1:" +
                  std::to_string(backend.port()) + "\"]\n";
  ASSERT_EQ(front.start(site("action=\"reverse_proxy\", upstream=\"origin\"",
                             upstream)),
            "");
  auto wire = fetch(front.port(), "GET");
  ASSERT_TRUE(wire.starts_with("HTTP/1.1 200 OK\r\n"));
  ASSERT_NE(wire.find("\r\n\r\n"), wire.npos);
  EXPECT_EQ(wire.substr(wire.find("\r\n\r\n") + 4), payload);
  auto pos = wire.find("ETag: ");
  ASSERT_NE(pos, wire.npos);
  auto etag = wire.substr(pos + 6, wire.find("\r\n", pos) - pos - 6);
  auto head = fetch(front.port(), "HEAD");
  EXPECT_TRUE(head.contains("Content-Length: 600000\r\n"));
  EXPECT_TRUE(head.ends_with("\r\n\r\n"));
  EXPECT_EQ(front.sum(&WorkerMetrics::upstream_connect_ok), 1U);
  auto range = fetch(front.port(), "GET", "Range: bytes=31-5040\r\n");
  EXPECT_TRUE(range.starts_with("HTTP/1.1 206"));
  EXPECT_TRUE(range.contains("Content-Range: bytes 31-5040/600000\r\n"));
  EXPECT_TRUE(range.ends_with("\r\n\r\n" + payload.substr(31, 5010)));
  auto unchanged =
      fetch(front.port(), "GET", "If-None-Match: " + etag + "\r\n");
  EXPECT_TRUE(unchanged.starts_with("HTTP/1.1 304"));
  EXPECT_TRUE(unchanged.ends_with("\r\n\r\n"));
  pos = wire.find("Last-Modified: ");
  ASSERT_NE(pos, wire.npos);
  const auto modified = wire.substr(pos + 15, wire.find("\r\n", pos) - pos - 15);
  EXPECT_TRUE(fetch(front.port(), "GET", "If-Modified-Since: " + modified + "\r\n")
                  .starts_with("HTTP/1.1 304"));
  EXPECT_FALSE(backend
                   .reload(site("action=\"static_file\", root=\"" +
                                (dir / "missing").string() + "\""))
                   .empty());
  EXPECT_TRUE(fetch(front.port(), "GET", "Range: bytes=-4\r\n")
                  .ends_with(payload.substr(payload.size() - 4)));
  EXPECT_EQ(front.sum(&WorkerMetrics::stale_events), 0U);
  EXPECT_EQ(backend.sum(&WorkerMetrics::stale_events), 0U);
}
TEST_F(Http1Handlers, LogsPipelinedRequestsAndRejectsSinkChangesOnReload) {
  CORING5_REQUIRE_RING();
  auto listener = create_tcp_listener(*Endpoint::parse("127.0.0.1:0"),
                                      ListenOptions{.reuse_port = false});
  ASSERT_TRUE(listener);
  const auto port = local_endpoint(listener->get())->port();
  listener->reset();
  auto config = site("action=\"respond\", body=\"ok\"");
  config.replace(config.find(":0"), 2, ":" + std::to_string(port));
  const auto path = (dir / "config.toml").string();
  const auto sink = (dir / "access.log").string();
  const auto log_config = [&](const std::string &name) {
    return config + "\n[access_log]\npath=\"" + name + "\"\n";
  };
  std::ofstream(path) << log_config(sink);
  Daemon daemon({.config_path = path, .control_socket = (dir / "ctl").string()});
  const auto started = daemon.start();
  ASSERT_TRUE(started) << started.error();
  std::ofstream(path) << log_config(sink + ".other");
  EXPECT_TRUE(daemon.reload().contains("[access_log] section changed"));
  Client c(port);
  ASSERT_TRUE(c.connected());
  ASSERT_TRUE(c.send_all("GET /asset HTTP/1.1\r\nHost: site\r\n\r\n"
                         "HEAD /asset?next HTTP/1.1\r\nHost: site\r\n"
                         "Connection: close\r\n\r\n"));
  EXPECT_TRUE(c.recv_n(4096).ends_with("\r\n\r\n"));
  EXPECT_TRUE(c.wait_eof());
  ASSERT_TRUE(daemon.stop());
  EXPECT_FALSE(std::filesystem::exists(sink + ".other"));
  std::ifstream log(sink);
  std::string row;
  ASSERT_TRUE(std::getline(log, row));
  EXPECT_TRUE(row.ends_with("\t200\tGET\t/asset"));
  ASSERT_TRUE(std::getline(log, row));
  EXPECT_TRUE(row.ends_with("\t200\tHEAD\t/asset?next"));
  EXPECT_FALSE(std::getline(log, row));
}
} // namespace
