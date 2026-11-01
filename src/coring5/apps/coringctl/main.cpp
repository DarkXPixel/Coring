// coringctl: talks to coringd's control socket.
//   coringctl [--socket PATH] ping|reload|stats|status|shutdown

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int main(int argc, char **argv) {
  std::string path = "/run/coring/control.sock";
  std::string command;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "--socket" && i + 1 < argc) {
      path = argv[++i];
    } else if (command.empty()) {
      command = a;
    } else {
      command.clear();
      break;
    }
  }
  if (command.empty()) {
    std::fputs(
        "usage: coringctl [--socket PATH] ping|reload|stats|status|shutdown\n",
        stderr);
    return 2;
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) {
    std::fputs("socket path too long\n", stderr);
    return 2;
  }
  std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);

  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0 ||
      ::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    std::perror(("coringctl: connect " + path).c_str());
    return 1;
  }
  command.push_back('\n');
  if (::send(fd, command.data(), command.size(), MSG_NOSIGNAL) < 0) {
    std::perror("coringctl: send");
    return 1;
  }
  char buf[4096];
  std::string response;
  ssize_t n = 0;
  while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0) {
    response.append(buf, static_cast<std::size_t>(n));
  }
  std::fputs(response.c_str(), stdout);
  ::close(fd);
  return response.starts_with("ERR") ? 1 : 0;
}
