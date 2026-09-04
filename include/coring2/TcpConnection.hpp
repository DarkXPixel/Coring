#pragma once
#include "coring2/Buffer.hpp"
#include "coring2/Deffer.hpp"
#include "coring2/IOContext.hpp"
// #include "coring2/ProtocolHandler.hpp"
#include "coring2/FixedBufferPool.hpp"
#include "http1/http1_handler.hpp"
#include "protocol/uninit_protocol.hpp"
#include <array>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <print>
#include <unistd.h>
#include <variant>
#include <vector>

namespace Coring2 {

struct BufferHeader {
  std::byte *next{nullptr};
  uint32_t size{0};
  uint32_t offset{0};
};

template <typename LoopType> class alignas(64) TcpConnection {
  enum class State : uint8_t { Active = 0, Closing = 1 };

public:
  std::variant<UninitProtocol<LoopType>, Http1Handler_<LoopType>> handler_{
      UninitProtocol<LoopType>{}};
  int socket_fd_{-1};
  uint8_t pending_io_count_ : 6 {0};
  State state_ : 1 {TcpConnection::State::Active};
  FixedBufferPool4096 &pool_buffer_;

  std::byte *write_head_{nullptr};
  std::byte *write_tail_{nullptr};

  bool multishot_enable : 1 = true;

  TcpConnection(int fd, FixedBufferPool4096 &pool_buffer);

  ~TcpConnection() {
    if (socket_fd_ >= 0) {
      ::close(socket_fd_);
    }
  }

  void on_io_completed() noexcept {
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
    auto response = std::visit(
        [&loop, &recieved_data, this](auto &&h) {
          return h.on_data(loop, *this, recieved_data);
        },
        handler_);
    if (response == HandlerResponse::Close) {
      mark_for_closing(loop);
      return;
    }
    if (response != HandlerResponse::NotRead && state_ == State::Active &&
        !multishot_enable) {
      start_reading(loop);
    }
  }

  void start_reading(LoopType &loop);

  void send_data(LoopType &loop, std::span<std::byte> data) noexcept {
    if (data.empty()) {
      return;
    }

    std::byte *full_block_ptr = data.data() - sizeof(BufferHeader);
    auto *hdr = new (full_block_ptr)
        BufferHeader{.next = nullptr,
                     .size = static_cast<uint32_t>(data.size()),
                     .offset = 0};

    if (!write_tail_) {
      write_head_ = write_tail_ = full_block_ptr;
    } else {
      reinterpret_cast<BufferHeader *>(write_tail_)->next = full_block_ptr;
      write_tail_ = full_block_ptr;
    }

    if (write_head_ == full_block_ptr) {
      flush_write_queue(loop);
    }

    // write_buf_.append_range(data);
    // if (!is_writing_) {
    //   is_writing_ = true;
    //   flush_write_queue(loop);
    // }
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
    if (!write_head_) {
      return;
    }

    auto *hdr = reinterpret_cast<BufferHeader *>(write_head_);
    hdr->offset += res;

    if (hdr->offset >= hdr->size) {
      std::byte *old_head = write_head_;

      write_head_ = hdr->next;
      if (!write_head_) {
        write_tail_ = nullptr;
      }

      pool_buffer_.deallocate(old_head);
    }

    if (write_head_) {
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
    auto *hdr = reinterpret_cast<BufferHeader *>(write_head_);

    const std::byte *send_ptr =
        write_head_ + sizeof(BufferHeader) + hdr->offset;
    std::size_t bytes_left = hdr->size - hdr->offset;

    std::span<const std::byte> remaining(send_ptr, bytes_left);

    ++pending_io_count_;
    loop.submit_send(this, remaining);
  };

  std::span<std::byte> prepare_buffer() {
    auto block = pool_buffer_.allocate();
    return block.subspan(sizeof(BufferHeader));
  }

private:
};

} // namespace Coring2