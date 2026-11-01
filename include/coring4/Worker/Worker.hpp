#pragma once

#include "coring4/BufferHandle.hpp"
#include "coring4/BufferPool.hpp"
#include "coring4/IoUring/IoUringBackend.hpp"
#include "coring4/IoUring/IoUringBackendConfig.hpp"
#include "coring4/ManageService.hpp"
#include "coring4/PoolType.hpp"
#include "coring4/ProvidedBufferPool.hpp"
#include "coring4/core/Types.hpp"
#include "coring4/io/IoOp.hpp"
#include "coring4/io/IoOpCompletion.hpp"
#include "coring4/listeners/Listener.hpp"
#include "coring4/routing/WorkerRouting.hpp"
#include "coring4/storage/ObjectPool.hpp"
#include <algorithm>
#include <expected>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <memory>
#include <print>
#include <string>
#include <sys/eventfd.h>
#include <variant>
#include <vector>
namespace Coring4 {
class Worker {
public:
  static std::expected<Worker, std::string> create(WorkerId id) {
    Worker result;
    result.worker_id_ = id;
    IoUringBackendConfig iouring_config{};
    if (auto backend = IOUringBackend::create(iouring_config); backend) {
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
    tcp_provided_buffer_pool.init(backend_.get_uring(), 0, 1024);
    for (int i = 0; i < 1024; ++i) {
      BufferHandle16 handle;
      tcp_provided_buffer_pool.recycle_buffer(
          tcp_buffer_pool.allocate()->data(), i);
    }

    tcp_provided_buffer_pool.advanace();
    if (channel_ == nullptr) {
      return;
    }

    backend_.prep_multishot_poll(io_ops_.allocate_and_construct(IoOp{
        .fd = channel_->ev_fd,
        .op_code = OpCode::Manage,
    }));
  }

  void add_listener(Listener2 listener) {
    if (listener.get_transport() == TransportProtocol::Tcp) {
      backend_.prep_multishot_accept(io_ops_.allocate_and_construct(
          IoOp{.fd = listener.get_fd(), .op_code = OpCode::Accept}));
    } else if (listener.get_transport() == TransportProtocol::Udp) {
    }
  }

  void on_manage(IoCompletion completion) noexcept {
    uint64_t val;
    ::read(completion.op->fd, &val, sizeof(val));

    auto requests = channel_->pop_all();
    for (auto &req : requests) {
      std::visit(
          [this](auto &&arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, Manage::UpdateRoutesRequest>) {
              // Same thread as every route lookup: a plain swap, no locking.
              auto res = routing_.install(std::move(arg.table));
              if (!res) {
                std::println("worker {}: route update rejected: {}",
                             worker_id_.id, res.error());
              }
              if (arg.ack) {
                arg.ack->complete(res);
              }
            }
          },
          req.request);
    }

    val = 0;
  }

  void on_accept(IoCompletion completion) noexcept {
    auto it =
        std::ranges::find(listeners_, completion.op->fd, &Listener2::get_fd);
    if (it == listeners_.end()) {
      return;
    }
  }

  void on_io(IoCompletion completion) {
    switch (completion.op->op_code) {
    case OpCode::Accept:
      on_accept(completion);
      break;
    case OpCode::Send:
    case OpCode::Recv:
    case OpCode::StopSignal:
    case OpCode::Close:
    case OpCode::Cancel:
    case OpCode::Connect:
    case OpCode::Read:
    case OpCode::Write:
    case OpCode::Manage:
      on_manage(completion);
      break;
    case OpCode::Poll:
      break;
    }
    if (!completion.more()) {
      io_ops_.deallocate(completion.op);
    }
  }

  void run() noexcept {
    init_on_thread();

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

  WorkerRouting routing_;
  std::shared_ptr<const class UpstreamRules> upstream_rules_;

  std::shared_ptr<Manage::WorkerChannel> channel_;
  IoOpPool io_ops_;
  ObjectPool<std::array<std::byte, 16384>, 1024> tcp_buffer_pool;
  ProvidedBufferPool<> tcp_provided_buffer_pool;
  uint64_t buf_for_signal{};
  bool is_running_{false};
  std::vector<Listener2> listeners_;
};
} // namespace Coring4
