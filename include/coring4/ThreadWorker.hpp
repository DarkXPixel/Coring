#pragma once

#include "coring4/Engine.hpp"
#include <condition_variable>
#include <expected>
#include <future>
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
  std::unique_ptr<Engine> engine_;
  std::jthread worker_thread_;

  ThreadWorker() = default;

public:
  static std::expected<std::unique_ptr<ThreadWorker>, std::string>
  createAndStart() {
    struct Enabler : public ThreadWorker {};
    auto worker = std::make_unique<Enabler>();

    std::promise<std::expected<void, std::string>> init_promise;
    auto init_future = init_promise.get_future();

    if (auto res = worker->start(std::move(init_promise)); !res) {
      return std::unexpected(res.error());
    }

    auto init_result = init_future.get();
    if (!init_result) {
      return std::unexpected(init_result.error());
    }

    return worker;
  }

  ~ThreadWorker() { stop(); }

  void stop() { worker_thread_.request_stop(); }

private:
  void run(std::stop_token stoken) { engine_->run(stoken); }

  std::expected<void, std::string>
  start(std::promise<std::expected<void, std::string>> init_promise) {
    try {
      worker_thread_ = std::jthread(
          [this](const std::stop_token &stoken,
                 std::promise<std::expected<void, std::string>> init_promise) {
            auto mb_engine = Engine::create();
            if (!mb_engine) {
              std::println("Create engine failed");
              init_promise.set_value(std::unexpected(mb_engine.error()));
              return;
            }

            this->engine_ = std::move(*mb_engine);
            init_promise.set_value({});
            this->run(stoken);
          },
          std::move(init_promise));
    } catch (...) {
      return std::unexpected("Thread start failed");
    }
    return {};
  }
};
} // namespace Coring4
