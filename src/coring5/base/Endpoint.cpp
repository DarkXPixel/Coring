#include "coring5/base/Endpoint.hpp"

#include <arpa/inet.h>
#include <charconv>
#include <cstring>
#include <netinet/in.h>

namespace Coring5 {

Result<Endpoint> Endpoint::parse(std::string_view text) {
  std::string_view host;
  std::string_view port_text;
  bool bracketed = false;

  if (text.starts_with('[')) {
    bracketed = true;
    const auto close = text.find(']');
    if (close == std::string_view::npos || close + 1 >= text.size() ||
        text[close + 1] != ':') {
      return unexpected_error(Errc::InvalidArgument);
    }
    host = text.substr(1, close - 1);
    port_text = text.substr(close + 2);
  } else {
    const auto colon = text.rfind(':');
    if (colon == std::string_view::npos) {
      return unexpected_error(Errc::InvalidArgument);
    }
    host = text.substr(0, colon);
    port_text = text.substr(colon + 1);
  }

  unsigned port = 0;
  const auto *first = port_text.data();
  const auto *last = first + port_text.size();
  const auto [ptr, ec] = std::from_chars(first, last, port);
  if (ec != std::errc{} || ptr != last || port_text.empty() || port > 65535) {
    return unexpected_error(Errc::InvalidArgument);
  }

  // Unbracketed hosts must be IPv4 (or empty = wildcard): "::1:80" is ambiguous
  // and rejected. Bracketed hosts must be a non-empty IPv6 literal.
  if (bracketed ? host.empty() : host.find(':') != std::string_view::npos) {
    return unexpected_error(Errc::InvalidArgument);
  }

  Endpoint ep;
  const std::string host_z(host);
  if (!bracketed) {
    auto *in = reinterpret_cast<sockaddr_in *>(&ep.addr);
    in->sin_family = AF_INET;
    in->sin_port = htons(static_cast<uint16_t>(port));
    if (host.empty()) {
      in->sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (::inet_pton(AF_INET, host_z.c_str(), &in->sin_addr) != 1) {
      return unexpected_error(Errc::InvalidArgument);
    }
    ep.len = sizeof(sockaddr_in);
  } else {
    auto *in6 = reinterpret_cast<sockaddr_in6 *>(&ep.addr);
    in6->sin6_family = AF_INET6;
    in6->sin6_port = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET6, host_z.c_str(), &in6->sin6_addr) != 1) {
      return unexpected_error(Errc::InvalidArgument);
    }
    ep.len = sizeof(sockaddr_in6);
  }
  return ep;
}

bool Endpoint::operator==(const Endpoint &o) const noexcept {
  return len == o.len && std::memcmp(&addr, &o.addr, len) == 0;
}

uint16_t Endpoint::port() const noexcept {
  if (addr.ss_family == AF_INET) {
    return ntohs(reinterpret_cast<const sockaddr_in *>(&addr)->sin_port);
  }
  if (addr.ss_family == AF_INET6) {
    return ntohs(reinterpret_cast<const sockaddr_in6 *>(&addr)->sin6_port);
  }
  return 0;
}

std::string Endpoint::to_string() const {
  char buf[INET6_ADDRSTRLEN]{};
  if (addr.ss_family == AF_INET) {
    ::inet_ntop(AF_INET,
                &reinterpret_cast<const sockaddr_in *>(&addr)->sin_addr, buf,
                sizeof(buf));
    return std::string(buf) + ":" + std::to_string(port());
  }
  if (addr.ss_family == AF_INET6) {
    ::inet_ntop(AF_INET6,
                &reinterpret_cast<const sockaddr_in6 *>(&addr)->sin6_addr, buf,
                sizeof(buf));
    return "[" + std::string(buf) + "]:" + std::to_string(port());
  }
  return "<unspec>";
}

} // namespace Coring5
