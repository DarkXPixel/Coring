#pragma once

#include "coring4/BufferHandle.hpp"
#include "coring4/BufferPool.hpp"
#include "coring4/Listener.hpp"
#include "coring4/ProvidedBufferPool.hpp"
#include "coring4/SessionStorage.hpp"
#include "coring4/Sessions.hpp"
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

  LocalBufferPool<> buffer_pool{0, 8192};

  ProvidedBufferPool<> provided_ring_pool_;

  uint16_t need_to_push_ring_pool{0};

  // boost::container::small_vector<class T, std::size_t N>//TEMP

  enum class OpCode : uint8_t { Accept = 0, Send, Recv };

  struct UserData {
    uint32_t generation{0};
    uint32_t index : 24;
    OpCode op : 8;
  };

  static_assert(sizeof(UserData) == 8);

public:
  Engine() = default;
  Engine(const Engine &) = delete;
  Engine &operator=(const Engine &) = delete;
  Engine(Engine &&other_) = delete;
  Engine &operator=(Engine &&) = delete;

  ~Engine() { cleanup(); }

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
      engine->provided_ring_pool_.recycle_buffer(engine->buffer_pool.allocate(),
                                                 i);
    }
    engine->provided_ring_pool_.advanace(ring_pool_size);

    engine->is_initialized_ = true;
    return engine;
  }

  void run(std::stop_token stoken) {
    std::atomic<bool> running{true};
    std::stop_callback stop_cb(stoken, [&running, this]() {
      running.store(false, std::memory_order_relaxed);
    });

    auto _ = add_listener_tcp(8080);

    while (running.load(std::memory_order_relaxed)) {
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
      if (this->need_to_push_ring_pool > 0) {
        this->provided_ring_pool_.advanace(
            std::exchange(need_to_push_ring_pool, 0));
      }
    }

    std::println("End loop");
  }

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
    }
  }

private:
  void cleanup() noexcept {
    if (is_initialized_) {
      io_uring_queue_exit(&ring_);
      is_initialized_ = false;
    }
  }

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

  void process_recv(io_uring_cqe *cqe) {
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
            if (cqe->res <= 0) {
              ::close(conn.fd);
              if (conn.write_handle.is_valid()) {
                this->buffer_pool.deallocate(conn.write_handle);
              }
              this->connections_.release(handle);
              return;
            }

            const bool has_buffer = (cqe->flags & IORING_CQE_F_BUFFER) != 0;

            if (has_buffer) {
              const auto bid =
                  static_cast<uint16_t>(cqe->flags >> IORING_CQE_BUFFER_SHIFT);

              auto handle_buf = this->provided_ring_pool_.get_by_bid(bid);
              this->provided_ring_pool_.recycle_buffer(
                  this->buffer_pool.allocate(), bid);
              ++this->need_to_push_ring_pool;

              this->send_data(conn.fd, handle_buf, cqe->res, handle);
              conn.write_handle = handle_buf;
              return;
            }
          }
        },
        conn);
    // const bool has_buffer = (cqe->flags & IORING_CQE_F_BUFFER) != 0;

    // if (has_buffer) {
    // } else {
    //   ::close(conn);
    // }
  }

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
            if (cqe->res <= 0) {
              this->buffer_pool.deallocate(conn.write_handle);
              ::close(conn.fd);
              this->connections_.release(handle);
            }
            this->buffer_pool.deallocate(conn.write_handle);
            conn.write_handle = {};
            auto *sqe = get_sqe();
            io_uring_prep_recv(sqe, conn.fd, nullptr, 0, 0);
            sqe->flags |= IOSQE_BUFFER_SELECT | IOSQE_IO_LINK;
            sqe->buf_group = provided_ring_pool_.get_bgid();
            UserData ud{.generation = handle.generation,
                        .index = handle.index,
                        .op = OpCode::Recv};
            io_uring_sqe_set_data64(sqe, std::bit_cast<uint64_t>(ud));
            return;
          }
        },
        conn);
  }

  void send_data(int fd, BufferHandle16 handle_data, std::size_t size,
                 SessionHandle handle) noexcept {
    auto *sqe = get_sqe();
    io_uring_prep_send(sqe, fd, handle_data.as_raw(), size, 0);

    io_uring_sqe_set_data64(
        sqe, std::bit_cast<uint64_t>(UserData{.generation = handle.generation,
                                              .index = handle.index,
                                              .op = OpCode::Send}));
  } // test

  std::expected<void, std::string> add_listener_tcp(int port) {
    auto mb_listener = Listener::create(port, Listener::Protocol::Tcp,
                                        Listener::IpVersion::All);
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