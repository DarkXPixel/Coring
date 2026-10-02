#include "coring2/http1/http1_parser.hpp"
#include "coring4/BufferHandle.hpp"
#include "coring4/EgressQueue.hpp"
#include "coring4/Engine.hpp"
#include "coring4/Http1Stage.hpp"
#include "coring4/PoolType.hpp"
#include "coring4/Router.hpp"
#include "coring4/Sessions.hpp"
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <utility>
#include <variant>

static std::string_view test_http200 = "HTTP/1.1 200 OK\r\n"
                                       "Content-Type: text/plain\r\n"
                                       "Content-Length: 13\r\n"
                                       "Connection: keep-alive\r\n"
                                       "\r\n"
                                       "Hello, World!";

static constexpr std::string_view response_502 =
    "HTTP/1.1 502 Bad Gateway\r\n"
    "Content-Type: text/html; charset=utf-8\r\n"
    "Content-Length: 154\r\n"
    "Connection: close\r\n"
    "\r\n"
    "<!DOCTYPE html>\n"
    "<html>\n"
    "<head><title>502 Bad Gateway</title></head>\n"
    "body><center><h1>502 Bad Gateway</h1></center></body>\n"
    "</html>";

namespace Coring4 {

void Engine::process_send_test_client_session(io_uring_cqe *cqe,
                                              TestClientSession &conn,
                                              SessionHandle session_handle) {
  if (cqe->res <= 0) {
    close_conn(session_handle, conn.fd);
    return;
  }

  if (bool low_watermark = conn.egress_queue->on_send_complete(
          cqe->res, ring_chunks_pool,
          [this](const BufferHandle16 &h) {
            if (h.get_pool_id() == PoolType::DefaultPool16KB) {
              default_pool_16kb.deallocate(h);
            }
          });
      conn.recv_paused && low_watermark) {
    conn.recv_paused = false;
  }
  if (conn.egress_queue->empty()) {
    egress_pool.deallocate(conn.egress_queue);
    conn.egress_queue = nullptr;
  } else {
    io_uring_sqe *sqe = get_sqe();
    conn.egress_queue->prepare_send_sqe(sqe);
    sqe->user_data = UserData::create(session_handle, OpCode::Send);
  }

  if (conn.recv_paused && conn.multishot_recv) {
    io_uring_sqe *sqe = get_sqe();
    io_uring_prep_cancel64(sqe, UserData::create(session_handle, OpCode::Recv),
                           0);
    sqe->user_data = 0;
  } else if (!conn.recv_paused && !conn.multishot_recv) {
    // start
    io_uring_sqe *sqe = get_sqe();

    io_uring_prep_recv_multishot(sqe, conn.fd, nullptr, 0, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = provided_ring_pool_.get_bgid();
    sqe->user_data = UserData::create(session_handle, OpCode::Recv);
    conn.multishot_recv = true;
  }
}

void Engine::process_send_http1_client_session(io_uring_cqe *cqe,
                                               Http1ClientSession &conn,
                                               SessionHandle session_handle) {
  conn.egress_queue->on_send_complete(
      cqe->res, ring_chunks_pool, [this](const BufferHandle16 &h) {
        if (h.get_pool_id() == PoolType::DefaultPool16KB) {
          default_pool_16kb.deallocate(h);
        }
      });
  if (conn.egress_queue->empty()) {
    egress_pool.deallocate(conn.egress_queue);
    conn.egress_queue = nullptr;
  } else {
    io_uring_sqe *sqe = get_sqe();
    conn.egress_queue->prepare_send_sqe(sqe);
    sqe->user_data = UserData::create(session_handle, OpCode::Send);
  }
}

void Engine::process_recv_test_client_session(io_uring_cqe *cqe,
                                              TestClientSession &conn,
                                              SessionHandle session_handle) {

  if ((cqe->flags & IORING_CQE_F_MORE) == 0) {
    conn.multishot_recv = false;
  }
  if (cqe->res <= 0) {
    if (cqe->res == -ECANCELED) {
      return;
    }
    close_conn(session_handle, conn.fd);
    return;
  }

  const bool has_buffer = (cqe->flags & IORING_CQE_F_BUFFER) != 0;

  if (has_buffer) {
    const auto bid =
        static_cast<uint16_t>(cqe->flags >> IORING_CQE_BUFFER_SHIFT);

    auto handle_buf = provided_ring_pool_.get_by_bid(bid);
    provided_ring_pool_.recycle_buffer(handle_buf, bid);

    // if (!conn.upstream_handle_.is_valid()) {
    //   if (auto h = upstreams_.get("127.0.0.1:8887"); h.is_valid()) {
    //     conn.upstream_handle_ = h;
    //   } else {
    //   }

    //   conn.upstream_handle_ = upstreams_.get("127.0.0.1:8887");
    //   // if(conn.upstream_handle_)
    // }

    std::span<std::byte> sp(
        reinterpret_cast<std::byte *>(const_cast<char *>(test_http200.data())),
        test_http200.size());

    handle_buf = BufferHandle16(sp, PoolType::Static);

    PendingChunk chunk;
    chunk.add_handle(handle_buf);
    if (conn.egress_queue == nullptr) {
      conn.egress_queue = egress_pool.allocate(conn.fd);
    }

    conn.recv_paused =
        conn.egress_queue->push(std::move(chunk), ring_chunks_pool);

    if (conn.recv_paused && conn.multishot_recv) {
      io_uring_sqe *sqe = get_sqe();
      io_uring_prep_cancel64(sqe,
                             UserData::create(session_handle, OpCode::Recv), 0);
      sqe->user_data = 0;
    } else if (!conn.recv_paused && !conn.multishot_recv) {
      // start
      io_uring_sqe *sqe = get_sqe();

      io_uring_prep_recv_multishot(sqe, conn.fd, nullptr, 0, 0);
      sqe->flags |= IOSQE_BUFFER_SELECT;
      sqe->buf_group = provided_ring_pool_.get_bgid();
      sqe->user_data = UserData::create(session_handle, OpCode::Recv);
      conn.multishot_recv = true;
    }

    io_uring_sqe *sqe = get_sqe();
    conn.egress_queue->prepare_send_sqe(sqe);
    sqe->user_data = UserData::create(session_handle, OpCode::Send);
  }
}

void Engine::process_close_test_client_session(io_uring_cqe *cqe,
                                               TestClientSession &conn,
                                               SessionHandle session_handle) {

  if (conn.egress_queue != nullptr) {
    conn.egress_queue->clear(ring_chunks_pool, [this](const BufferHandle16 &h) {
      if (h.get_pool_id() == PoolType::DefaultPool16KB) {
        default_pool_16kb.deallocate(h);
      }
    });
    egress_pool.deallocate(conn.egress_queue);
    conn.egress_queue = nullptr;
  }
}

void Engine::process_read_static_upstream_session(
    io_uring_cqe *cqe, StaticUpstreamSession &conn,
    SessionHandle session_handle) {
  const bool has_buffer = (cqe->flags & IORING_CQE_F_BUFFER) != 0;

  if (has_buffer) {
    const auto bid =
        static_cast<uint16_t>(cqe->flags >> IORING_CQE_BUFFER_SHIFT);

    auto read_handle = provided_ring_pool_.get_by_bid(bid);
    auto &client_conn =
        std::get<Http1ClientSession>(*connections_.get(conn.client_handle));
    provided_ring_pool_.recycle_buffer(default_pool_16kb.allocate(), bid);

    bool need_send_sqe{false};
    if (client_conn.egress_queue == nullptr) {
      client_conn.egress_queue = egress_pool.allocate(client_conn.fd);
      need_send_sqe = true;
    }

    read_handle.set_size(cqe->res);

    std::string_view test(reinterpret_cast<const char *>(read_handle.as_raw()),
                          read_handle.size());

    PendingChunk chunk;
    chunk.add_handle(read_handle);
    client_conn.egress_queue->push(std::move(chunk), ring_chunks_pool);
    if (need_send_sqe) {
      io_uring_sqe *sqe = get_sqe();
      client_conn.egress_queue->prepare_send_sqe(sqe);
      sqe->user_data = UserData::create(session_handle, OpCode::Send);
    }
    conn.readed += cqe->res;
    if (conn.readed < conn.size && cqe->res != 0) {
      auto *sqe = get_sqe();
      io_uring_prep_read(sqe, conn.fd, nullptr, 0, conn.readed);
      sqe->flags |= IOSQE_BUFFER_SELECT;
      sqe->buf_group = provided_ring_pool_.get_bgid();
      sqe->user_data = UserData::create(session_handle, OpCode::Read);
    } else {
      client_conn.stage = Http1Stage::ReadingHeaders;
      prep_client_recv_provided_buf(client_conn.fd, conn.client_handle);
    }
  }
}

const char *get_content_type(const char *path) {
  const char *ext = strrchr(path, '.');
  if (!ext)
    return "application/octet-stream"; // Значение по умолчанию (двоичный файл)

  if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0)
    return "text/html; charset=utf-8";
  if (strcmp(ext, ".css") == 0)
    return "text/css";
  if (strcmp(ext, ".js") == 0)
    return "text/javascript";
  if (strcmp(ext, ".json") == 0)
    return "application/json";
  if (strcmp(ext, ".png") == 0)
    return "image/png";
  if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0)
    return "image/jpeg";
  if (strcmp(ext, ".gif") == 0)
    return "image/gif";
  if (strcmp(ext, ".svg") == 0)
    return "image/svg+xml";
  if (strcmp(ext, ".webp") == 0)
    return "image/webp";
  if (strcmp(ext, ".pdf") == 0)
    return "application/pdf";
  if (strcmp(ext, ".mp4") == 0)
    return "video/mp4";
  if (strcmp(ext, ".mp3") == 0)
    return "audio/mpeg";
  if (strcmp(ext, ".zip") == 0)
    return "application/zip";
  if (strcmp(ext, ".txt") == 0)
    return "text/plain; charset=utf-8";

  return "application/octet-stream";
}

void Engine::process_recv_http1_client_session(io_uring_cqe *cqe,
                                               Http1ClientSession &conn,
                                               SessionHandle session_handle) {
  if (conn.stage == Http1Stage::WaitUpstream) {
    return;
  }
  if (conn.stage == Http1Stage::ReadingHeaders) {
    if (!conn.recv_handle.is_valid()) {
      const bool has_buffer = (cqe->flags & IORING_CQE_F_BUFFER) != 0;

      if (has_buffer) {
        const auto bid =
            static_cast<uint16_t>(cqe->flags >> IORING_CQE_BUFFER_SHIFT);

        conn.recv_handle = provided_ring_pool_.get_by_bid(bid);
        provided_ring_pool_.recycle_buffer(default_pool_16kb.allocate(), bid);
      }
    }

    conn.recv_offset += cqe->res;
    if (std::cmp_greater_equal(conn.recv_offset, conn.recv_handle.size())) {
      ::close(conn.fd);
      return;
    }
    std::string_view str_parse(
        reinterpret_cast<const char *>(conn.recv_handle.as_raw()),
        conn.recv_offset);
    if (auto res = Coring2::Http1Parser::parse(str_parse); res) {
      if (auto host = res->request.get("Host"); host) {
        if (const auto *route = router_.get_route(conn.port, *host); route) {
          const auto &out_route = route->out_routes.at("/");
          const std::string_view base_path = out_route.root;

          auto [handle, slot] = upstream_connections_.emplace<std::monostate>();
          if (!handle.is_valid() || slot == nullptr) {
            ::close(conn.fd);
            return;
          }

          auto &upstream = slot->emplace<StaticUpstreamSession>();
          std::string path;
          if (res->request.path == "/") {
            path = std::string(base_path) + "/" + std::string(out_route.index);
          } else {
            path = std::string(base_path) + std::string(res->request.path);
          }

          upstream.fd = ::open(path.c_str(), O_RDONLY);
          int err = errno;
          upstream.client_handle = session_handle;
          auto *sqe = get_sqe();
          io_uring_prep_read(sqe, upstream.fd, nullptr, 0, 0);
          sqe->flags |= IOSQE_BUFFER_SELECT;
          sqe->buf_group = provided_ring_pool_.get_bgid();
          sqe->user_data = UserData::create(handle, OpCode::Read);
          conn.stage = Http1Stage::WaitUpstream;

          if (conn.egress_queue == nullptr) {
            conn.egress_queue = egress_pool.allocate(conn.fd);
          }

          struct stat st;
          fstat(upstream.fd, &st);
          // char header[256];

          std::string header =
              std::format("HTTP/1.1 200 OK\r\n"
                          "Content-Length: {}\r\n"
                          "Content-Type: {}\r\n"
                          "Connection: close\r\n\r\n",
                          st.st_size, get_content_type(path.c_str()));
          upstream.size = st.st_size;
          PendingChunk chunk;
          conn.recv_handle.set_size(header.size());
          std::memcpy(conn.recv_handle.as_raw(), header.c_str(), header.size());
          chunk.add_handle(conn.recv_handle);
          conn.recv_handle = {};
          conn.egress_queue->push(std::move(chunk), ring_chunks_pool);
          sqe = get_sqe();
          conn.egress_queue->prepare_send_sqe(sqe);
          sqe->user_data = UserData::create(session_handle, OpCode::Send);
        }
        router_.get_route(conn.port, *host);
      }

    } else {
      if (res.error() == Coring2::Http1Parser::ParseError::Incomplete) {
        auto *sqe = get_sqe();
        io_uring_prep_recv(sqe, conn.fd,
                           conn.recv_handle.as_raw() + conn.recv_offset,
                           conn.recv_handle.size() - conn.recv_offset, 0);
        sqe->user_data = UserData::create(session_handle, OpCode::Recv);
        return;
      }
      ::close(conn.fd);
      return;
    }
  }
  // provided_ring_pool_.recycle_buffer(handle_buf, bid);
}
} // namespace Coring4