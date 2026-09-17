#pragma once

#include <chrono>
#include <expected>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <memory>
#include <print>
#include <stop_token>
#include <string_view>
#include <thread>
namespace Coring4 {
class Engine {
private:
  static constexpr auto RING_ENTRIES_COUNT = 4096;

  io_uring ring_;
  bool is_initialized_{false};

public:
  Engine() = default;
  Engine(const Engine &) = delete;
  Engine &operator=(const Engine &) = delete;
  Engine(Engine &&other_) = delete;
  Engine &operator=(Engine &&) = delete;

  ~Engine() { cleanup(); }

  static std::expected<std::unique_ptr<Engine>, std::string_view> create() {
    auto engine = std::make_unique<Engine>();
    io_uring_params params{};
    params.flags |= IORING_SETUP_SINGLE_ISSUER;

    auto ret =
        io_uring_queue_init_params(RING_ENTRIES_COUNT, &engine->ring_, &params);

    if (ret < 0) {
      return std::unexpected("Failed to init io_uring");
    }
    engine->is_initialized_ = true;
    return engine;
  }

  void run(const std::stop_token &stoken) {
    while (!stoken.stop_requested()) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      std::println("1 sec");
    }
  }

private:
  void cleanup() noexcept {
    if (is_initialized_) {
      io_uring_queue_exit(&ring_);
      is_initialized_ = false;
    }
  }
  void add_listener(int port) {}
};
} // namespace Coring4