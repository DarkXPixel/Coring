#pragma once

// HTTP/1.1 server connection (M4.1 S2). Thin I/O shell around the sans-IO
// Http1::ServerSession (proto/http1session): it owns the fd, the ring
// submissions and the timer; the session owns all protocol state.
//
//   recv CQE  -> session.on_data/on_eof -> sync()
//   send CQE  -> session.on_sent        -> sync()
//   timer     -> session.on_deadline    -> sync()
//   exchange  -> on_exchange() (called by the Worker's ready-list drain)
//
// sync() is the only place that submits: routes a fresh Exchange, queues at
// most ONE send and ONE recv, re-arms the deadline when its epoch changed and
// closes when the session says so. Every submission is counted (I2); the
// session is released (exchange detached, chunks returned) once nothing is in
// flight. No Lease is kept across a step (I10): the session copies what it
// needs into its bounded stage chunk.
//
// Routing is not implemented yet: every accepted request is answered with a
// synthetic 501 (handlers are M4).

#include "coring5/io/ConnCore.hpp"
#include "coring5/proto/http1session/ServerSession.hpp"
#include <optional>

namespace Coring5 {

class Http1Conn {
public:
  static constexpr PoolId kPool = PoolId::Http1;

  explicit Http1Conn(FdOwner fd) noexcept : core(std::move(fd)) {}

  ConnCore core;

  void start(IoContext &ctx);
  void on_cqe(IoContext &ctx, const Cqe &cqe, IoBufferRing::Lease &lease);
  void on_timer(IoContext &ctx, DeadlineKind kind);
  void on_drain(IoContext &ctx);
  // The ExchangePool delivered our exchange (Side::Ingress).
  void on_exchange(IoContext &ctx);

#ifdef CORING5_FAKE_RING
  // Test-only: the S2 lifecycle tests inspect the sans-IO session's observable
  // state (deadline, close reason, served count). Compiled out of production.
  [[nodiscard]] Http1::ServerSession &session() noexcept { return *s_; }
#endif

private:
  void sync(IoContext &ctx);
  void arm_deadline(IoContext &ctx);

  std::optional<Http1::ServerSession> s_; // emplaced in start() (needs ctx)
  uint32_t armed_epoch_{0};
  bool recv_inflight_{false};
  bool send_inflight_{false};
};

static_assert(ConnectionType<Http1Conn>);

} // namespace Coring5
