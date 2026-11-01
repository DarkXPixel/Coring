// coringd: the Coring5 daemon.
//
//   coringd --config /etc/coring/coring.toml [--control
//   /run/coring/control.sock]
//           [--check-config]
//
// Signals (handled by this thread only, through signalfd; workers block all):
//   SIGTERM/SIGINT  graceful shutdown (drain)
//   SIGHUP          reload configuration

#include "coring5/runtime/Daemon.hpp"
#include <csignal>
#include <cstdio>
#include <cstring>
#include <poll.h>
#include <string_view>
#include <sys/signalfd.h>
#include <unistd.h>

namespace {

void usage() {
  std::fputs(
      "usage: coringd --config FILE [--control SOCKET] [--check-config]\n",
      stderr);
}

} // namespace

int main(int argc, char **argv) {
  Coring5::DaemonOptions opts;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "--config" && i + 1 < argc) {
      opts.config_path = argv[++i];
    } else if (a == "--control" && i + 1 < argc) {
      opts.control_socket = argv[++i];
    } else if (a == "--check-config") {
      opts.check_only = true;
    } else {
      usage();
      return 2;
    }
  }
  if (opts.config_path.empty()) {
    usage();
    return 2;
  }

  // Block before any thread is created so every worker inherits the mask.
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGTERM);
  sigaddset(&set, SIGHUP);
  ::pthread_sigmask(SIG_BLOCK, &set, nullptr);
  std::signal(SIGPIPE, SIG_IGN);
  const int sfd = ::signalfd(-1, &set, SFD_CLOEXEC);
  if (sfd < 0) {
    std::perror("signalfd");
    return 1;
  }

  Coring5::Daemon daemon(opts);
  if (auto r = daemon.start(); !r) {
    std::fprintf(stderr, "coringd: %s\n", r.error().c_str());
    return 1;
  }
  if (opts.check_only) {
    std::puts("configuration OK");
    return 0;
  }
  std::fprintf(stderr, "coringd: %s\n", daemon.status().c_str());

  while (!daemon.stop_requested()) {
    pollfd p{sfd, POLLIN, 0};
    if (::poll(&p, 1, 200) <= 0) {
      continue; // timeout also lets a control-socket shutdown be noticed
    }
    signalfd_siginfo si{};
    if (::read(sfd, &si, sizeof(si)) != static_cast<ssize_t>(sizeof(si))) {
      continue;
    }
    if (si.ssi_signo == SIGHUP) {
      std::fprintf(stderr, "coringd: reload: %s\n", daemon.reload().c_str());
    } else {
      break;
    }
  }

  std::fputs("coringd: shutting down\n", stderr);
  daemon.stop();
  return 0;
}
