#pragma once

#include "coring4/core/IpEndpoint.hpp"
#include "coring4/core/Types.hpp"
#include "coring4/listeners/ListenerConfig.hpp"
#include "coring4/network/NetworkConfig.hpp"
#include "coring4/tcp/TcpConfig.hpp"
#include <algorithm>
#include <asm-generic/socket.h>
#include <cerrno>
#include <cstdint>
#include <expected>
#include <functional>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
namespace Coring4 {
class Listener2 {
public:
  static std::expected<Listener2, std::string>
  create(const ListenerConfig &config, const NetworkConfig &network_config) {
    if (auto err = validate_config(config); err) {
      return std::unexpected(err.value());
    }

    if (config.transport == TransportProtocol::Tcp) {
      return create_tcp(config, network_config);
    } else if (config.transport == TransportProtocol::Udp) {
      return create_udp(config);
    }
  }

  Listener2() = default;
  Listener2(const Listener2 &) = delete;
  Listener2 &operator=(const Listener2 &) = delete;

  Listener2(Listener2 &&other) noexcept
      : fd(std::exchange(other.fd, -1)), ip_endpoint(other.ip_endpoint),
        is_raw(other.is_raw), support_http1(other.support_http1),
        support_http2(other.support_http2), is_quic(other.is_quic),
        protocol(other.protocol), name_upstream_pool(other.name_upstream_pool),
        name_route(other.name_route), redirect_to_port(other.redirect_to_port) {
  }

  ~Listener2() {
    if (fd != -1) {
      ::close(fd);
    }
  }

  [[nodiscard]] int get_fd() const { return fd; }

  [[nodiscard]] TransportProtocol get_transport() const { return protocol; }

private:
  static std::optional<std::string>
  validate_config(const ListenerConfig &config) {
    bool is_raw = false;
    bool is_quic = false;
    bool support_http1 = false;
    bool support_http2 = false;

    if (config.protocols.empty()) {
      return "Must be protocols";
    }
    for (const auto &i : config.protocols) {
      if (i == ApplicationProtocol::L4) {
        if (config.protocols.size() != 1) {
          return "If is l4, must be only 1 protocol";
        }
      } else if (i == ApplicationProtocol::QUIC) {
        if (config.protocols.size() != 1) {
          return "If you want udp and tcp, must be 2 config";
        }
        if (config.transport != TransportProtocol::Udp) {
          return "For quic must be udp";
        }
        is_quic = true;
      } else if (i == ApplicationProtocol::HTTP1) {
        support_http1 = true;
      } else if (i == ApplicationProtocol::HTTP2) {
        support_http2 = true;
      }
    }
    if ((support_http1 || support_http2) &&
        config.transport != TransportProtocol::Tcp) {
      return "For http1 or http2 must be tcp";
    }

    if (is_quic && config.tls.empty()) {
      return "For quic must be tls";
    }
    if (support_http1 && support_http2 && config.tls.empty()) {
      return "For http1 and http2 together must be tls";
    }

    return std::nullopt;
  }

  static std::expected<Listener2, std::string>
  create_udp(const ListenerConfig &config) {
    return {};
  }
  static std::expected<Listener2, std::string>
  create_tcp(const ListenerConfig &config,
             const NetworkConfig &network_config) {
    bool is_raw =
        std::ranges::contains(config.protocols, ApplicationProtocol::L4);
    bool support_http1 =
        std::ranges::contains(config.protocols, ApplicationProtocol::HTTP1);

    bool support_http2 =
        std::ranges::contains(config.protocols, ApplicationProtocol::HTTP2);

    int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
      return std::unexpected(
          std::format("Create tcp socket failed (errno: {})", errno));
    }

    int yes_opt = 1;
    int no_opt = 0;

    if (::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes_opt,
                     sizeof(yes_opt)) < 0) {
      ::close(listen_fd);
      return std::unexpected(
          std::format("setsockopt(SO_REUSEADDR) failed (errno: {})", errno));
    }
    if (::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &yes_opt,
                     sizeof(yes_opt)) < 0) {
      ::close(listen_fd);
      return std::unexpected(
          std::format("setsockopt(SO_REUSEPORT) failed (errno: {})", errno));
    }

    if (network_config.tcp_config.nodelay) {
      if (::setsockopt(listen_fd, IPPROTO_TCP, TCP_NODELAY, &yes_opt,
                       sizeof(yes_opt)) < 0) {
        ::close(listen_fd);
        return std::unexpected(
            std::format("setsockopt(TCP_NODELAY) failed (errno: {})", errno));
      }
    }

    if (network_config.tcp_config.keep_alive) {
      if (::setsockopt(listen_fd, SOL_SOCKET, SO_KEEPALIVE, &yes_opt,
                       sizeof(yes_opt)) < 0) {
        ::close(listen_fd);
        return std::unexpected(
            std::format("setsockopt(SO_KEEPALIVE) failed (errno: {})", errno));
      }

      if (::setsockopt(listen_fd, IPPROTO_TCP, TCP_KEEPIDLE,
                       &network_config.tcp_config.idle_time,
                       sizeof(network_config.tcp_config.idle_time)) < 0) {
        ::close(listen_fd);
        return std::unexpected(
            std::format("setsockopt(TCP_KEEPIDLE) failed (errno: {})", errno));
      }

      if (::setsockopt(listen_fd, IPPROTO_TCP, TCP_KEEPINTVL,
                       &network_config.tcp_config.keep_interval,
                       sizeof(network_config.tcp_config.keep_interval)) < 0) {
        ::close(listen_fd);
        return std::unexpected(
            std::format("setsockopt(TCP_KEEPINTVL) failed (errno: {})", errno));
      }

      if (::setsockopt(listen_fd, IPPROTO_TCP, TCP_KEEPCNT,
                       &network_config.tcp_config.keep_count,
                       sizeof(network_config.tcp_config.keep_count)) < 0) {
        ::close(listen_fd);
        return std::unexpected(
            std::format("setsockopt(TCP_KEEPCNT) failed (errno: {})", errno));
      }
    }

    auto endpoint = IpEndpoint::parse(config.address);
    if (!endpoint) {
      ::close(listen_fd);
      return std::unexpected("Error parse config address");
    }

    auto sockaddr = endpoint.value().to_sockaddr();

    if (::bind(listen_fd, reinterpret_cast<struct sockaddr *>(&sockaddr),
               endpoint.value().sockaddr_length()) < 0) {
      ::close(listen_fd);
      return std::unexpected(
          std::format("Bind socket failed (errno: {})", errno));
    }

    if (::listen(listen_fd, SOMAXCONN) < 0) {
      ::close(listen_fd);
      return std::unexpected(std::format("Listen failed (errno: {})", errno));
    }

    Listener2 listener;
    listener.ip_endpoint = endpoint.value();
    listener.is_raw = is_raw;
    listener.support_http1 = support_http1;
    listener.support_http2 = support_http2;
    listener.protocol = TransportProtocol::Tcp;
    listener.tls_path = config.tls;

    listener.name_upstream_pool = config.upstream_pool;
    listener.name_route = config.route;

    return listener;
  }

private:
  IpEndpoint ip_endpoint;
  int fd{-1};
  bool is_raw{false};
  bool support_http1{false};
  bool support_http2{false};
  bool is_quic{false};
  std::string tls_path{false};
  TransportProtocol protocol;
  std::string name_upstream_pool;
  std::string name_route;
  int redirect_to_port;
};
} // namespace Coring4
