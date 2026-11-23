#include "coring5/net/IdleUpstreams.hpp"
#include "http1_conn_harness.hpp"
#include <arpa/inet.h>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <poll.h>

using namespace Coring5;
using namespace Coring5::Test;

namespace {
using Op = ConnHarness::Outstanding;

ConfigSnapshot routes() {
  ConfigSnapshot config;
  auto &listener = config.listeners.emplace_back();
  listener.id = kHttpListener;
  listener.route = "site";
  auto &site = config.routes["site"];
  site.hosts.emplace("", 0);
  site.vhosts.emplace_back().exact = {
      {"/file", StaticRoute{"/files"}},
      {"/proxy", ProxyRoute{"origin", UpstreamId{1}}}};
  auto &up = config.upstreams.emplace_back();
  up.id = UpstreamId{1};
  up.backends.push_back(*Endpoint::parse("127.0.0.1:8000"));
  return config;
}

FdOwner loopback() {
  FdOwner listener(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
  FdOwner client(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  auto *sa = reinterpret_cast<sockaddr *>(&addr);
  socklen_t size = sizeof(addr);
  if (::bind(listener.get(), sa, size) || ::listen(listener.get(), 1) ||
      ::getsockname(listener.get(), sa, &size) ||
      ::connect(client.get(), sa, size)) {
    throw std::runtime_error("loopback setup failed");
  }
  return FdOwner(::accept4(listener.get(), nullptr, nullptr, SOCK_CLOEXEC));
}

class AdapterIo : public ::testing::Test {
protected:
  ConfigSnapshot config = routes();
  StaticRoots roots;
  UpstreamPool upstreams;
  std::unique_ptr<ConnHarness> harness;
  size_t initial_fds{};
  size_t fd_count() const {
    return static_cast<size_t>(std::distance(
        std::filesystem::directory_iterator("/proc/self/fd"), {}));
  }
  void SetUp() override {
    initial_fds = fd_count();
    auto root = StaticFileRoot::open("/tmp");
    ASSERT_TRUE(root);
    roots.directories.emplace("/files", std::move(*root));
    upstreams.apply_config(config);
    harness = std::make_unique<ConnHarness>(8, 8, ConnHarness::base_limits(),
                                            &config, kStageChunk, loopback());
    harness->ctx().upstreams = &upstreams;
    harness->ctx().static_roots = &roots;
  }
  void TearDown() override {
    if (harness) {
      harness->c().core.close(harness->ctx(), Errc::Shutdown);
      harness->reconcile();
      harness->flush_pending();
      ASSERT_TRUE(harness->closed());
      harness->reap_closed();
      harness->expect_clean_end();
      EXPECT_EQ(harness->buffers().free_count(), harness->buffers().count());
      harness.reset();
    }
    roots.directories.clear();
    EXPECT_EQ(fd_count(), initial_fds);
  }
  bool pending(OpKind kind, uint8_t aux = 0) const {
    return std::ranges::any_of(harness->outstanding_, [&](const auto &op) {
      return op.kind == kind && op.tag.aux() == aux;
    });
  }
  Op operation(OpKind kind, uint8_t aux = 0) {
    for (auto op : harness->outstanding_) {
      if (op.kind == kind && op.tag.aux() == aux) {
        return op;
      }
    }
    throw std::runtime_error("missing submission kind " +
                             std::to_string(static_cast<int>(kind)));
  }
  void done(Op op, int result) {
    harness->deliver_op(op, result);
    harness->drain_exchanges();
  }
  void request(std::string_view raw) {
    while (!raw.empty()) {
      auto part = raw.substr(0, kBuffer);
      ASSERT_TRUE(pending(OpKind::Recv));
      harness->deliver_recv(part);
      harness->drain_exchanges();
      raw.remove_prefix(part.size());
    }
  }
  std::string sent(uint8_t aux = 0) {
    std::string bytes;
    while (pending(OpKind::Send, aux)) {
      auto op = operation(OpKind::Send, aux);
      const auto n =
          std::min(op.len, 9U); // short completions, including chunk framing
      bytes.append(static_cast<const char *>(op.buf), n);
      done(op, static_cast<int>(n));
    }
    return bytes;
  }
  void connected(int result = 0) {
    done(operation(OpKind::LinkTimeout), -ECANCELED);
    done(operation(OpKind::Connect), result);
  }
  void upstream(std::string_view bytes) {
    while (!bytes.empty()) {
      auto op = operation(OpKind::Recv, kAuxPeer);
      auto part = bytes.substr(0, op.len);
      std::memcpy(const_cast<void *>(op.buf), part.data(), part.size());
      done(op, static_cast<int>(part.size()));
      bytes.remove_prefix(part.size());
    }
  }
  void opened(uint64_t size) {
    auto open = operation(OpKind::FileOpen);
    EXPECT_STREQ(static_cast<const char *>(open.buf), "file");
    done(open, ::open("/dev/null", O_RDONLY | O_CLOEXEC));
    auto stat = operation(OpKind::FileStat);
    auto *out = static_cast<struct statx *>(const_cast<void *>(stat.buf));
    out->stx_mask = StaticFileRoot::stat_fields;
    out->stx_mode = S_IFREG | 0444;
    out->stx_size = size;
    done(stat, 0);
  }
};

TEST_F(AdapterIo, FileReadStopsAtCreditAndSurvivesRootRetirement) {
  request(req("GET", "/file", "Range: bytes=5-104\r\n"));
  roots.directories.clear();
  harness->ctx().snapshot = nullptr;
  opened(120);
  auto first = operation(OpKind::Read);
  EXPECT_EQ(first.offset, 5u);
  ASSERT_EQ(first.len, kHigh);
  std::memset(const_cast<void *>(first.buf), 'a', first.len);
  done(first, static_cast<int>(first.len));
  EXPECT_FALSE(pending(OpKind::Read));
  auto wire = sent();
  auto last = operation(OpKind::Read);
  EXPECT_EQ(last.offset, 5u + kHigh);
  EXPECT_EQ(last.len, 100u - kHigh);
  std::memset(const_cast<void *>(last.buf), 'b', last.len);
  done(last, static_cast<int>(last.len));
  wire += sent();
  EXPECT_TRUE(wire.starts_with("HTTP/1.1 206"));
  EXPECT_TRUE(wire.contains("Content-Range: bytes 5-104/120\r\n"));
  EXPECT_TRUE(
      wire.ends_with(std::string(kHigh, 'a') + std::string(last.len, 'b')));
}

TEST_F(AdapterIo, FileOpenWinningCloseRaceReleasesReturnedFd) {
  request(req("GET", "/file"));
  auto op = operation(OpKind::FileOpen);
  harness->c().core.close(harness->ctx(), Errc::Shutdown);
  harness->reconcile();
  auto late = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
  ASSERT_GE(late, 0);
  done(op, late);
  EXPECT_EQ(::fcntl(late, F_GETFD), -1);
  EXPECT_EQ(errno, EBADF);
}

TEST_F(AdapterIo, FileTruncationAbortsAfterHead) {
  request(req("GET", "/file"));
  opened(300);
  auto wire = sent();
  ASSERT_TRUE(wire.starts_with("HTTP/1.1 200"));
  done(operation(OpKind::Read), 0);
  EXPECT_TRUE(harness->closing());
}

TEST_F(AdapterIo, ProxyStreamsChunkedUploadAndFiltersBothHeadsAcrossReload) {
  request(
      req("POST", "/proxy",
          "Transfer-Encoding: chunked\r\nExpect: 100-continue\r\nConnection: "
          "private\r\nPrivate: secret\r\nX-Forwarded-For: fake\r\n"));
  connected();
  auto head = sent(kAuxPeer);
  EXPECT_TRUE(head.starts_with("POST /proxy HTTP/1.1"));
  EXPECT_TRUE(head.contains("X-Forwarded-For: 127.0.0.1\r\n"));
  EXPECT_TRUE(head.contains("X-Forwarded-Proto: http\r\n"));
  EXPECT_TRUE(head.contains("X-Forwarded-Host: a\r\n"));
  EXPECT_FALSE(head.contains("fake"));
  EXPECT_FALSE(head.contains("secret"));
  upstream("HTTP/1.1 100 Continue\r\n\r\n");
  EXPECT_EQ(sent(), "HTTP/1.1 100 Continue\r\n\r\n");
  request("4\r\nbody\r\n0\r\n\r\n");
  EXPECT_EQ(sent(kAuxPeer), "4\r\nbody\r\n0\r\n\r\n");
  harness->ctx().snapshot = nullptr;
  upstreams.apply_config(ConfigSnapshot{});
  upstream("HTTP/1.1 201 Created\r\nTransfer-Encoding: chunked\r\nConnection: "
           "hidden\r\nHidden: secret\r\nX-Origin: "
           "one\r\n\r\n4\r\n1234\r\n0\r\n\r\n");
  auto wire = sent();
  EXPECT_TRUE(wire.starts_with("HTTP/1.1 201"));
  EXPECT_TRUE(wire.contains("X-Origin: one\r\n"));
  EXPECT_FALSE(wire.contains("secret"));
  EXPECT_TRUE(wire.ends_with("4\r\n1234\r\n0\r\n\r\n"));
}

TEST_F(AdapterIo, ForwardsOnlyPermittedTrailersToOptedInClients) {
  request(req("GET", "/proxy", "Connection: TE\r\nTE: Trailers\r\n"));
  connected();
  const auto head = sent(kAuxPeer);
  EXPECT_TRUE(head.contains("Connection: close, TE\r\n"));
  EXPECT_TRUE(head.contains("TE: trailers\r\n"));
  upstream("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
           "4\r\nbody\r\n0\r\nX-Checksum: abc\r\nSet-Cookie: secret\r\n\r\n");
  const auto wire = sent();
  EXPECT_TRUE(wire.ends_with("4\r\nbody\r\n0\r\nX-Checksum: abc\r\n\r\n"));
  EXPECT_FALSE(wire.contains("secret"));
  EXPECT_EQ(get(harness->metrics().trailers_forwarded), 1U);
  EXPECT_EQ(get(harness->metrics().trailers_dropped), 1U);
}

TEST_F(AdapterIo, RejectsTrailerNominatedByUpstreamConnection) {
  request(req("GET", "/proxy"));
  connected();
  sent(kAuxPeer);
  upstream("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
           "Connection: X-Private\r\n\r\n");
  EXPECT_TRUE(sent().starts_with("HTTP/1.1 200"));
  upstream("0\r\nX-Private: data\r\n\r\n");
  EXPECT_TRUE(sent().empty());
  EXPECT_TRUE(harness->closing());
  EXPECT_EQ(get(harness->metrics().trailers_rejected), 1U);
}

TEST_F(AdapterIo, UpgradeRelaysRawBytesAndDrainsOnPeerEof) {
  request(req("GET", "/proxy",
              "Connection: Upgrade\r\nUpgrade: WebSocket\r\n"
              "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
              "Sec-WebSocket-Version: 13\r\n") + "client-tail");
  connected();
  const auto handshake = sent(kAuxPeer);
  EXPECT_TRUE(handshake.contains("Upgrade: websocket\r\n"));
  EXPECT_TRUE(handshake.contains("Connection: Upgrade\r\n"));
  EXPECT_TRUE(handshake.contains("Sec-WebSocket-Version: 13\r\n"));
  EXPECT_FALSE(handshake.contains("client-tail"));

  upstream("HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\n"
           "Upgrade: websocket\r\n"
           "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\nserver-tail");
  EXPECT_EQ(sent(kAuxPeer), "client-tail");
  const auto wire = sent();
  EXPECT_TRUE(wire.starts_with("HTTP/1.1 101 Switching Protocols\r\n"));
  EXPECT_TRUE(wire.contains("Upgrade: websocket\r\n"));
  EXPECT_TRUE(wire.contains("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"));
  EXPECT_TRUE(wire.ends_with("\r\n\r\nserver-tail"));
  EXPECT_FALSE(wire.contains("Transfer-Encoding"));
  request("more-client-data");
  EXPECT_EQ(sent(kAuxPeer), "more-client-data");
  upstream("more-server-data");
  EXPECT_EQ(sent(), "more-server-data");
  done(operation(OpKind::Recv, kAuxPeer), 0);
  EXPECT_TRUE(harness->closing());
}

TEST_F(AdapterIo, UnsolicitedUpgradeIsRejectedBeforeSwitching) {
  request(req("GET", "/proxy"));
  connected();
  sent(kAuxPeer);
  upstream("HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\n"
           "Upgrade: websocket\r\n\r\n");
  EXPECT_TRUE(sent().starts_with("HTTP/1.1 502"));
}

TEST_F(AdapterIo, RetryUnsentGetButNeverReplayShortWrite) {
  request(req("GET", "/proxy"));
  connected();
  done(operation(OpKind::Send, kAuxPeer), -EPIPE);
  EXPECT_EQ(get(harness->metrics().upstream_retries), 1U);
  connected();
  done(operation(OpKind::Send, kAuxPeer), 1);
  done(operation(OpKind::Send, kAuxPeer), -EPIPE);
  EXPECT_FALSE(pending(OpKind::Connect));
  EXPECT_TRUE(sent().starts_with("HTTP/1.1 502"));
}

TEST_F(AdapterIo, PeerCloseAfterIdleProbeRetriesOnlyUnsentRequest) {
  IdleUpstreams idle;
  harness->ctx().idle_upstreams = &idle;
  int pair[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair), 0);
  FdOwner server(pair[1]);
  const auto ref = upstreams.pick(UpstreamId{1}, harness->ctx().now);
  ASSERT_TRUE(ref);
  idle.park(*ref, harness->ctx().now + Millis(1000), FdOwner(pair[0]));
  EXPECT_EQ(upstreams.active(*ref), 0U);

  request(req("GET", "/proxy"));
  EXPECT_FALSE(pending(OpKind::Connect));
  EXPECT_EQ(upstreams.active(*ref), 1U);
  server.reset();
  done(operation(OpKind::Send, kAuxPeer), -EPIPE);
  EXPECT_EQ(get(harness->metrics().upstream_retries), 1U);
  connected();
  done(operation(OpKind::Send, kAuxPeer), 1);
  done(operation(OpKind::Send, kAuxPeer), -EPIPE);
  EXPECT_FALSE(pending(OpKind::Connect));
  EXPECT_TRUE(sent().starts_with("HTTP/1.1 502"));
  EXPECT_EQ(upstreams.active(*ref), 0U);
  harness->ctx().idle_upstreams = nullptr;
}

TEST_F(AdapterIo, DirtyOrClosedIdleSocketsAreEvictedBeforeSend) {
  IdleUpstreams idle;
  const auto ref = upstreams.pick(UpstreamId{1}, harness->ctx().now);
  ASSERT_TRUE(ref);
  for (bool closed : {false, true}) {
    int pair[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair), 0);
    FdOwner peer(pair[1]);
    if (closed) {
      peer.reset();
    } else {
      ASSERT_EQ(::send(peer.get(), "x", 1, MSG_NOSIGNAL), 1);
    }
    idle.park(*ref, harness->ctx().now + Millis(1000), FdOwner(pair[0]));
    EXPECT_FALSE(idle.take(*ref, harness->ctx().now));
  }
}

TEST_F(AdapterIo, FailedPostDialIsNeverRetried) {
  request(req("POST", "/proxy"));
  connected(-ECONNREFUSED);
  EXPECT_FALSE(pending(OpKind::Connect));
  EXPECT_EQ(get(harness->metrics().upstream_retries), 0U);
  EXPECT_TRUE(sent().starts_with("HTTP/1.1 503"));
}

TEST_F(AdapterIo, InvalidUpstreamFramingFailsBeforeFinalHead) {
  request(req("GET", "/proxy"));
  connected();
  sent(kAuxPeer);
  upstream("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nTransfer-Encoding: "
           "chunked\r\n\r\n");
  EXPECT_TRUE(sent().starts_with("HTTP/1.1 502"));
}

TEST_F(AdapterIo, ProxyEarlyFinalWithUploadInFlightDrainsBothFds) {
  request(req("PUT", "/proxy", "Content-Length: 20\r\n") + "abc");
  connected();
  auto head = operation(OpKind::Send, kAuxPeer);
  done(head, static_cast<int>(head.len));
  const auto upload = operation(OpKind::Send, kAuxPeer);
  upstream("HTTP/1.1 413 Too Large\r\nContent-Length: 0\r\n\r\n");
  EXPECT_TRUE(sent().starts_with("HTTP/1.1 413"));
  EXPECT_TRUE(harness->closing());
  EXPECT_EQ(std::string_view(static_cast<const char *>(upload.buf), upload.len),
            "abc");
  done(upload, -EPIPE);
}

TEST_F(AdapterIo, UpstreamTimeoutWithReceiveInFlightReturns504) {
  request(req("GET", "/proxy"));
  connected();
  sent(kAuxPeer);
  auto read = operation(OpKind::Recv, kAuxPeer);
  harness->c().on_timer(harness->ctx(), DeadlineKind::UpstreamRead);
  harness->drain_exchanges();
  EXPECT_TRUE(sent().starts_with("HTTP/1.1 504"));
  done(read, -ECANCELED);
}

TEST_F(AdapterIo, ProxyStopsReceivingAtResponseWatermarkAndFinishesAtEof) {
  request(req("GET", "/proxy"));
  connected();
  sent(kAuxPeer);
  upstream("HTTP/1.0 200 OK\r\n\r\n");
  upstream(std::string(kHigh, 'x'));
  EXPECT_FALSE(pending(OpKind::Recv, kAuxPeer));
  auto wire = sent();
  ASSERT_TRUE(pending(OpKind::Recv, kAuxPeer));
  done(operation(OpKind::Recv, kAuxPeer), 0);
  wire += sent();
  EXPECT_TRUE(wire.contains("Transfer-Encoding: chunked"));
  EXPECT_EQ(std::ranges::count(wire, 'x'), kHigh);
  EXPECT_TRUE(wire.ends_with("0\r\n\r\n"));
}

TEST_F(AdapterIo, DisconnectDuringLinkedDialWaitsForBothCompletions) {
  request(req("GET", "/proxy"));
  auto connect = operation(OpKind::Connect);
  auto timeout = operation(OpKind::LinkTimeout);
  done(operation(OpKind::Disconnect), POLLERR);
  EXPECT_TRUE(harness->closing());
  done(timeout, -ECANCELED);
  done(connect, 0);
  EXPECT_FALSE(pending(OpKind::Send, kAuxPeer));
}

TEST_F(AdapterIo, TruncatedProxyBodyCannotRecycleAnInflightPayloadSend) {
  request(req("GET", "/proxy"));
  connected();
  sent(kAuxPeer);
  upstream("HTTP/1.1 200 OK\r\nContent-Length: 1000\r\n\r\n");
  EXPECT_TRUE(sent().starts_with("HTTP/1.1 200"));
  upstream("0123456789abcdef");
  auto payload = operation(OpKind::Send);
  done(operation(OpKind::Recv, kAuxPeer), 0);
  // The Exchange has already returned its chunks to the pool.
  EXPECT_EQ(harness->chunk_pool_.in_use(), 1u); // responder's recv buffer
  EXPECT_EQ(
      std::string_view(static_cast<const char *>(payload.buf), payload.len),
      "0123456789abcdef");
  done(payload, static_cast<int>(payload.len));
  EXPECT_TRUE(harness->closing());
}

TEST_F(AdapterIo, UploadPausesClientReceivesUntilUpstreamConsumesCredit) {
  request(req("PUT", "/proxy", "Content-Length: 128\r\n"));
  request(std::string(kHigh, 'z'));
  EXPECT_FALSE(pending(OpKind::Recv));
  connected();
  auto head = operation(OpKind::Send, kAuxPeer);
  done(head, static_cast<int>(head.len));
  EXPECT_EQ(sent(kAuxPeer), std::string(kHigh, 'z'));
  EXPECT_TRUE(pending(OpKind::Recv));
  request(std::string(kHigh, 'x'));
  EXPECT_EQ(sent(kAuxPeer), std::string(kHigh, 'x'));
  upstream("HTTP/1.1 204 No Content\r\n\r\n");
  EXPECT_TRUE(sent().starts_with("HTTP/1.1 204"));
}

TEST_F(AdapterIo, MissingFileCanBeFollowedByAnotherRequest) {
  request(req("GET", "/file"));
  done(operation(OpKind::FileOpen), -ENOENT);
  EXPECT_TRUE(sent().starts_with("HTTP/1.1 404"));
  request(req("HEAD", "/file"));
  opened(10);
  auto head = sent();
  EXPECT_TRUE(head.starts_with("HTTP/1.1 200"));
  EXPECT_TRUE(head.ends_with("Content-Length: 10\r\n\r\n"));
  EXPECT_FALSE(pending(OpKind::Read));
}
} // namespace
