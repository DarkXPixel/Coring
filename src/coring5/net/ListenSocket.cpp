#include "coring5/net/ListenSocket.hpp"

#include <cerrno>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

namespace Coring5 {

namespace {
bool set_opt(int fd, int level, int name, int value) noexcept {
  return ::setsockopt(fd, level, name, &value, sizeof(value)) == 0;
}
} // namespace

Result<FdOwner> create_tcp_listener(const Endpoint &ep,
                                    const ListenOptions &opts) {
  FdOwner fd(
      ::socket(ep.family(), SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
  if (!fd) {
    return unexpected_error(Errc::Internal, errno);
  }
  if (opts.reuse_addr && !set_opt(fd.get(), SOL_SOCKET, SO_REUSEADDR, 1)) {
    return unexpected_error(Errc::Internal, errno);
  }
  if (opts.reuse_port && !set_opt(fd.get(), SOL_SOCKET, SO_REUSEPORT, 1)) {
    return unexpected_error(Errc::Internal, errno);
  }
  if (ep.family() == AF_INET6 &&
      !set_opt(fd.get(), IPPROTO_IPV6, IPV6_V6ONLY, opts.v6only ? 1 : 0)) {
    return unexpected_error(Errc::Internal, errno);
  }
  if (opts.tcp_nodelay && !set_opt(fd.get(), IPPROTO_TCP, TCP_NODELAY, 1)) {
    return unexpected_error(Errc::Internal, errno);
  }
  if (::bind(fd.get(), ep.sa(), ep.len) != 0) {
    return unexpected_error(Errc::InvalidArgument, errno);
  }
  if (::listen(fd.get(), opts.backlog) != 0) {
    return unexpected_error(Errc::Internal, errno);
  }
  return fd;
}

Result<Endpoint> local_endpoint(int fd) {
  Endpoint ep;
  ep.len = sizeof(ep.addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr *>(&ep.addr), &ep.len) != 0) {
    return unexpected_error(Errc::Internal, errno);
  }
  return ep;
}

} // namespace Coring5
