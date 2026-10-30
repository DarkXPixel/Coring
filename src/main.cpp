#include "coring4/ManageService.hpp"
#include "coring4/Worker/Worker.hpp"
#include "coring4/io/IoOp.hpp"
#include "coring4/listeners/Listener.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <memory>
#include <print>
#include <thread>

std::atomic_bool shutdown_requested{false};

void signal_handler(int signal) {}

int main(int argc, char *argv[]) {
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  auto worker = Coring4::Worker::create(Coring4::WorkerId(0));
  if (!worker) {
    std::println("{}", worker.error());
  }

  std::shared_ptr<Coring4::Manage::WorkerChannel> channel =
      worker.value().create_or_get_worker_channel();

  std::jthread th([&channel]() {
    std::this_thread::sleep_for(std::chrono::seconds(10));
    channel->push(Coring4::Manage::ManageRequest{
        .request = Coring4::Manage::AddListenerRequest{}});
  });

  worker.value().run();

  return 0;
}
