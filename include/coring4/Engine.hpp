#pragma once

#include "coring4/BufferHandle.hpp"
#include "coring4/BufferPool.hpp"
#include "coring4/EgressQueue.hpp"
#include "coring4/Listener.hpp"
#include "coring4/ManageService.hpp"
#include "coring4/PoolType.hpp"
#include "coring4/ProvidedBufferPool.hpp"
#include "coring4/SessionStorage.hpp"
#include "coring4/Sessions.hpp"
#include "coring4/ThreadLocalSlabPool.hpp"
#include <algorithm>
#include <array>
#include <asm-generic/socket.h>
#include <atomic>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <memory>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <print>
#include <stop_token>
#include <string_view>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <thread>

#include <utility>
#include <variant>
#include <vector>
namespace Coring4 {
class Engine {
private:
  static constexpr auto RING_ENTRIES_COUNT = 4096;

  io_uring ring_;
  bool is_initialized_{false};

  std::vector<Listener> listeners_;

  SessionStorage<ClientConnectionVariant> connections_;

  LocalBufferPool<> default_pool_16kb{PoolType::DefaultPool16KB, 8192};

  ProvidedBufferPool<> provided_ring_pool_;
  ThreadLocalSlabPool<EgressQueue, 8192> egress_pool;
  ThreadLocalSlabPool<StackRingBuffer<PendingChunk, 32>, 16> ring_chunks_pool;

  uint16_t need_to_push_ring_pool{0};

  bool is_running_{true};

  uint64_t buf_for_singnal;
  std::shared_ptr<Manage::WorkerChannel> channel_;

  // boost::container::small_vector<class T, std::size_t N>//TEMP

  enum class OpCode : uint8_t {
    Accept = 0,
    Send,
    Recv,
    StopSignal,
    Close,
    Connect,
    Read,
    Write,
    ManageSignal
  };

  struct UserData {
    uint32_t generation{0};
    uint32_t index : 24;
    OpCode op : 8;

    static UserData create(SessionHandle handle, OpCode op) {
      return UserData{
          .generation = handle.generation, .index = handle.index, .op = op};
    }

    operator uint64_t() const { return std::bit_cast<uint64_t>(*this); }
  };

  static_assert(sizeof(UserData) == 8);

public:
  Engine() = default;
  Engine(const Engine &) = delete;
  Engine &operator=(const Engine &) = delete;
  Engine(Engine &&other_) = delete;
  Engine &operator=(Engine &&) = delete;

  ~Engine() { cleanup(); }

  [[maybe_unused]] std::shared_ptr<Manage::WorkerChannel>
  create_or_get_worker_channel() {
    if (channel_) {
      return channel_;
    }
    channel_ = std::make_shared<Manage::WorkerChannel>();
    channel_->ev_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    return channel_;
  }

  static std::expected<std::unique_ptr<Engine>, std::string> create() {
    auto engine = std::make_unique<Engine>();
    io_uring_params params{};
    params.flags |= IORING_SETUP_SINGLE_ISSUER;

    auto ret =
        io_uring_queue_init_params(RING_ENTRIES_COUNT, &engine->ring_, &params);

    if (ret < 0) {
      return std::unexpected("Failed to init io_uring");
    }

    const auto ring_pool_size = 1024;
    if (!engine->provided_ring_pool_.init(&engine->ring_, 1, ring_pool_size)) {
      return std::unexpected("Failed to init provided ring pool");
    }
    for (int i = 0; i < ring_pool_size; ++i) {
      engine->provided_ring_pool_.recycle_buffer(
          engine->default_pool_16kb.allocate(), i);
    }
    engine->provided_ring_pool_.advanace();
    engine->create_or_get_worker_channel();
    engine->is_initialized_ = true;
    return engine;
  }

  void recv_manage_singnal() noexcept {
    auto *sqe = get_sqe();
    io_uring_prep_read(sqe, channel_->ev_fd, &buf_for_singnal,
                       sizeof(buf_for_singnal), 0);
    sqe->user_data = UserData{.op = OpCode::ManageSignal};
  }

  void run(std::stop_token stoken) {

    int stop_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    {
      auto *sqe = get_sqe();
      io_uring_prep_read(sqe, stop_fd, &buf_for_singnal,
                         sizeof(buf_for_singnal), 0);
      io_uring_sqe_set_data64(sqe, UserData{.op = OpCode::StopSignal});
    }

    recv_manage_singnal();

    std::stop_callback stop_cb(stoken, [this, stop_fd]() {
      uint64_t val = 1;
      write(stop_fd, &val, sizeof(val));
    });

    // auto _ = add_listener(8080);
    is_running_ = true;
    while (is_running_) {
      io_uring_cqe *cqe{nullptr};
      int ret = io_uring_submit_and_wait(&ring_, 1);

      if (ret < 0) {
        std::println("Error {}", ret);
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
      this->provided_ring_pool_.advanace();
    }

    std::println("End loop");
  }

  template <typename T>
  void process_cqe_t(io_uring_cqe *cqe, SessionHandle session_handle, T &Conn);

  void process_cqe(io_uring_cqe *cqe) {
    const auto ud = std::bit_cast<UserData>(cqe->user_data);
    switch (ud.op) {
    case OpCode::Accept:
      process_accept(cqe);
      break;
    case OpCode::Send:
      process_send(cqe);
      break;
    case OpCode::Recv:
      process_recv(cqe);
      break;
    case OpCode::StopSignal:
      is_running_ = false;
      std::println("Stop signal");
      break;
    case OpCode::Close:
      process_close(cqe);
      break;
    case OpCode::ManageSignal:
      process_manage(cqe);
      break;
    }
  }

private:
  void cleanup() noexcept {
    if (is_initialized_) {
      io_uring_queue_exit(&ring_);
      is_initialized_ = false;
    }
  }

  void process_manage(io_uring_cqe *cqe);

  void process_accept(io_uring_cqe *cqe) {
    auto *sqe = get_sqe();
    io_uring_prep_recv(sqe, cqe->res, nullptr, 0, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT | IOSQE_IO_LINK;
    sqe->buf_group = provided_ring_pool_.get_bgid();

    auto [handle, slot] = connections_.emplace<std::monostate>();

    if (!handle.is_valid()) {
      ::close(cqe->res);
      return;
    }
    auto &detection = slot->emplace<TestClientSession>();
    detection.fd = cqe->res;

    UserData ud{.generation = handle.generation,
                .index = handle.index,
                .op = OpCode::Recv};
    io_uring_sqe_set_data64(sqe, std::bit_cast<uint64_t>(ud));
  }

  void process_close(io_uring_cqe *cqe) {
    if (cqe->user_data == 0) {
      return;
    }

    auto ud = std::bit_cast<UserData>(cqe->user_data);
    auto handle = SessionHandle{.index = ud.index, .generation = ud.generation};
    auto *mb_conn =
        connections_.get({.index = ud.index, .generation = ud.generation});
    if (mb_conn == nullptr) {
      return;
    }

    ClientConnectionVariant &conn = *mb_conn;
    std::visit(
        [cqe, this, handle](auto &conn) {
          using T = std::decay_t<decltype(conn)>;

          if constexpr (std::is_same_v<T, TestClientSession>) {
            if (conn.egress_queue) {
              conn.egress_queue->clear(
                  ring_chunks_pool, [this](const BufferHandle16 &h) {
                    if (h.get_pool_id() == PoolType::DefaultPool16KB) {
                      default_pool_16kb.deallocate(h);
                    }
                  });
              egress_pool.deallocate(conn.egress_queue);
              conn.egress_queue = nullptr;
            }
          }
        },
        conn);

    connections_.release(handle);
  }

  void process_recv_test_client_session(io_uring_cqe *cqe,
                                        TestClientSession &conn,
                                        SessionHandle session_handle);

  void process_send_test_client_session(io_uring_cqe *cqe,
                                        TestClientSession &conn,
                                        SessionHandle session_handle);

  void close_conn(SessionHandle session_handle, int fd) {
    io_uring_sqe *sqe = get_sqe();
    io_uring_prep_close(sqe, fd);
    sqe->user_data = UserData{.generation = session_handle.generation,
                              .index = session_handle.index,
                              .op = OpCode::Close};
  }

  void process_recv(io_uring_cqe *cqe) {
    static std::string test_http200 = "HTTP/1.1 200 OK\r\n"
                                      "Content-Type: text/plain\r\n"
                                      "Content-Length: 13\r\n"
                                      "Connection: keep-alive\r\n"
                                      "\r\n"
                                      "Hello, World!";

    if (cqe->user_data == 0) {
      return;
    }
    auto ud = std::bit_cast<UserData>(cqe->user_data);

    auto handle = SessionHandle{.index = ud.index, .generation = ud.generation};
    auto *mb_conn =
        connections_.get({.index = ud.index, .generation = ud.generation});
    if (mb_conn == nullptr) {
      return;
    }

    ClientConnectionVariant &conn = *mb_conn;
    std::visit(
        [cqe, this, handle](auto &conn) {
          using T = std::decay_t<decltype(conn)>;

          if constexpr (std::is_same_v<T, TestClientSession>) {
            process_recv_test_client_session(cqe, conn, handle);
          }
        },
        conn);
  }

  // void flush_tx_queue(TestClientSession &session) {
  //   while (auto buf_opt = session.tx_queue.peek()) {
  //     auto &buf = *buf_opt;
  //   }
  // }

  void process_send(io_uring_cqe *cqe) {
    if (cqe->user_data == 0) {
      return;
    }
    auto ud = std::bit_cast<UserData>(cqe->user_data);

    auto handle = SessionHandle{.index = ud.index, .generation = ud.generation};
    auto *mb_conn =
        connections_.get({.index = ud.index, .generation = ud.generation});
    if (mb_conn == nullptr) {
      return;
    }

    ClientConnectionVariant &conn = *mb_conn;
    std::visit(
        [cqe, this, handle](auto &conn) {
          using T = std::decay_t<decltype(conn)>;

          if constexpr (std::is_same_v<T, TestClientSession>) {
            process_send_test_client_session(cqe, conn, handle);
          }
        },
        conn);
  }

  void send_data(int fd, BufferHandle16 handle_data,
                 SessionHandle handle) noexcept {
    auto *sqe = get_sqe();
    io_uring_prep_send(sqe, fd, handle_data.data(), handle_data.size(), 0);

    io_uring_sqe_set_data64(
        sqe, std::bit_cast<uint64_t>(UserData{.generation = handle.generation,
                                              .index = handle.index,
                                              .op = OpCode::Send}));
  } // test

  std::expected<void, std::string>
  add_listener(int port, Listener::Protocol protocol = Listener::Protocol::Tcp,
               Listener::IpVersion ipVersion = Listener::IpVersion::Ipv4) {
    auto mb_listener = Listener::create(port, protocol, ipVersion);
    if (!mb_listener) {
      return std::unexpected(
          std::format("Error create listener on port {}", port));
    }
    std::println("Add listener{}", mb_listener->listen_address());
    submit_accept(mb_listener->fd());
    listeners_.push_back(std::move(*mb_listener));
    return {};
  }

  void submit_accept(int listen_fd) {
    auto *sqe = get_sqe();
    io_uring_prep_multishot_accept(sqe, listen_fd, nullptr, nullptr,
                                   SOCK_CLOEXEC | SOCK_NONBLOCK);

    UserData ud = {.generation = static_cast<uint32_t>(listen_fd),
                   .op = OpCode::Accept};
    sqe->user_data = std::bit_cast<uint64_t>(ud);
  }

  void kill_listener(int fd) noexcept {}

  io_uring_sqe *get_sqe() noexcept {
    io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
    if (sqe == nullptr) {
      io_uring_submit(&ring_);
      sqe = io_uring_get_sqe(&ring_);
      if (sqe == nullptr) [[unlikely]] {
        std::terminate(); // temp
      }
    }
    return sqe;
  }
};
} // namespace Coring4