#pragma once

#include "coring4/IoUring/IoUringBackend.hpp"
#include "coring4/ManageService.hpp"
#include "coring4/core/Types.hpp"
#include "coring4/io/IoOp.hpp"
#include "coring4/io/IoOpCompletion.hpp"
#include <expected>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <string>
#include <sys/eventfd.h>
#include <variant>
namespace Coring4 {
class Worker {
public:
  static std::expected<Worker, std::string> create(WorkerId id) {
    Worker result;
    result.worker_id_ = id;
    if (auto backend = IOUringBackend::create(); backend) {
      result.backend_ = std::move(backend.value());
    } else {
      return std::unexpected(backend.error());
    }
    return result;
  }

  [[maybe_unused]] std::shared_ptr<Manage::WorkerChannel>
  create_or_get_worker_channel() {
    if (channel_) {
      return channel_;
    }
    channel_ = std::make_shared<Manage::WorkerChannel>();
    channel_->ev_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    return channel_;
  }

  void init_on_thread() noexcept {
    if (channel_ == nullptr) {
      return;
    }

    backend_.prep_multishot_poll(io_ops_.allocate_and_construct(IoOp{
        .fd = channel_->ev_fd,
        .op_code = OpCode::Manage,
    }));
  }

  void on_manage(IoCompletion completion) noexcept {
    uint64_t val;
    ::read(completion.op->fd, &val, sizeof(val));

    auto requests = channel_->pop_all();
    for (auto &req : requests) {
      std::visit([this](auto &&arg) {}, req.request);
    }
  }

  void on_io(IoCompletion completion) {
    switch (completion.op->op_code) {
    case OpCode::Accept:
    case OpCode::Send:
    case OpCode::Recv:
    case OpCode::StopSignal:
    case OpCode::Close:
    case OpCode::Cancel:
    case OpCode::Connect:
    case OpCode::Read:
    case OpCode::Write:
    case OpCode::Manage:
      break;
    case OpCode::Poll:
      break;
    }
  }

  void run() noexcept {
    if (channel_ == nullptr) {
      return;
    }

    is_running_ = true;

    auto callback = [this](IoCompletion completion) {
      this->on_io(completion);
    };

    while (is_running_) {
      backend_.loop(callback);
    }
  }

private:
  WorkerId worker_id_;
  IOUringBackend backend_;

  std::shared_ptr<Manage::WorkerChannel> channel_;
  IoOpPool io_ops_;
  uint64_t buf_for_signal{};
  bool is_running_{false};
};
} // namespace Coring4