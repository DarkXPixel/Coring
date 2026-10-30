#pragma once

#include <arpa/inet.h>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <sys/socket.h>
#include <system_error>
#include <variant>
namespace Coring4 {
class IpEndpoint {
public:
  using IPv4 = std::array<std::uint8_t, 4>;
  using IPv6 = std::array<std::uint8_t, 16>;

  enum class Family : std::uint8_t { IPv4, IPv6 };

private:
  std::variant<IPv4, IPv6> address_;
  std::uint16_t port_ = 0;

public:
  IpEndpoint() = default;

  IpEndpoint(const IPv4 &ip, std::uint16_t port) : address_(ip), port_(port) {}

  IpEndpoint(const IPv6 &ip, std::uint16_t port) : address_(ip), port_(port) {}

  static std::optional<IpEndpoint> parse(std::string_view str) {
    std::string_view ip;
    std::string_view port_str;

    if (str.empty())
      return std::nullopt;

    if (str.front() == '[') {
      const auto close = str.find(']');

      if (close == std::string_view::npos || close + 1 >= str.size() ||
          str[close + 1] != ':')
        return std::nullopt;

      ip = str.substr(1, close - 1);
      port_str = str.substr(close + 2);
    } else {
      const auto colon = str.rfind(':');

      if (colon == std::string_view::npos) {
        return std::nullopt;
      }

      if (str.find(':') != colon) {
        return std::nullopt;
      }

      ip = str.substr(0, colon);
      port_str = str.substr(colon + 1);
    }

    if (ip.empty() || port_str.empty()) {
      return std::nullopt;
    }

    unsigned int port = 0;

    auto [ptr, ec] = std::from_chars(port_str.data(),
                                     port_str.data() + port_str.size(), port);

    if (ec != std::errc{} || ptr != port_str.data() + port_str.size() ||
        port > 65535) {
      return std::nullopt;
    }

    const std::string ip_str(ip);

    IPv4 ipv4{};
    if (inet_pton(AF_INET, ip_str.c_str(), ipv4.data()) == 1) {
      return IpEndpoint(ipv4, static_cast<std::uint16_t>(port));
    }

    IPv6 ipv6{};
    if (inet_pton(AF_INET6, ip_str.c_str(), ipv6.data()) == 1) {
      return IpEndpoint(ipv6, static_cast<std::uint16_t>(port));
    }

    return std::nullopt;
  }

  [[nodiscard]]
  Family family() const noexcept {
    return std::holds_alternative<IPv4>(address_) ? Family::IPv4 : Family::IPv6;
  }

  [[nodiscard]]
  std::uint16_t port() const noexcept {
    return port_;
  }

  void set_port(std::uint16_t port) noexcept { port_ = port; }

  [[nodiscard]]
  const std::variant<IPv4, IPv6> &address() const noexcept {
    return address_;
  }

  [[nodiscard]]
  bool is_ipv4() const noexcept {
    return family() == Family::IPv4;
  }

  [[nodiscard]]
  bool is_ipv6() const noexcept {
    return family() == Family::IPv6;
  }

  [[nodiscard]]
  std::string ip_string() const {
    std::array<char, INET6_ADDRSTRLEN> buffer{};

    const int af = is_ipv4() ? AF_INET : AF_INET6;

    const void *addr =
        is_ipv4() ? static_cast<const void *>(std::get<IPv4>(address_).data())
                  : static_cast<const void *>(std::get<IPv6>(address_).data());

    if (inet_ntop(af, addr, buffer.data(),
                  static_cast<socklen_t>(buffer.size())) == nullptr) {
      return {};
    }

    return buffer.data();
  }

  [[nodiscard]]
  std::string to_string() const {
    if (is_ipv6()) {
      return "[" + ip_string() + "]:" + std::to_string(port_);
    }

    return ip_string() + ":" + std::to_string(port_);
  }

  [[nodiscard]]
  sockaddr_storage to_sockaddr() const noexcept {
    sockaddr_storage storage{};

    if (is_ipv4()) {
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(port_);

      std::memcpy(&addr.sin_addr, std::get<IPv4>(address_).data(),
                  sizeof(addr.sin_addr));

      std::memcpy(&storage, &addr, sizeof(addr));
    } else {
      sockaddr_in6 addr{};
      addr.sin6_family = AF_INET6;
      addr.sin6_port = htons(port_);

      std::memcpy(&addr.sin6_addr, std::get<IPv6>(address_).data(),
                  sizeof(addr.sin6_addr));

      std::memcpy(&storage, &addr, sizeof(addr));
    }

    return storage;
  }

  [[nodiscard]]
  socklen_t sockaddr_length() const noexcept {
    return is_ipv4() ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
  }

  static std::optional<IpEndpoint> from_sockaddr(const sockaddr *addr,
                                                 socklen_t length) noexcept {
    if (addr == nullptr) {
      return std::nullopt;
    }

    if (addr->sa_family == AF_INET) {
      if (length < sizeof(sockaddr_in)) {
        return std::nullopt;
      }

      const auto *a = reinterpret_cast<const sockaddr_in *>(addr);

      IPv4 ip{};
      std::memcpy(ip.data(), &a->sin_addr, ip.size());

      return IpEndpoint(ip, ntohs(a->sin_port));
    }

    if (addr->sa_family == AF_INET6) {
      if (length < sizeof(sockaddr_in6)) {
        return std::nullopt;
      }

      const auto *a = reinterpret_cast<const sockaddr_in6 *>(addr);

      IPv6 ip{};
      std::memcpy(ip.data(), &a->sin6_addr, ip.size());

      return IpEndpoint(ip, ntohs(a->sin6_port));
    }

    return std::nullopt;
  }

  bool operator==(const IpEndpoint &) const = default;
};
} // namespace Coring4
