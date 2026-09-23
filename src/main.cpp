#include "coring4/Listener.hpp"
#include "coring4/ManageService.hpp"
#include "coring4/ThreadWorker.hpp"
#include <atomic>
#include <condition_variable>
#include <csignal>
#include <memory>
#include <print>

std::atomic_bool shutdown_requested{false};

void signal_handler(int signal) {
  shutdown_requested.store(true, std::memory_order_relaxed);
  shutdown_requested.notify_one();
}

int main(int argc, char *argv[]) {
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  Coring4::Manage::ManageService manage;

  std::array<std::unique_ptr<Coring4::ThreadWorker>, 3> threads;
  uint32_t th_c = 0;
  for (auto &i : threads) {
    auto th = Coring4::ThreadWorker::createAndStart(th_c);
    th_c += 2;
    if (!th.has_value()) {
      return -1;
    }
    manage.add_channel((*th)->get_channel());
    i = std::move(*th);
  }
  // auto th1 = Coring4::ThreadWorker::createAndStart();
  // if (!th1.has_value()) {
  //   std::println("{}", th.error());
  //   return -1;
  // }

  manage.send_request(Coring4::Manage::AddListenerRequest{
      .port = 8080,
      .protocol = Coring4::Listener::Protocol::Tcp,
      .version = Coring4::Listener::IpVersion::All});

  manage.send_request(Coring4::Manage::AddListenerRequest{
      .port = 8088,
      .protocol = Coring4::Listener::Protocol::Tcp,
      .version = Coring4::Listener::IpVersion::All});

  shutdown_requested.wait(false);

  // th->get()->stop();

  return 0;
}