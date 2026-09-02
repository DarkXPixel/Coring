#include "coring2/TcpConnection.hpp"
#include "coring2/IOContext.hpp"
#include "coring2/WorkerThreadLoop.hpp"
#include "coring2/debug/test_handler.hpp"
#include "coring2/http1/http1_handler.hpp"
#include "coring2/uring/UringEventLoop.hpp"
#include <memory>
#include <print>

namespace Coring2 {
template <typename LoopType>
TcpConnection<LoopType>::TcpConnection(int fd) : socket_fd_(fd) {
  write_buf_.reserve(256);
  // handler_ = std::make_unique<Debug::TestHandler<LoopType>>(*this);
  handler_ = std::make_unique<Http1Handler<LoopType>>(*this);

  std::println("[+] New connection fd: {}", socket_fd_);
}

template <typename LoopType>
void TcpConnection<LoopType>::start_reading(LoopType &loop) {
  loop.submit_read(this, 15);
  ++pending_io_count_;
}

static_assert(sizeof(TcpConnection<UringLoop>) <= 64,
              "TcpConnection must be exactly 64 bytes!");

template class TcpConnection<UringLoop>;
} // namespace Coring2