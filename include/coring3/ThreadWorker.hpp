#pragma once

#include "SessionStorage.hpp"
#include "coring3/ComplectionKind.hpp"
#include "coring3/ProvidedBufferPool.hpp"
#include "coring3/Sessions.hpp"
#include "coring3/Stream.hpp"
#include "coring3/UserData.hpp"
#include "coring3/utility/TaggedPointer.hpp"
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <print>
#include <system_error>
#include <unordered_map>
#include <vector>
namespace Coring3 {
class ThreadWorker {
public:
  struct Config {
    uint32_t ring_entries{4096};
    size_t max_session{10000};
  };

  explicit ThreadWorker(uint32_t worker_id, Config config)
      : worker_id_(worker_id) {
    io_uring_params params{};
    params.flags = IORING_SETUP_SINGLE_ISSUER;

    int ret = io_uring_queue_init_params(config.ring_entries, &ring_, &params);
    if (ret < 0) {
      throw std::system_error(-ret, std::generic_category(),
                              "Failed to init io_uring");
    }
    rx_buf_pool.init(&ring_, rx_buf_pool.BGID_CLIENT_RX,
                     rx_buf_pool.NUM_BUFFERS);
  }

  ~ThreadWorker() { io_uring_queue_exit(&ring_); }

  ThreadWorker(const ThreadWorker &) = delete;
  ThreadWorker &operator=(const ThreadWorker &) = delete;

  void run() {
    running_ = true;
    std::println("[Worker {}] Event loop started", worker_id_);
    while (running_) {
      io_uring_cqe *cqe{nullptr};
      int ret = io_uring_submit_and_wait(&ring_, 1);

      if (ret < 0) {
        if (ret == -EINTR) {
          continue;
        }
        break;
      }

      unsigned head;
      unsigned count = 0;
      io_uring_for_each_cqe(&ring_, head, cqe) {
        ++count;
        process_cqe(cqe);
      }

      io_uring_cq_advance(&ring_, count);
    }
  }

  void stop() {
    running_ = false;
    io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
    if (sqe) {
      io_uring_prep_nop(sqe);
      sqe->user_data = 0;
      io_uring_submit(&ring_);
    }
  }

  void add_listener(int server_fd, uint16_t port) {
    listeners_[server_fd] = port;
  }

  void submit_accept(int listener_fd) {
    io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
    auto user_data = 0ULL; // Utility::TaggedPointer::pack(, U tag)
    io_uring_prep_accept(sqe, listener_fd, nullptr, nullptr, 0);
    sqe->accept_flags |= IORING_ACCEPT_MULTISHOT;
    sqe->user_data = MULTISHOT_ACCEPT_USER_DATA;
  }

  void process_cqe(io_uring_cqe *cqe) {
    const UserData ud = UserData::unpack(cqe->user_data);
    const auto op = static_cast<ComplectionKind>(ud.op_code);
    const SessionHandle handle{.index = ud.index, .generation = ud.generation};

    // switch (op) { case ComplectionKind::Accept: }
  }

  void on_accept(int client_fd) noexcept;
  void on_client_read(SessionHandle conn_handle,
                      ClientConnectionVariant &conn_var,
                      io_uring_cqe *cqe) noexcept;

  void arm_detecting_recv(SessionHandle conn_handle,
                          DetectionSession &conn_var);

  void promote_to_http1(SessionHandle conn_handle,
                        DetectionSession &detecting) noexcept;

  void promote_to_http2(SessionHandle conn_handle,
                        DetectionSession &detectiong) noexcept;

  void arm_client_recv_provided(SessionHandle handle, int fd) noexcept;

  void close_client_connection(SessionHandle handle,
                               ClientConnectionVariant &conn_var) noexcept;

  void process_http1_data(SessionHandle conn_handle,
                          Http1ClientSession &session,
                          std::span<const std::byte> rx_data) noexcept;

private:
  io_uring ring_;
  bool running_{false};
  uint32_t worker_id_{0};

  SessionStorage<ClientConnectionVariant, 10000> client_conns_;
  SessionStorage<UpstreamConnectionVariant, 100000> upstream_conns_;
  SessionStorage<StreamVariant, 50000> streams_;

  ProvidedBufferPool rx_buf_pool;
  // SessionStorage<Connection>
  std::unordered_map<int, uint16_t> listeners_;
  static constexpr uint64_t MULTISHOT_ACCEPT_USER_DATA = 0xDEADBEEF;
};
} // namespace Coring3