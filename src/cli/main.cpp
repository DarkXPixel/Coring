#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cout << "Usage: proxyctl <command>\n";
    return 1;
  }

  std::string command = argv[1];

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);

  if (fd < 0) {
    std::cerr << "Socket creation failed\n";
    return 1;
  }

  sockaddr_un address{};

  address.sun_family = AF_UNIX;

  std::strncpy(address.sun_path, "/run/coring/control.sock",
               sizeof(address.sun_path) - 1);

  if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) <
      0) {
    std::cerr << "Cannot connect to proxy\n" << "errno:" << errno << "\n";
    close(fd);
    return 1;
  }

  send(fd, command.data(), command.size(), MSG_NOSIGNAL);

  char buffer[256]{};

  auto bytes = recv(fd, buffer, sizeof(buffer) - 1, 0);

  if (bytes > 0)
    std::cout << std::string(buffer, bytes) << '\n';

  close(fd);
}
