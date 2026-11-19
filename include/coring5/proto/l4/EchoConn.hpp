#pragma once

// Reference connection type. It exercises the whole lifecycle: provided
// buffers, short sends, idle deadline, drain, cancel-on-close. Real protocols
// copy this shape.
//
//   recv (single-shot) -> keep Lease -> send (loop on short writes) -> recv ...
//
// One recv in flight at a time gives natural backpressure (invariant I6): we
// do not read again until the previous data is fully written back.

#include "coring5/io/ConnCore.hpp"

namespace Coring5 {

class EchoConn {
public:
  static constexpr PoolId kPool = PoolId::Echo;

  explicit EchoConn(FdOwner fd) noexcept : core(std::move(fd)) {}

  ConnCore core;

  void start(IoContext &ctx);
  void on_cqe(IoContext &ctx, const Cqe &cqe, BufferRing::Lease &lease);
  void on_timer(IoContext &ctx, DeadlineKind kind);
  // Finish the response that is being written, then close. Idle: close now.
  void on_drain(IoContext &ctx);

private:
  void arm_recv(IoContext &ctx);
  void send_rest(IoContext &ctx);

  BufferRing::Lease pending_; // data being echoed; released after the send
  uint32_t sent_{0};
};

static_assert(ConnectionType<EchoConn>);

} // namespace Coring5
