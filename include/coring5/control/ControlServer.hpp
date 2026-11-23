#pragma once

// Unix-socket admin interface. One line in, one response out:
//
//   ping | reload | stats | status | shutdown
//
// Security: socket mode 0660 + SO_PEERCRED check (same uid as the daemon, or
// root). A dedicated poll loop multiplexes bounded nonblocking clients.
// Command handlers run serially on the control thread.

#include <atomic>
#include <functional>
#include <string>
#include <string_view>
#include <thread>

namespace Coring5 {

struct ControlHandlers {
  std::function<std::string()> reload; // returns a one-line report
  std::function<std::string()> stats;  // Prometheus text
  std::function<std::string()> status; // one-line status
  std::function<void()> shutdown;
};

class ControlServer {
public:
  ControlServer(std::string path, ControlHandlers handlers)
      : path_(std::move(path)), handlers_(std::move(handlers)) {}
  ControlServer(const ControlServer &) = delete;
  ControlServer &operator=(const ControlServer &) = delete;
  ~ControlServer() { stop(); }

  [[nodiscard]] std::string start(); // empty string = ok, else error text
  void stop();

private:
  void run();
  struct Client;
  bool service(Client &client, short events);
  [[nodiscard]] std::string execute(std::string_view cmd);

  std::string path_;
  ControlHandlers handlers_;
  int fd_{-1};
  int stop_fd_{-1};
  std::thread thread_;
  std::atomic<bool> running_{false};
};

} // namespace Coring5
