#pragma once

// Test helpers: start the real runtime (WorkerGroup + ConfigManager) in-process
// and talk to it with plain blocking sockets.

#include "coring5/config/Loader.hpp"
#include "coring5/runtime/ConfigManager.hpp"
#include "coring5/runtime/WorkerGroup.hpp"
#include <arpa/inet.h>
#include <cstring>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace Coring5::testing {

// io_uring may be unavailable (seccomp, kernel.io_uring_disabled, old kernel).
// Returns true if the environment can run the integration tests.
inline bool ring_available(std::string *why = nullptr) {
  auto r = Ring::create(RingConfig{.sq_entries = 8, .cq_entries = 16});
  if (!r && why != nullptr) {
    *why = std::string(to_string(r.error().code)) +
           " errno=" + std::to_string(r.error().sys);
  }
  return static_cast<bool>(r);
}

#define CORING5_REQUIRE_RING()                                                 \
  do {                                                                         \
    std::string _why;                                                          \
    if (!::Coring5::testing::ring_available(&_why)) {                          \
      GTEST_SKIP() << "io_uring unavailable: " << _why;                        \
    }                                                                          \
  } while (0)

class Client {
public:
  explicit Client(uint16_t port) {
    fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    timeval tv{.tv_sec = 5, .tv_usec = 0};
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ok_ = ::connect(fd_, reinterpret_cast<sockaddr *>(&a), sizeof(a)) == 0;
  }
  ~Client() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }
  Client(const Client &) = delete;
  Client &operator=(const Client &) = delete;

  [[nodiscard]] bool connected() const { return ok_; }
  bool send_all(const std::string &s) {
    std::size_t off = 0;
    while (off < s.size()) {
      const ssize_t n =
          ::send(fd_, s.data() + off, s.size() - off, MSG_NOSIGNAL);
      if (n <= 0) {
        return false;
      }
      off += static_cast<std::size_t>(n);
    }
    return true;
  }
  // Reads exactly `n` bytes (or fewer on EOF/timeout/error; see last_recv()).
  std::string recv_n(std::size_t n) {
    std::string out;
    char buf[65536];
    while (out.size() < n) {
      const ssize_t r = ::recv(fd_, buf, std::min(sizeof(buf), n - out.size()), 0);
      if (r < 0 && errno == EINTR) {
        ++eintr_count_;
        continue;
      }
      if (r <= 0) {
        last_ret_ = r;
        last_errno_ = r < 0 ? errno : 0;
        break;
      }
      out.append(buf, static_cast<std::size_t>(r));
    }
    return out;
  }
  // True if the peer closed the connection (EOF) within the timeout.
  bool wait_eof() {
    char c;
    ssize_t n;
    do {
      n = ::recv(fd_, &c, 1, 0);
      if (n < 0 && errno == EINTR) {
        ++eintr_count_;
      }
    } while (n < 0 && errno == EINTR);
    last_errno_ = n < 0 ? errno : 0;
    last_ret_ = n;
    return n == 0;
  }
  [[nodiscard]] std::string last_recv() const {
    return "recv=" + std::to_string(last_ret_) + " errno=" + std::to_string(last_errno_) +
           " eintr_retries=" + std::to_string(eintr_count_);
  }
  void shutdown_write() { ::shutdown(fd_, SHUT_WR); }
  void reset() { // abortive close (RST)
    linger l{1, 0};
    ::setsockopt(fd_, SOL_SOCKET, SO_LINGER, &l, sizeof(l));
    ::close(fd_);
    fd_ = -1;
  }

private:
  int fd_{-1};
  bool ok_{false};
  ssize_t last_ret_{0};
  int last_errno_{0};
  int eintr_count_{0};
};

// Builds a config text for N workers and the given listeners (name, address).
inline std::string make_config(
    unsigned workers,
    std::initializer_list<std::pair<const char *, const char *>> listeners,
    unsigned idle_ms = 60000) {
  std::string t = "[runtime]\nworkers = " + std::to_string(workers) +
                  "\nbuffer_count = 256\nbuffer_size = "
                  "4096\nmax_connections_per_worker = 512\n"
                  "tick_ms = 10\ndrain_timeout_ms = 2000\n"
                  "[limits]\nidle_timeout_ms = " +
                  std::to_string(idle_ms) + "\n";
  for (auto &[name, addr] : listeners) {
    t += std::string("[[listener]]\nname = \"") + name + "\"\naddress = \"" +
         addr + "\"\nprotocol = \"echo\"\n";
  }
  return t;
}

struct Stack {
  WorkerGroup workers;
  std::unique_ptr<ConfigManager> config;

  [[nodiscard]] std::string start(const std::string &toml) {
    auto parsed = parse_config(toml);
    if (!parsed) {
      return parsed.error();
    }
    if (auto s = workers.start(parsed->runtime); !s) {
      return s.error();
    }
    config = std::make_unique<ConfigManager>(workers);
    auto r = config->apply(std::move(*parsed));
    return r ? std::string{} : r.error();
  }

  [[nodiscard]] std::string reload(const std::string &toml) {
    auto parsed = parse_config(toml);
    if (!parsed) {
      return parsed.error();
    }
    auto r = config->apply(std::move(*parsed));
    return r ? std::string{} : r.error();
  }

  // Port of the first listener (resolves "127.0.0.1:0").
  [[nodiscard]] uint16_t port(std::size_t i = 0) const {
    return config->current()->listeners.at(i).endpoint.port();
  }

  [[nodiscard]] uint64_t sum(Counter WorkerMetrics::*field) const {
    uint64_t n = 0;
    for (const auto &m : workers.metrics()) {
      n += get(m.*field);
    }
    return n;
  }
};

} // namespace Coring5::testing
