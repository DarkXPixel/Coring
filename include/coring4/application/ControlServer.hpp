#pragma once

#include "coring4/application/CoringDaemon.hpp"
#include <atomic>
#include <cstring>
#include <stdexcept>
#include <string>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
namespace Coring4::Control {
class CoringServer {
public:
  CoringServer(CoringDaemon &daemon, std::string_view socket_path)
      : daemon_(daemon), socket_path_(socket_path) {}

  ~CoringServer() { stop(); }

  void start() {
    if (running_.exchange(true)) {
      return;
    }

    server_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
      throw std::runtime_error("socket failed");
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;

    std::strncpy(address.sun_path, socket_path_.c_str(),
                 sizeof(address.sun_path) - 1);

    unlink(socket_path_.c_str());

    if (::bind(server_fd_, reinterpret_cast<sockaddr *>(&address),
               sizeof(address)) < 0) {
      throw std::runtime_error("bind failed");
    }

    if (::listen(server_fd_, 16) < 0) {
      throw std::runtime_error("listen failed");
    }

    thread_ = std::thread(&CoringServer::run, this);
  }
  void stop() {
    if (!running_.exchange(false)) {
      return;
    }

    if (thread_.joinable()) {
      thread_.join();
    }

    if (server_fd_ >= 0) {
      ::close(server_fd_);
      server_fd_ = -1;
    }

    unlink(socket_path_.c_str());
  }

private:
  void run() {
    while (running_.load()) {
      pollfd pfd{.fd = server_fd_, .events = POLLIN, .revents = 0};

      int result = poll(&pfd, 1, 500);
      if (result <= 0) {
        continue;
      }

      if (!(pfd.revents & POLLIN)) {
        continue;
      }

      int client_fd = accept(server_fd_, nullptr, nullptr);

      if (client_fd < 0) {
        continue;
      }

      handle_client(client_fd);
      ::close(client_fd);
    }
  }
  void handle_client(int client_fd) {
    char buffer[256]{};
    auto bytes = recv(client_fd, buffer, sizeof(buffer) - 1, 0);

    if (bytes <= 0) {
      return;
    }
    std::string_view command(buffer, bytes);

    std::string response;

    if (command == "PING") {
      response = "PONG";
    }

    else if (command == "SHUTDOWN") {
      response = "SHUTTING_DOWN";
      daemon_.stop();
    }

    send(client_fd, response.data(), response.size(), MSG_NOSIGNAL);
  }

  CoringDaemon &daemon_;
  std::string socket_path_;
  int server_fd_{-1};
  std::atomic_bool running_{false};
  std::thread thread_;
};
} // namespace Coring4::Control
