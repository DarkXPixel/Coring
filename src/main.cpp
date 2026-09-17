#include "coring4/ThreadWorker.hpp"
#include <atomic>
#include <condition_variable>
#include <csignal>
#include <print>

std::atomic_bool shutdown_requested{false};

void signal_handler(int signal) {
  shutdown_requested.store(true, std::memory_order_relaxed);
  shutdown_requested.notify_one();
}

int main(int argc, char *argv[]) {
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  auto th = Coring4::ThreadWorker::createAndStart();
  if (!th.has_value()) {
    std::println("{}", th.error());
    return -1;
  }
  shutdown_requested.wait(false);

  th->get()->stop();

  return 0;
}