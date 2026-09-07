#include "coring3/ThreadWorker.hpp"
#include "coring3/ComplectionKind.hpp"
#include "coring3/EmptySession.hpp"
#include "coring3/Sessions.hpp"
#include "coring3/Stream.hpp"
#include "coring3/UserData.hpp"
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
                                  int bytes_read) noexcept {
  if (bytes_read <= 0) {
    return;
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
              // to_http1
            } else {
              arm_detecting_recv(conn_handle, session);
            }
            return;
          }

          if (is_http2_magic(peek_span)) {
            // to http2
          } else {
            // to http1
          }
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
}

} // namespace Coring3