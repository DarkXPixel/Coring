#pragma once

// Reference Connection implementation (the only protocol enabled in the
// skeleton). It exercises the whole lifecycle: provided buffers, short sends,
// idle deadline, drain, cancel-on-close. Real protocols copy this shape.
//
//   recv (single-shot) -> keep Lease -> send (loop on short writes) -> recv ...
//
// One recv in flight at a time gives natural backpressure (invariant I6): we
// do not read again until the previous data is fully written back.

#include "coring5/io/Connection.hpp"

namespace Coring5 {

class EchoConnection final : public Connection {
public:
  using Connection::Connection;
  void start(IoContext &ctx) override;

protected:
  void on_cqe(IoContext &ctx, const Cqe &cqe,
              BufferRing::Lease &lease) override;
  void on_timer(IoContext &ctx, DeadlineKind kind) override;
  // Finish the response that is being written, then close. Idle: close now.
  void on_drain(IoContext &ctx) override;

private:
  void arm_recv(IoContext &ctx);
  void send_rest(IoContext &ctx);

  BufferRing::Lease pending_; // data being echoed; released after the send
  uint32_t sent_{0};
};

} // namespace Coring5
