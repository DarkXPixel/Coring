#pragma once
#include "coring2/Buffer.hpp"
#include "coring2/Deffer.hpp"
#include "coring2/IOContext.hpp"
#include "coring2/ProtocolHandler.hpp"
// #include "coring2/uring/UringEventLoop.hpp"
#include <array>
#include <cerrno>
#include <cstdint>
#include <memory>
#include <print>
#include <unistd.h>
#include <vector>

namespace Coring2 {
template <typename LoopType> class alignas(64) TcpConnection {
  enum class State : uint8_t { Active, Closing };

public:
  std::vector<std::byte> write_buf_; // temp
  std::unique_ptr<IProtocolHandler<LoopType>> handler_{nullptr};
  std::size_t bytes_sent_{0};
  int socket_fd_{-1};
  uint8_t pending_io_count_ : 6 {0};
  State state_ : 2 {TcpConnection::State::Active};
  bool is_writing_ : 1 {false};
  uint8_t padding_flags_ : 7 {0};

public:
  TcpConnection(int fd);

  ~TcpConnection() {
    if (socket_fd_ >= 0) {
      ::close(socket_fd_);
    }
  }

  inline void on_io_completed() noexcept {
    --pending_io_count_;
    if (state_ == State::Closing && pending_io_count_ == 0) {
      delete this; // temp
    }
  }

  void on_read_completed(LoopType &loop, int res, void *buf) {
    Coring2::Utility::Defer _([this]() { on_io_completed(); });
    if (res <= 0) {
      mark_for_closing(loop);
      return;
    }
    if (state_ == State::Closing && pending_io_count_ == 0) {
      delete this; // temp
    }

    if (buf == nullptr) {
      // reread
    }

    std::span<const std::byte> recieved_data(static_cast<std::byte *>(buf),
                                             res);
    auto response = handler_->on_data(loop, recieved_data);
    if (response == HandlerResponse::Close) {
      mark_for_closing(loop);
      return;
    }
    if (response != HandlerResponse::NotRead && state_ == State::Active) {
      start_reading(loop);
    }
  }

  void start_reading(LoopType &loop);

  void send_data(LoopType &loop, std::span<const std::byte> data) noexcept {
    write_buf_.append_range(data);
    if (!is_writing_) {
      is_writing_ = true;
      flush_write_queue(loop);
    }
  }

  void on_cancel_completed(LoopType &loop, int res) noexcept {
    Coring2::Utility::Defer _([this]() { on_io_completed(); });
    if (res == -ENOENT) {
      pending_io_count_ = 0;
    }
  }

  void on_close_completed(LoopType &loop, int res) noexcept {
    Coring2::Utility::Defer _([this]() { on_io_completed(); });
    std::println("[-] Close connection fd: {}", socket_fd_);
    if (res < 0) {
      std::println("Close failed for fd {} with error: {}", socket_fd_, res);
    }
    socket_fd_ = -1;
  }

  void on_write_completed(LoopType &loop, int res) noexcept {
    Coring2::Utility::Defer _([this]() { on_io_completed(); });
    if (res <= 0) {
      mark_for_closing(loop);
      return;
    }
    bytes_sent_ += res;
    if (bytes_sent_ == write_buf_.size()) {
      bytes_sent_ = 0;
      write_buf_.clear();
      is_writing_ = false;
    } else {
      flush_write_queue(loop);
    }
  }

  void mark_for_closing(LoopType &loop) {
    if (state_ == State::Closing) {
      return;
    }

    state_ = State::Closing;

    if (socket_fd_ >= 0) {
      ++pending_io_count_;
      loop.close_fd(this);
    }
  }

  void flush_write_queue(LoopType &loop) noexcept {
    std::span<const std::byte> remaining(write_buf_.data() + bytes_sent_,
                                         write_buf_.size() - bytes_sent_);
    ++pending_io_count_;
    loop.submit_write(this, remaining);
  };

private:
};

} // namespace Coring2