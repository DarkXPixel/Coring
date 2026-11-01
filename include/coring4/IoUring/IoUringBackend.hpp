#pragma once

#include "IoUringBackendConfig.hpp"
#include "coring4/core/Types.hpp"
#include "coring4/io/IoOp.hpp"
#include "coring4/io/IoOpCompletion.hpp"
#include <bit>
#include <cerrno>
#include <expected>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <liburing/io_uring/query.h>
#include <optional>
#include <poll.h>
#include <string>
#include <utility>
namespace Coring4 {
class IOUringBackend {
public:
  IOUringBackend() = default;
  IOUringBackend(IOUringBackend &&other) noexcept
      : is_initialized(std::exchange(other.is_initialized, false)),
        uring_(other.uring_) {}

  IOUringBackend &operator=(IOUringBackend &&other) noexcept {
    is_initialized = std::exchange(other.is_initialized, false);
    uring_ = other.uring_;
    return *this;
  }

  io_uring *get_uring() noexcept { return &uring_; }

  static std::expected<IOUringBackend, std::string>
  create(const IoUringBackendConfig &config) {
    IOUringBackend result;
    io_uring_params params{};
    if (!std::has_single_bit(config.cq_entries) ||
        !std::has_single_bit(config.sq_entries)) {
      return std::unexpected(
          "Invalid io_uring config. cq_entries and sq_entries must be pow2");
    }

    if (config.single_issuer) {
      params.flags |= IORING_SETUP_SINGLE_ISSUER;
    }
    params.flags |= IORING_SETUP_CQSIZE;
    params.cq_entries = config.cq_entries;

    auto ret =
        io_uring_queue_init_params(config.sq_entries, &result.uring_, &params);

    if (ret < 0) {
      return std::unexpected("Failed to init io_uring");
    }
    result.is_initialized = true;
    result.config = config;
    return result;
  }

  void prep_read(IoOp *op) noexcept {
    io_uring_sqe *sqe = get_sqe();
    io_uring_prep_read(sqe, op->fd, op->buffer_handle.ptr, op->size,
                       op->offset);
    io_uring_sqe_set_data(sqe, op);
  }

  void prep_multishot_accept(IoOp *op) noexcept {
    io_uring_sqe *sqe = get_sqe();
    io_uring_prep_multishot_accept(sqe, op->fd, nullptr, nullptr,
                                   SOCK_CLOEXEC | SOCK_NONBLOCK);
    io_uring_sqe_set_data(sqe, op);
  }

  void prep_multishot_poll(IoOp *op) noexcept {
    io_uring_sqe *sqe = get_sqe();
    io_uring_prep_poll_multishot(sqe, op->fd, POLLIN);
    io_uring_sqe_set_data(sqe, op);
  }

  template <typename Func>
  std::optional<std::string> loop(Func &&callback) noexcept {
    io_uring_cqe *cqe{nullptr};
    int ret = io_uring_submit_and_wait(&uring_, 1);

    if (ret < 0) {
      if (ret == -EINTR) {
        return std::nullopt;
      }
      return std::string("Error");
    }

    unsigned head;
    unsigned count = 0;

    io_uring_for_each_cqe(&uring_, head, cqe) {
      ++count;
      IoCompletion completion{
          .op = static_cast<IoOp *>(io_uring_cqe_get_data(cqe)),
          .result = cqe->res,
          .flags = cqe->flags,
      };
      callback(completion);
    }

    io_uring_cq_advance(&uring_, count);

    return std::nullopt;
  }

private:
  io_uring_sqe *get_sqe() noexcept { return io_uring_get_sqe(&uring_); }
  io_uring uring_;
  bool is_initialized{false};
  IoUringBackendConfig config;
};
} // namespace Coring4
