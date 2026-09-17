#pragma once

#include "coring4/Engine.hpp"
#include <condition_variable>
#include <expected>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <memory>
#include <print>
#include <semaphore>
#include <stop_token>
#include <thread>
#include <utility>
namespace Coring4 {
class ThreadWorker {
private:
  std::jthread worker_thread_;
  bool running_{true};
  std::unique_ptr<Engine> engine_;

  ThreadWorker() = default;

public:
  static std::expected<std::unique_ptr<ThreadWorker>, std::string>
  createAndStart() {
    struct Enabler : public ThreadWorker {};
    auto worker = std::make_unique<Enabler>();
    auto mb_engine = Engine::create();
    if (!mb_engine) {
      return std::unexpected("Create engine failed");
    }

    worker->engine_ = std::move(*mb_engine);

    if (auto res = worker->start(); !res) {
      return std::unexpected(res.error());
    }

    return worker;
  }

  ~ThreadWorker() { stop(); }

  void stop() {
    if (running_) {
      worker_thread_.request_stop();
    }
  }

private:
  void run(const std::stop_token &stoken) {
    engine_->run(stoken);
    running_ = false;
  }

  std::expected<void, std::string> start() {
    try {
      worker_thread_ = std::jthread(
          [this](const std::stop_token &stoken) { this->run(stoken); });
    } catch (...) {
      return std::unexpected("Thread start failed");
    }
    return {};
  }
};
} // namespace Coring4
