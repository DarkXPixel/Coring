#include "coring5/control/ControlServer.hpp"
#include "coring5/io/FdOwner.hpp"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <gtest/gtest.h>
#include <iterator>
#include <memory>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace Coring5 {
namespace {

auto open_fds() {
  return std::distance(std::filesystem::directory_iterator("/proc/self/fd"),
                       std::filesystem::directory_iterator{});
}

class ControlSocket : public ::testing::Test {
protected:
  void SetUp() override {
    char dir[] = "/tmp/coring-control-XXXXXX";
    ASSERT_NE(::mkdtemp(dir), nullptr);
    path = std::string(dir) + "/admin.sock";
    ControlHandlers handlers{};
    handlers.stats = [] { return std::string(8 * 1024 * 1024, 's'); };
    server = std::make_unique<ControlServer>(path, std::move(handlers));
    ASSERT_EQ(server->start(), "");
  }

  void TearDown() override {
    if (server) {
      server->stop();
      EXPECT_FALSE(std::filesystem::exists(path));
      std::filesystem::remove(std::filesystem::path(path).parent_path());
    }
    EXPECT_EQ(open_fds(), baseline);
  }

  FdOwner connect_client() const {
    FdOwner fd(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    if (::connect(fd.get(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) <
        0) {
      ADD_FAILURE() << std::strerror(errno);
      return {};
    }
    timeval limit{.tv_sec = 5, .tv_usec = 0};
    EXPECT_EQ(
        ::setsockopt(fd.get(), SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit)),
        0);
    return fd;
  }

  static void send_text(const FdOwner &fd, std::string_view text) {
    ASSERT_EQ(::send(fd.get(), text.data(), text.size(), MSG_NOSIGNAL),
              static_cast<ssize_t>(text.size()));
  }

  static std::string receive(const FdOwner &fd) {
    std::string result;
    std::array<char, 16384> buf{};
    for (;;) {
      const auto n = ::recv(fd.get(), buf.data(), buf.size(), 0);
      if (n < 0 && errno == EINTR) {
        continue;
      }
      if (n <= 0) {
        EXPECT_EQ(n, 0) << std::strerror(errno);
        return result;
      }
      result.append(buf.data(), static_cast<std::size_t>(n));
    }
  }

  FdOwner pending_stats() const {
    auto fd = connect_client();
    send_text(fd, "stats\n");
    pollfd pfd{fd.get(), POLLIN, 0};
    EXPECT_EQ(::poll(&pfd, 1, 5000), 1);
    EXPECT_TRUE(pfd.revents & POLLIN);
    return fd;
  }

  const decltype(open_fds()) baseline = open_fds();
  std::string path;
  std::unique_ptr<ControlServer> server;
};

TEST_F(ControlSocket, FragmentedCommandAllowsAnotherClientThrough) {
  auto partial = connect_client();
  send_text(partial, "pi");
  auto complete = connect_client();
  send_text(complete, "ping\n");
  EXPECT_EQ(receive(complete), "PONG\n");
  send_text(partial, "ng \r\n");
  EXPECT_EQ(receive(partial), "PONG\n");
}

TEST_F(ControlSocket, FullServerShedsExtraClientAndReusesReleasedSlot) {
  std::array<FdOwner, 16> held{};
  for (auto &fd : held) {
    fd = connect_client();
    send_text(fd, "pi");
  }
  auto excess = connect_client();
  EXPECT_EQ(receive(excess), "");

  send_text(held.front(), "ng\n");
  EXPECT_EQ(receive(held.front()), "PONG\n");
  auto replacement = connect_client();
  send_text(replacement, "ping\n");
  EXPECT_EQ(receive(replacement), "PONG\n");
}

TEST_F(ControlSocket, BackpressuredStatsResumesAfterPing) {
  auto stats = pending_stats();
  auto ping = connect_client();
  send_text(ping, "ping\n");
  EXPECT_EQ(receive(ping), "PONG\n");
  auto reply = receive(stats);
  ASSERT_EQ(reply.size(), 8 * 1024 * 1024U + 1);
  EXPECT_EQ(reply.find_first_not_of('s'), reply.size() - 1);
  EXPECT_EQ(reply.back(), '\n');
}

TEST_F(ControlSocket, StopWithPartialReadAndPendingWriteThenRestart) {
  auto partial = connect_client();
  send_text(partial, "pi");
  auto stats = pending_stats();
  server->stop();
  EXPECT_EQ(receive(partial), "");
  EXPECT_LT(receive(stats).size(), 8 * 1024 * 1024U);
  EXPECT_FALSE(std::filesystem::exists(path));
  server->stop();
  ASSERT_EQ(server->start(), "");
  auto ping = connect_client();
  send_text(ping, "ping\n");
  EXPECT_EQ(receive(ping), "PONG\n");
}

TEST_F(ControlSocket, IncompleteCommandExpiresAndOversizedCommandIsRejected) {
  auto idle = connect_client();
  send_text(idle, "p");
  EXPECT_EQ(receive(idle), "");
  auto oversized = connect_client();
  send_text(oversized, std::string(256, 'x'));
  EXPECT_EQ(receive(oversized), "ERR command too long\n");
}

} // namespace
} // namespace Coring5
