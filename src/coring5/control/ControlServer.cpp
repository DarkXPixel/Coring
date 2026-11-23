#include "coring5/control/ControlServer.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <format>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <system_error>
#include <unistd.h>

namespace Coring5 {

std::string ControlServer::start() {
  if (running_.exchange(true)) {
    return {};
  }
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path_.size() >= sizeof(addr.sun_path)) {
    running_ = false;
    return "control socket path too long";
  }
  std::memcpy(addr.sun_path, path_.c_str(), path_.size() + 1);

  fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd_ < 0) {
    running_ = false;
    return std::format("control socket: {}", std::strerror(errno));
  }
  stop_fd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (stop_fd_ < 0) {
    const int error = errno;
    ::close(fd_);
    fd_ = -1;
    running_ = false;
    return std::format("control eventfd: {}", std::strerror(error));
  }
  ::unlink(path_.c_str());
  const mode_t old_umask = ::umask(0117); // creates the socket as 0660
  const int rc = ::bind(fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
  ::umask(old_umask);
  if (rc != 0 || ::listen(fd_, 16) != 0) {
    const std::string err =
        std::format("control socket '{}': {}", path_, std::strerror(errno));
    if (rc == 0) {
      ::unlink(path_.c_str());
    }
    ::close(fd_);
    ::close(stop_fd_);
    fd_ = stop_fd_ = -1;
    running_ = false;
    return err;
  }
  try {
    thread_ = std::thread(&ControlServer::run, this);
  } catch (const std::system_error &error) {
    stop();
    return error.what();
  }
  return {};
}

void ControlServer::stop() {
  if (!running_.exchange(false)) {
    return;
  }
  const uint64_t one = 1;
  while (::write(stop_fd_, &one, sizeof(one)) < 0 && errno == EINTR) {
  }
  if (thread_.joinable()) {
    thread_.join();
  }
  ::close(fd_);
  ::close(stop_fd_);
  fd_ = stop_fd_ = -1;
  ::unlink(path_.c_str());
}

namespace {
using Clock = std::chrono::steady_clock;
constexpr auto kTimeout = std::chrono::seconds(1);
constexpr std::size_t kClients = 16;

bool retryable() {
  return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK;
}
} // namespace

struct ControlServer::Client {
  int fd{-1};
  Clock::time_point deadline{};
  std::array<char, 256> input{};
  std::size_t used{};
  std::string response;
  std::size_t sent{};

  void close() {
    if (fd >= 0) {
      ::close(fd);
      fd = -1;
    }
    used = sent = 0;
    std::string{}.swap(response);
  }
};

void ControlServer::run() {
  std::array<Client, kClients> clients{};
  Clock::time_point accept_after{};
  while (running_.load()) {
    const auto now = Clock::now();
    auto next = Clock::time_point::max();
    std::array<pollfd, kClients + 2> fds{};
    if (accept_after > now) {
      next = accept_after;
    }
    fds[0] = {accept_after > now ? -1 : fd_, POLLIN, 0};
    fds[1] = {stop_fd_, POLLIN, 0};
    for (std::size_t i = 0; i < clients.size(); ++i) {
      auto &c = clients[i];
      if (c.fd >= 0) {
        next = std::min(next, c.deadline);
      }
      fds[i + 2] = {
          c.fd, static_cast<short>(c.response.empty() ? POLLIN : POLLOUT), 0};
    }
    int timeout = -1;
    if (next != Clock::time_point::max()) {
      timeout = static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(
                                     std::max(next, now) - now)
                                     .count());
    }
    if (::poll(fds.data(), fds.size(), timeout) < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (fds[1].revents != 0 || !running_.load()) {
      break;
    }
    for (std::size_t i = 0; i < clients.size(); ++i) {
      auto &c = clients[i];
      const short events = fds[i + 2].revents;
      if (c.fd >= 0 && (Clock::now() >= c.deadline ||
                        (events != 0 && !service(c, events)))) {
        c.close();
      }
    }
    // Accept after dispatch so readiness never carries over to a reused slot.
    if ((fds[0].revents & POLLIN) != 0) {
      int fd = ::accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
      if (fd < 0) {
        if (!retryable()) {
          accept_after = Clock::now() + kTimeout;
        }
        continue;
      }
      auto slot = std::ranges::find(clients, -1, &Client::fd);
      if (slot == clients.end()) {
        ::close(fd);
        continue;
      }
      slot->fd = fd;
      slot->deadline = Clock::now() + kTimeout;
      ucred cred{};
      socklen_t len = sizeof(cred);
      if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0 ||
          (cred.uid != ::geteuid() && cred.uid != 0)) {
        slot->response = "ERR permission denied\n";
      }
    }
  }
  for (auto &client : clients) {
    client.close();
  }
}

bool ControlServer::service(Client &c, short events) {
  if ((events & (POLLERR | POLLNVAL)) != 0) {
    return false;
  }
  if (c.response.empty()) {
    const auto n =
        ::recv(c.fd, c.input.data() + c.used, c.input.size() - c.used, 0);
    if (n < 0) {
      return retryable();
    }
    c.used += static_cast<std::size_t>(n);
    std::string_view cmd(c.input.data(), c.used);
    const auto end = cmd.find('\n');
    if (end == cmd.npos && n > 0 && c.used < c.input.size()) {
      return true;
    }
    if (end == cmd.npos && c.used == c.input.size()) {
      c.response = "ERR command too long\n";
    } else {
      if (c.used == 0) {
        return false;
      }
      cmd = cmd.substr(0, end);
      while (!cmd.empty() && (cmd.back() == '\r' || cmd.back() == ' ')) {
        cmd.remove_suffix(1);
      }
      c.response = execute(cmd);
      if (c.response.empty() || c.response.back() != '\n') {
        c.response.push_back('\n');
      }
    }
    c.deadline = Clock::now() + kTimeout;
  }
  const auto n = ::send(c.fd, c.response.data() + c.sent,
                        c.response.size() - c.sent, MSG_NOSIGNAL);
  if (n <= 0) {
    return n < 0 && retryable();
  }
  c.sent += static_cast<std::size_t>(n);
  c.deadline = Clock::now() + kTimeout;
  return c.sent < c.response.size();
}

std::string ControlServer::execute(std::string_view cmd) {
  if (cmd == "ping") {
    return "PONG";
  }
  if (cmd == "reload" && handlers_.reload) {
    return handlers_.reload();
  }
  if (cmd == "stats" && handlers_.stats) {
    return handlers_.stats();
  }
  if (cmd == "status" && handlers_.status) {
    return handlers_.status();
  }
  if (cmd == "shutdown" && handlers_.shutdown) {
    handlers_.shutdown();
    return "OK shutting down";
  }
  return "ERR unknown command";
}

} // namespace Coring5
