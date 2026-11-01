#include "coring4/ManageService.hpp"
#include "coring4/Worker/Worker.hpp"
#include "coring4/application/ControlServer.hpp"
#include "coring4/application/CoringDaemon.hpp"
#include "coring4/io/IoOp.hpp"
#include "coring4/listeners/Listener.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <memory>
#include <print>
#include <thread>
#include <vector>

volatile std::sig_atomic_t shutdown_requested = 0;

void signal_handler(int signal) {
  if (signal == SIGTERM || signal == SIGINT) {
    shutdown_requested = 1;
  }
}

using namespace std::chrono_literals;

struct PlaneData {
  std::jthread th;
  std::shared_ptr<Coring4::Manage::WorkerChannel> worker_channel;
};

int main(int argc, char *argv[]) {
  std::signal(SIGINT, &signal_handler);
  std::signal(SIGTERM, &signal_handler);

  Coring4::Control::CoringDaemon daemon;
  Coring4::Control::CoringServer control(daemon, "/run/coring/control.sock");

  daemon.start();
  control.start();

  while ((shutdown_requested == 0) && daemon.is_running()) {
    std::this_thread::sleep_for(500ms);
  }

  daemon.stop();
  control.stop();

  // std::signal(SIGINT, signal_handler);
  // std::signal(SIGTERM, signal_handler);

  // std::shared_ptr<class RouteTable> routing_;
  // std::shared_ptr<class UpstreamRules> upstream_rules_;

  // std::vector<PlaneData> threads;

  // auto worker = Coring4::Worker::create(Coring4::WorkerId(0));
  // if (!worker) {
  //   std::println("{}", worker.error());
  // }

  // std::shared_ptr<Coring4::Manage::WorkerChannel> channel =
  //     worker.value().create_or_get_worker_channel();

  // std::jthread th([&channel]() {
  //   std::this_thread::sleep_for(std::chrono::seconds(10));
  //   channel->push(Coring4::Manage::ManageRequest{
  //       .request = Coring4::Manage::AddListenerRequest{}});
  // });

  // worker.value().run();

  return 0;
}
