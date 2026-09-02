#pragma once

#include <cerrno>
#include <cstdint>
#include <expected>
#include <format>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <string>

namespace Coring2 {
class UringEngine2 {
  static constexpr uint32_t QUEUE_DEPTH = 4096;

  io_uring ring_;

public:
  static std::expected<UringEngine2, std::string> create() {
    io_uring_params params{};

    UringEngine2 engine;
    int res{0};
    if ((res = io_uring_queue_init_params(QUEUE_DEPTH, &engine.ring_,
                                          &params)) < 0) {
      return std::unexpected(std::format("Failed to init io_uring: {}", res));
    }
    return engine;
  }

  void run() {
    std::array<io_uring_cqe *, 128> cqes;

    while (true) {
      int ret = io_uring_submit_and_wait(&ring_, 1);
      if (ret < 0 && ret != -EINTR) {
        break;
      }

      uint32_t count =
          io_uring_peek_batch_cqe(&ring_, cqes.data(), cqes.size());

      for (int i = 0; i < count; ++i) {
        io_uring_cqe *cqe = cqes[i];
        // auto* ctx =
      }
    }
  }
};
} // namespace Coring2