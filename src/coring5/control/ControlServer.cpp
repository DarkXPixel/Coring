#include "coring5/control/ControlServer.hpp"

#include <cerrno>
#include <cstring>
#include <format>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
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
  stop_fd_ = ::eventfd(0, EFD_CLOEXEC);
  if (fd_ < 0 || stop_fd_ < 0) {
    running_ = false;
    return std::format("control socket: {}", std::strerror(errno));
  }
  ::unlink(path_.c_str());
  const mode_t old_umask = ::umask(0117); // creates the socket as 0660
  const int rc = ::bind(fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
  ::umask(old_umask);
  if (rc != 0 || ::listen(fd_, 16) != 0) {
    const std::string err =
        std::format("control socket '{}': {}", path_, std::strerror(errno));
    ::close(fd_);
    ::close(stop_fd_);
    fd_ = stop_fd_ = -1;
    running_ = false;
    return err;
  }
  thread_ = std::thread(&ControlServer::run, this);
  return {};
}

void ControlServer::stop() {
  if (!running_.exchange(false)) {
    return;
  }
  const uint64_t one = 1;
  (void)!::write(stop_fd_, &one, sizeof(one));
  if (thread_.joinable()) {
    thread_.join();
  }
  ::close(fd_);
  ::close(stop_fd_);
  fd_ = stop_fd_ = -1;
  ::unlink(path_.c_str());
}

void ControlServer::run() {
  while (running_.load()) {
    pollfd fds[2] = {{fd_, POLLIN, 0}, {stop_fd_, POLLIN, 0}};
    if (::poll(fds, 2, -1) < 0 && errno != EINTR) {
      return;
    }
    if ((fds[1].revents & POLLIN) != 0) {
      return;
    }
    if ((fds[0].revents & POLLIN) != 0) {
      const int c = ::accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC);
      if (c >= 0) {
        serve(c);
        ::close(c);
      }
    }
  }
}

void ControlServer::serve(int client) {
  ucred cred{};
  socklen_t len = sizeof(cred);
  if (::getsockopt(client, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0 ||
      (cred.uid != ::geteuid() && cred.uid != 0)) {
    constexpr std::string_view deny = "ERR permission denied\n";
    (void)!::send(client, deny.data(), deny.size(), MSG_NOSIGNAL);
    return;
  }
  timeval tv{.tv_sec = 1, .tv_usec = 0};
  ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  char buf[256];
  const ssize_t n = ::recv(client, buf, sizeof(buf) - 1, 0);
  if (n <= 0) {
    return;
  }
  std::string_view cmd(buf, static_cast<std::size_t>(n));
  while (!cmd.empty() &&
         (cmd.back() == '\n' || cmd.back() == '\r' || cmd.back() == ' ')) {
    cmd.remove_suffix(1);
  }
  std::string resp = execute(cmd);
  if (resp.empty() || resp.back() != '\n') {
    resp.push_back('\n');
  }
  (void)!::send(client, resp.data(), resp.size(), MSG_NOSIGNAL);
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
