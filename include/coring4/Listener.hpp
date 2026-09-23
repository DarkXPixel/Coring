#pragma once

#include <arpa/inet.h>
#include <asm-generic/socket.h>
#include <cerrno>
#include <cstdint>
#include <expected>
#include <format>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <print>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
namespace Coring4 {

enum class ListenerCreateError : uint8_t {
  CreateSocketFailed = 0,
  SetSockOptFailed,
  BindSocketFailed,
  ListenFailed
};
class Listener {
public:
  Listener() = default;
  Listener(const Listener &) = delete;
  Listener &operator=(const Listener &) = delete;
  Listener(Listener &&other) noexcept
      : fd_(std::exchange(other.fd_, -1)), protocol_(other.protocol_),
        version_(other.version_), address_(other.address_) {}

  ~Listener() {
    if (fd_ >= 0) {
      std::println("Close listener(): {}", listen_address());
      close(fd_);
      fd_ = -1;
    }
  }

  enum class Protocol : uint8_t { Tcp = 0, Udp, Unix };

  enum class IpVersion : uint8_t { Ipv4 = 0, Ipv6, All };

  static std::expected<Listener, ListenerCreateError>
  create(int port, Protocol protocol, IpVersion version) {
    int socket_type = SOCK_NONBLOCK | SOCK_CLOEXEC |
                      (protocol == Protocol::Tcp ? SOCK_STREAM : SOCK_DGRAM);
    int socket_domain = version == IpVersion::Ipv4 ? AF_INET : AF_INET6;
    int listen_fd = ::socket(socket_domain, socket_type, 0);
    if (listen_fd < 0) {
      std::println("Create socket failed (port: {})", port);
      return std::unexpected(ListenerCreateError::CreateSocketFailed);
    }

    int yes_opt = 1;
    int no_opt = 0;

    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes_opt,
                 sizeof(yes_opt));
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &yes_opt,
                 sizeof(yes_opt));

    if (protocol == Protocol::Tcp) {
      int timeout_sec = 3;
      ::setsockopt(listen_fd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &timeout_sec,
                   sizeof(timeout_sec));
      ::setsockopt(listen_fd, IPPROTO_TCP, TCP_NODELAY, &yes_opt,
                   sizeof(yes_opt));
    }

    if (version == IpVersion::All) {
      if (::setsockopt(listen_fd, IPPROTO_IPV6, IPV6_V6ONLY, &no_opt,
                       sizeof(no_opt)) < 0) {
        std::println("Set ipv6_only(0) failed");
        close(listen_fd);
        return std::unexpected(ListenerCreateError::SetSockOptFailed);
      }
    }

    sockaddr_storage addr_storage{};
    int size_sockaddr = 0;

    if (version == IpVersion::Ipv4) {
      auto *addr = reinterpret_cast<sockaddr_in *>(&addr_storage);
      addr->sin_family = AF_INET;
      addr->sin_addr.s_addr = INADDR_ANY;
      addr->sin_port = htons(port);
      size_sockaddr = sizeof(sockaddr_in);
    } else {
      auto *addr = reinterpret_cast<sockaddr_in6 *>(&addr_storage);
      addr->sin6_family = AF_INET6;
      addr->sin6_addr = in6addr_any;
      addr->sin6_port = htons(port);
      size_sockaddr = sizeof(sockaddr_in6);
    }

    if (::bind(listen_fd, reinterpret_cast<struct sockaddr *>(&addr_storage),
               sizeof(addr_storage)) < 0) {
      std::println("bind() port {} failed", port);
      ::close(listen_fd);
      return std::unexpected(ListenerCreateError::BindSocketFailed);
    }

    if (protocol == Protocol::Tcp) {
      if (::listen(listen_fd, SOMAXCONN) < 0) {
        std::println("listen() port {} failed fd: {}", port, listen_fd);
        ::close(listen_fd);
        return std::unexpected(ListenerCreateError::ListenFailed);
      }
    }

    int t = errno;

    Listener result;
    result.fd_ = listen_fd;
    result.address_ = addr_storage;
    result.protocol_ = protocol;
    result.version_ = version;
    return result;
  }

  [[nodiscard]] uint16_t port() const noexcept {
    if (version_ == IpVersion::Ipv4) {
      const auto *addr = reinterpret_cast<const sockaddr_in *>(&address_);
      return ntohs(addr->sin_port);
    }
    if (version_ == IpVersion::Ipv6 || version_ == IpVersion::All) {
      const auto *addr = reinterpret_cast<const sockaddr_in6 *>(&address_);
      return ntohs(addr->sin6_port);
    }
    return 0;
  }

  [[nodiscard]] std::string listen_address() const {
    std::array<char, INET6_ADDRSTRLEN> ip_str{};
    uint16_t port = 0;
    if (version_ == IpVersion::Ipv4) {
      const auto *addr = reinterpret_cast<const sockaddr_in *>(&address_);
      inet_ntop(AF_INET, &(addr->sin_addr), ip_str.data(), ip_str.size());
      port = ntohs(addr->sin_port);
      return std::format("{}:{}", ip_str.data(), port);
    }
    if (version_ == IpVersion::Ipv6 || version_ == IpVersion::All) {
      const auto *addr = reinterpret_cast<const sockaddr_in6 *>(&address_);
      inet_ntop(AF_INET6, &(addr->sin6_addr), ip_str.data(), ip_str.size());
      port = ntohs(addr->sin6_port);
      return std::format("[{}]:{}", ip_str.data(), port);
    }
    return "Unknown";
  }

  [[nodiscard]] int fd() const { return fd_; }

private:
  int fd_{-1};
  Protocol protocol_{Protocol::Tcp};
  IpVersion version_{IpVersion::Ipv4};
  sockaddr_storage address_;
};
} // namespace Coring4