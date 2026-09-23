#include "coring4/BufferHandle.hpp"
#include "coring4/EgressQueue.hpp"
#include "coring4/Engine.hpp"
#include "coring4/PoolType.hpp"
#include <cerrno>
#include <cstdint>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <utility>

static std::string_view test_http200 = "HTTP/1.1 200 OK\r\n"
                                       "Content-Type: text/plain\r\n"
                                       "Content-Length: 13\r\n"
                                       "Connection: keep-alive\r\n"
                                       "\r\n"
                                       "Hello, World!";

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

  //   if (!conn.recv_paused) {
  //     auto *sqe = get_sqe();
  //     io_uring_prep_recv(sqe, conn.fd, nullptr, 0, 0);
  //     sqe->flags |= IOSQE_BUFFER_SELECT;
  //     sqe->buf_group = provided_ring_pool_.get_bgid();
  //     sqe->user_data = UserData::create(session_handle, OpCode::Recv);
  //   }
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
} // namespace Coring4