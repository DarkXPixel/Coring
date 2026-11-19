#include "coring5/net/ListenSocket.hpp"
#include "support/TestServer.hpp"
#include <atomic>
#include <chrono>
#include <fstream>
#include <netinet/tcp.h>
#include <poll.h>
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

// A plain blocking TCP backend running on a test thread. Every accepted socket
// is handed to `handler(fd)`; the handler owns and closes it.
class Backend {
public:
  template <class H> explicit Backend(H handler) {
    auto ep = Endpoint::parse("127.0.0.1:0");
    auto fd = create_tcp_listener(*ep, ListenOptions{.reuse_port = false});
    EXPECT_TRUE(fd);
    listen_ = std::move(*fd);
    port_ = local_endpoint(listen_.get())->port();
    thread_ = std::thread([this, handler] {
      while (!stop_) {
        pollfd p{listen_.get(), POLLIN, 0};
        if (::poll(&p, 1, 20) <= 0) {
          continue;
        }
        const int c = ::accept4(listen_.get(), nullptr, nullptr, SOCK_CLOEXEC);
        if (c < 0) {
          continue;
        }
        ++accepted_;
        std::lock_guard lk(mtx_);
        handlers_.emplace_back([handler, c] { handler(c); });
      }
    });
  }
  ~Backend() {
    stop_ = true;
    thread_.join();
    for (auto &t : handlers_) {
      t.join();
    }
  }
  [[nodiscard]] uint16_t port() const { return port_; }
  [[nodiscard]] int accepted() const { return accepted_; }

private:
  FdOwner listen_;
  uint16_t port_{0};
  std::atomic<bool> stop_{false};
  std::atomic<int> accepted_{0};
  std::thread thread_;
  std::mutex mtx_;
  std::vector<std::thread> handlers_;
};

// Reads until EOF, echoing everything back; then half-closes and appends
// "<bye>" to prove the reverse direction still works after client EOF.
void echo_then_bye(int fd) {
  char buf[16384];
  for (;;) {
    const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) {
      break;
    }
    std::size_t off = 0;
    while (off < static_cast<std::size_t>(n)) {
      const ssize_t w = ::send(fd, buf + off, static_cast<std::size_t>(n) - off,
                               MSG_NOSIGNAL);
      if (w <= 0) {
        ::close(fd);
        return;
      }
      off += static_cast<std::size_t>(w);
    }
  }
  (void)::send(fd, "<bye>", 5, MSG_NOSIGNAL);
  ::close(fd);
}

void plain_echo(int fd) {
  char buf[16384];
  for (;;) {
    const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) {
      break;
    }
    if (::send(fd, buf, static_cast<std::size_t>(n), MSG_NOSIGNAL) != n) {
      break;
    }
  }
  ::close(fd);
}

uint16_t dead_port() { // bound-then-closed: connecting is refused
  auto ep = Endpoint::parse("127.0.0.1:0");
  auto fd = create_tcp_listener(*ep, ListenOptions{.reuse_port = false});
  return local_endpoint(fd->get())->port();
}

std::string l4_config(unsigned workers, const std::string &backends,
                      const std::string &extra_limits = "",
                      const std::string &upstream_extra = "",
                      unsigned max_conn = 512) {
  return "[runtime]\nworkers = " + std::to_string(workers) +
         "\nbuffer_count = 256\nbuffer_size = 4096\n"
         "max_connections_per_worker = " +
         std::to_string(max_conn) +
         "\ntick_ms = 10\ndrain_timeout_ms = 2000\nrelay_memory_mib = 16\n"
         "[limits]\nidle_timeout_ms = 60000\nconnect_timeout_ms = 1000\n" +
         extra_limits + "[[upstream]]\nname = \"be\"\nbackends = [" + backends +
         "]\n" + upstream_extra +
         "[[listener]]\nname = \"front\"\naddress = \"127.0.0.1:0\"\n"
         "protocol = \"l4\"\nupstream = \"be\"\n";
}

std::string addr(uint16_t port) {
  return "\"127.0.0.1:" + std::to_string(port) + "\"";
}

} // namespace

TEST(L4, RelaysBothDirectionsAndPreservesData) {
  CORING5_REQUIRE_RING();
  Backend be(plain_echo);
  Stack s;
  ASSERT_EQ(s.start(l4_config(2, addr(be.port()))), "");
  Client c(s.port());
  ASSERT_TRUE(c.connected());
  ASSERT_TRUE(c.send_all("hello through the proxy"));
  EXPECT_EQ(c.recv_n(23), "hello through the proxy");
  EXPECT_EQ(s.sum(&WorkerMetrics::stale_events), 0U);
}

TEST(L4, LargePayloadSurvivesShortWritesAndBackpressure) {
  CORING5_REQUIRE_RING();
  Backend be(plain_echo);
  Stack s;
  // Small watermarks force pause/resume cycles during the transfer.
  ASSERT_EQ(s.start(l4_config(1, addr(be.port()),
                              "queue_high_watermark = 16384\n"
                              "queue_low_watermark = 4096\n")),
            "");
  std::string payload(8 << 20, '\0');
  for (std::size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<char>(i * 131 + 17);
  }
  Client c(s.port());
  ASSERT_TRUE(c.connected());
  std::thread writer([&] { EXPECT_TRUE(c.send_all(payload)); });
  const auto got = c.recv_n(payload.size());
  writer.join();
  ASSERT_EQ(got.size(), payload.size()) << c.last_recv();
  EXPECT_TRUE(got == payload);
  EXPECT_EQ(s.sum(&WorkerMetrics::stale_events), 0U);
}

TEST(L4, HalfCloseFromClientStillDeliversTheReply) {
  CORING5_REQUIRE_RING();
  Backend be(echo_then_bye);
  Stack s;
  ASSERT_EQ(s.start(l4_config(1, addr(be.port()))), "");
  Client c(s.port());
  ASSERT_TRUE(c.connected());
  ASSERT_TRUE(c.send_all("ping"));
  c.shutdown_write(); // FIN toward the proxy; the reply path must stay open
  EXPECT_EQ(c.recv_n(9), "ping<bye>");
  EXPECT_TRUE(c.wait_eof());
  EXPECT_TRUE(
      wait_until([&] { return s.sum(&WorkerMetrics::conns_live) == 0; }));
  EXPECT_EQ(s.sum(&WorkerMetrics::stale_events), 0U);
}

TEST(L4, UpstreamCloseIsPropagatedToTheClient) {
  CORING5_REQUIRE_RING();
  Backend be([](int fd) {
    (void)::send(fd, "greeting", 8, MSG_NOSIGNAL);
    ::close(fd);
  });
  Stack s;
  ASSERT_EQ(s.start(l4_config(1, addr(be.port()))), "");
  Client c(s.port());
  ASSERT_TRUE(c.connected());
  EXPECT_EQ(c.recv_n(8), "greeting");
  EXPECT_TRUE(c.wait_eof());
  c.shutdown_write();
  EXPECT_TRUE(
      wait_until([&] { return s.sum(&WorkerMetrics::conns_live) == 0; }));
}

TEST(L4, RefusedBackendClosesTheClientAndEjectsIt) {
  CORING5_REQUIRE_RING();
  Stack s;
  ASSERT_EQ(s.start(l4_config(1, addr(dead_port()), "",
                              "max_fails = 2\neject_time_ms = 60000\n")),
            "");
  for (int i = 0; i < 4; ++i) {
    Client c(s.port());
    ASSERT_TRUE(c.connected());
    EXPECT_TRUE(c.wait_eof()) << c.last_recv();
  }
  EXPECT_GE(s.sum(&WorkerMetrics::upstream_connect_fail), 2U);
  EXPECT_GE(s.sum(&WorkerMetrics::upstream_ejections), 1U);
  // The only backend is ejected: panic mode still tries it.
  EXPECT_GE(s.sum(&WorkerMetrics::upstream_panic_picks), 1U);
  EXPECT_TRUE(
      wait_until([&] { return s.sum(&WorkerMetrics::conns_live) == 0; }));
  EXPECT_EQ(s.sum(&WorkerMetrics::stale_events), 0U);
}

TEST(L4, ConnectFailureRetriesOnAnotherBackendBeforeAnyByteIsSent) {
  CORING5_REQUIRE_RING();
  Backend good(plain_echo);
  Stack s;
  ASSERT_EQ(s.start(l4_config(1, addr(dead_port()) + ", " + addr(good.port()))),
            "");
  // Round robin starts at the dead backend for half of the connections; every
  // client must still be served.
  for (int i = 0; i < 12; ++i) {
    Client c(s.port());
    ASSERT_TRUE(c.connected());
    ASSERT_TRUE(c.send_all("x"));
    ASSERT_EQ(c.recv_n(1), "x") << "client " << i << " " << c.last_recv();
  }
  EXPECT_GE(s.sum(&WorkerMetrics::upstream_retries), 1U);
  EXPECT_EQ(s.sum(&WorkerMetrics::stale_events), 0U);
}

TEST(L4, HangingBackendIsBoundedByTheConnectTimeout) {
  CORING5_REQUIRE_RING();
  // 192.0.2.1 (TEST-NET-1) black-holes SYNs where a route exists; without a
  // route connect fails at once. Either way the client must see EOF promptly.
  Stack s;
  ASSERT_EQ(s.start(l4_config(1, "\"192.0.2.1:80\"", "",
                              "connect_timeout_ms = 300\n")),
            "");
  Client c(s.port());
  ASSERT_TRUE(c.connected());
  const auto t0 = std::chrono::steady_clock::now();
  EXPECT_TRUE(c.wait_eof()) << c.last_recv();
  EXPECT_LT(std::chrono::steady_clock::now() - t0, 2s);
  EXPECT_TRUE(
      wait_until([&] { return s.sum(&WorkerMetrics::conns_live) == 0; }));
}

TEST(L4, ClientResetTearsDownBothSides) {
  CORING5_REQUIRE_RING();
  std::atomic<int> backend_eof{0};
  Backend be([&](int fd) {
    char b[64];
    while (::recv(fd, b, sizeof(b), 0) > 0) {
    }
    ++backend_eof;
    ::close(fd);
  });
  Stack s;
  ASSERT_EQ(s.start(l4_config(1, addr(be.port()))), "");
  for (int i = 0; i < 20; ++i) {
    Client c(s.port());
    ASSERT_TRUE(c.connected());
    ASSERT_TRUE(c.send_all("data"));
    c.reset();
  }
  EXPECT_TRUE(wait_until([&] { return backend_eof.load() == 20; }));
  EXPECT_TRUE(
      wait_until([&] { return s.sum(&WorkerMetrics::conns_live) == 0; }));
  EXPECT_EQ(s.sum(&WorkerMetrics::stale_events), 0U);
}

TEST(L4, ConnectionChurnLeaksNoSlotsOrChunks) {
  CORING5_REQUIRE_RING();
  Backend be(plain_echo);
  Stack s;
  ASSERT_EQ(s.start(l4_config(2, addr(be.port()), "", "", 64)), "");
  std::vector<std::thread> ts;
  std::atomic<int> ok{0};
  for (int t = 0; t < 4; ++t) {
    ts.emplace_back([&] {
      for (int i = 0; i < 100; ++i) {
        Client c(s.port());
        if (c.connected() && c.send_all("abc") && c.recv_n(3) == "abc") {
          ++ok;
        }
        if (i % 3 == 0) {
          c.reset();
        }
      }
    });
  }
  for (auto &t : ts) {
    t.join();
  }
  EXPECT_GE(ok.load(), 350) << "churn failures";
  EXPECT_TRUE(
      wait_until([&] { return s.sum(&WorkerMetrics::conns_live) == 0; }));
  EXPECT_EQ(s.sum(&WorkerMetrics::relay_chunks_in_use), 0U);
  EXPECT_EQ(s.sum(&WorkerMetrics::stale_events), 0U);
}

TEST(L4, SlowReaderKeepsRelayMemoryBounded) {
  CORING5_REQUIRE_RING();
  // The backend streams endlessly; the client never reads. Queued bytes must
  // stop at the watermark (plus kernel socket buffers), not grow with time.
  std::atomic<bool> stop{false};
  Backend be([&](int fd) {
    std::string block(64 * 1024, 'x');
    while (!stop) {
      pollfd p{fd, POLLOUT, 0};
      if (::poll(&p, 1, 50) > 0 &&
          ::send(fd, block.data(), block.size(), MSG_NOSIGNAL | MSG_DONTWAIT) <
              0 &&
          errno != EAGAIN) {
        break;
      }
    }
    ::close(fd);
  });
  Stack s;
  constexpr unsigned kHigh = 64 * 1024;
  ASSERT_EQ(s.start(l4_config(1, addr(be.port()),
                              "queue_high_watermark = 65536\n"
                              "queue_low_watermark = 16384\n")),
            "");
  Client c(s.port());
  ASSERT_TRUE(c.connected());
  EXPECT_TRUE(
      wait_until([&] { return s.sum(&WorkerMetrics::relay_pauses) >= 1; }));
  std::this_thread::sleep_for(500ms);
  // high watermark + one recv worth of data + partial chunks, with margin.
  const uint64_t bound_chunks = (kHigh + 4 * 4096) / 4096 + 4;
  EXPECT_LE(s.sum(&WorkerMetrics::relay_chunks_peak), bound_chunks)
      << "relay queue grew past its watermark";
  stop = true;
  c.reset();
  EXPECT_TRUE(
      wait_until([&] { return s.sum(&WorkerMetrics::conns_live) == 0; }));
  EXPECT_EQ(s.sum(&WorkerMetrics::relay_chunks_in_use), 0U);
}

TEST(L4, IdleConnectionIsClosedByTheIdleTimeout) {
  CORING5_REQUIRE_RING();
  Backend be(plain_echo);
  Stack s;
  auto cfg = l4_config(1, addr(be.port()));
  const auto p = cfg.find("idle_timeout_ms = 60000");
  cfg.replace(p, std::string("idle_timeout_ms = 60000").size(),
              "idle_timeout_ms = 150");
  ASSERT_EQ(s.start(cfg), "");
  Client c(s.port());
  ASSERT_TRUE(c.connected());
  ASSERT_TRUE(c.send_all("k"));
  ASSERT_EQ(c.recv_n(1), "k");
  EXPECT_TRUE(c.wait_eof()) << c.last_recv();
  EXPECT_TRUE(
      wait_until([&] { return s.sum(&WorkerMetrics::conns_live) == 0; }));
}

TEST(L4, GracefulShutdownClosesRelayedConnections) {
  CORING5_REQUIRE_RING();
  Backend be(plain_echo);
  Stack s;
  ASSERT_EQ(s.start(l4_config(2, addr(be.port()))), "");
  std::vector<std::unique_ptr<Client>> cs;
  for (int i = 0; i < 8; ++i) {
    cs.push_back(std::make_unique<Client>(s.port()));
    ASSERT_TRUE(cs.back()->send_all("a"));
    ASSERT_EQ(cs.back()->recv_n(1), "a");
  }
  const auto t0 = std::chrono::steady_clock::now();
  s.workers.shutdown(2000ms);
  EXPECT_LT(std::chrono::steady_clock::now() - t0, 1500ms);
  for (auto &c : cs) {
    EXPECT_TRUE(c->wait_eof());
  }
}

TEST(L4, ReloadChangingBackendsAffectsNewConnections) {
  CORING5_REQUIRE_RING();
  Backend a(plain_echo);
  Backend b([](int fd) {
    (void)::send(fd, "B", 1, MSG_NOSIGNAL);
    ::close(fd);
  });
  Stack s;
  ASSERT_EQ(s.start(l4_config(1, addr(a.port()))), "");
  {
    Client c(s.port());
    ASSERT_TRUE(c.send_all("q"));
    EXPECT_EQ(c.recv_n(1), "q");
  }
  // Same listener (name+address) with a different upstream: the port is the
  // one the listener already holds, so reuse it.
  const auto port = s.port();
  auto cfg = l4_config(1, addr(b.port()));
  const auto pos = cfg.find("127.0.0.1:0");
  cfg.replace(pos, std::string("127.0.0.1:0").size(),
              "127.0.0.1:" + std::to_string(port));
  auto first = l4_config(1, addr(a.port()));
  (void)first;
  // The first config used port 0, so the listener identity differs; a plain
  // reload therefore replaces the listener. That is fine: check the result.
  ASSERT_EQ(s.reload(cfg), "");
  Client c(s.port());
  ASSERT_TRUE(c.connected());
  EXPECT_EQ(c.recv_n(1), "B");
  EXPECT_EQ(s.sum(&WorkerMetrics::stale_events), 0U);
}

TEST(L4, ListenerNeedsAnExistingUpstream) {
  auto bad = [](const std::string &text) {
    auto p = parse_config(text);
    return p && !validate_config(*p);
  };
  EXPECT_TRUE(
      bad("[[listener]]\nname=\"a\"\naddress=\":8081\"\nprotocol=\"l4\"\n"));
  EXPECT_TRUE(
      bad("[[listener]]\nname=\"a\"\naddress=\":8081\"\nprotocol=\"l4\"\n"
          "upstream=\"nope\"\n"));
  EXPECT_TRUE(bad("[[upstream]]\nname=\"u\"\nbackends=[]\n"));
  EXPECT_TRUE(bad("[[upstream]]\nname=\"u\"\nbackends=[\"0.0.0.0:80\"]\n"));
  EXPECT_TRUE(bad("[[upstream]]\nname=\"u\"\nbackends=[\"1.2.3.4:0\"]\n"));
  EXPECT_TRUE(bad("[[upstream]]\nname=\"u\"\nbackends=[\"1.2.3.4:80\"]\n"
                  "[[upstream]]\nname=\"u\"\nbackends=[\"1.2.3.5:80\"]\n"));
  EXPECT_TRUE(bad("[[upstream]]\nname=\"u\"\nbackends=[\"127.0.0.1:8081\"]\n"
                  "[[listener]]\nname=\"a\"\naddress=\"127.0.0.1:8081\"\n"
                  "protocol=\"l4\"\nupstream=\"u\"\n"))
      << "self-referencing upstream";
  auto ok =
      parse_config("[[upstream]]\nname=\"u\"\nbackends=[\"127.0.0.1:9\"]\n"
                   "[[listener]]\nname=\"a\"\naddress=\":8081\"\n"
                   "protocol=\"l4\"\nupstream=\"u\"\n");
  ASSERT_TRUE(ok) << ok.error();
  EXPECT_TRUE(validate_config(*ok));
}
