#include "coring3/ThreadWorker.hpp"
#include "coring3/ComplectionKind.hpp"
#include "coring3/EmptySession.hpp"
#include "coring3/ProvidedBufferPool.hpp"
#include "coring3/Sessions.hpp"
#include "coring3/Stream.hpp"
#include "coring3/UserData.hpp"
#include <cstddef>
#include <cstring>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <sys/socket.h>
#include <variant>

namespace Coring3 {
constexpr std::string_view H2_CLIENT_PREFACE =
    "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

[[nodiscard]] constexpr bool
is_http2_magic(std::span<const std::byte> buffer) noexcept {
  if (buffer.size() < H2_CLIENT_PREFACE.size()) {
    return false;
  }

  return std::memcmp(buffer.data(), H2_CLIENT_PREFACE.data(),
                     H2_CLIENT_PREFACE.size()) == 0;
}

void ThreadWorker::on_accept(int client_fd) noexcept {
  if (client_fd < 0) {
    return;
  }

  auto [handle, slot] = client_conns_.emplace<EmptySession>();
  if (!handle.is_valid()) {
    close(client_fd);
    return;
  }

  auto &detection = slot->emplace<DetectionSession>();
  detection.fd = client_fd;

  UserData ud{.generation = handle.generation,
              .index = handle.index,
              .op_code = static_cast<uint8_t>(ComplectionKind::ClientRecv)};

  io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
  io_uring_prep_recv(sqe, client_fd, detection.peek_buf.data(),
                     detection.peek_buf.size(), MSG_PEEK);
  io_uring_sqe_set_data64(sqe, ud.pack());
}

void ThreadWorker::on_client_read(SessionHandle conn_handle,
                                  ClientConnectionVariant &conn_var,
                                  io_uring_cqe *cqe) noexcept {
  const int bytes_read = cqe->res;
  if (bytes_read <= 0) {
    close_client_connection(conn_handle, conn_var);
    return;
  }

  std::span<std::byte> rx_data;
  uint16_t bid = 0;

  if (cqe->flags & IORING_CQE_F_BUFFER) {
    bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
    rx_data = rx_buf_pool.get_buffer(bid, bytes_read);
  }

  std::visit(
      [this, conn_handle, bytes_read](auto &session) {
        using T = std::decay_t<decltype(session)>;

        if constexpr (std::is_same_v<T, DetectionSession>) {
          session.bytes_read += bytes_read;
          std::span<std::byte> peek_span{session.peek_buf.data(),
                                         session.bytes_read};
          if (peek_span.size() < H2_CLIENT_PREFACE.size()) {
            if (std::memcmp(peek_span.data(), H2_CLIENT_PREFACE.data(),
                            peek_span.size()) != 0) {
              promote_to_http1(conn_handle, session);
            } else {
              arm_detecting_recv(conn_handle, session);
            }
            return;
          }

          if (is_http2_magic(peek_span)) {
            promote_to_http2(conn_handle, session);
          } else {
            promote_to_http2(conn_handle, session);
          }
        } else if constexpr (std::is_same_v<T, Http1ClientSession>) {
          // process
        }
      },
      conn_var);
}

void ThreadWorker::arm_detecting_recv(SessionHandle conn_handle,
                                      DetectionSession &conn_var) {
  UserData ud = {.generation = conn_handle.generation,
                 .index = conn_handle.index,
                 .op_code = static_cast<uint8_t>(ComplectionKind::ClientRecv)};

  io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
  io_uring_prep_recv(sqe, conn_var.fd,
                     conn_var.peek_buf.data() + conn_var.bytes_read,
                     conn_var.peek_buf.size() - conn_var.bytes_read, MSG_PEEK);
  io_uring_sqe_set_data64(sqe, ud.pack());
}

void ThreadWorker::promote_to_http1(SessionHandle conn_handle,
                                    DetectionSession &detecting) noexcept {
  int fd = detecting.fd;

  auto *slot = client_conns_.get(conn_handle);
  auto &h1 = slot->emplace<Http1ClientSession>();
  h1.fd = fd;

  auto [stream_handle, stream_slot] = streams_.emplace<EmptySession>();

  if (!stream_handle.is_valid()) {
    close(fd);
    return;
  }

  auto &stream = stream_slot->emplace<HttpTransactionStream>();

  stream.self_handle = stream_handle;
  stream.client_conn_handle = conn_handle;
  stream.state = StreamState::ReadingHeaders;

  h1.active_stream_handle = stream_handle;

  arm_client_recv_provided(conn_handle, fd);
}

void ThreadWorker::promote_to_http2(SessionHandle conn_handle,
                                    DetectionSession &detecting) noexcept {
  int fd = detecting.fd;
  auto *slot = client_conns_.get(conn_handle);
  if (!slot) {
    return;
  }

  auto &h2 = slot->emplace<Http2ClientSession>();
  h2.fd = fd;
  h2.remote_window_size = 65535;

  arm_client_recv_provided(conn_handle, fd);
}

void ThreadWorker::arm_client_recv_provided(SessionHandle handle,
                                            int fd) noexcept {
  io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
  if (!sqe) {
    return;
  }

  io_uring_prep_recv(sqe, fd, nullptr, 0, 0);
  sqe->flags |= IOSQE_BUFFER_SELECT;
  sqe->buf_group = ProvidedBufferPool::BGID_CLIENT_RX;
  UserData ud{.generation = handle.generation,
              .index = handle.index,
              .op_code = static_cast<uint8_t>(ComplectionKind::ClientRecv)};
  io_uring_sqe_set_data64(sqe, ud.pack());
}

void ThreadWorker::close_client_connection(
    SessionHandle handle, ClientConnectionVariant &conn_var) noexcept {
  std::visit(
      [this](auto &session) {
        using T = std::decay_t<decltype(session)>;

        if constexpr (!std::is_same_v<T, EmptySession>) {
          if (session.fd >= 0) {
            close(session.fd);
            session.fd = -1;
          }
        }

        if constexpr (std::is_same_v<T, Http1ClientSession>) {
          if (session.active_stream_handle.is_valid()) {
            streams_.release(session.active_stream_handle);
          }
        }
      },
      conn_var);
  client_conns_.release(handle);
}

void ThreadWorker::process_http1_data(
    SessionHandle conn_handle, Http1ClientSession &session,
    std::span<const std::byte> rx_data) noexcept {
  auto *stream_slot = streams_.get(session.active_stream_handle);
  if (!stream_slot) {
    return;
  }

  auto *stream_ptr = std::get_if<HttpTransactionStream>(stream_slot);
  if (!stream_ptr) {
    return;
  }
  auto &stream = *stream_ptr;
  auto &meta = stream.request_meta;

  // auto &stream = stream_slot->get<HttpTransactionStream>();
}

} // namespace Coring3