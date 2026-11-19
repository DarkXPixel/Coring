#pragma once

// HTTP/1.1 server connection (M4). STATUS: operational stub.
//
// What it does today: arms the header deadline, receives into a provided
// buffer, parses the request head with the strict sans-IO parser and answers a
// fixed `501 Not Implemented` (valid head) or `400 Bad Request` (malformed or
// incomplete head), always with `Connection: close`, then closes. This
// exercises the whole lifecycle (I2/I3, deadlines, drain, short sends).
//
// Documented limit: a head must arrive in ONE recv (no staging buffer yet, see
// invariant I10 and docs/ARCHITECTURE.md section 7); a head split across
// segments is answered with 400. Body framing, routing and the real handlers
// are M4.

#include "coring5/io/ConnCore.hpp"
#include <string_view>

namespace Coring5 {

class Http1Conn {
public:
  static constexpr PoolId kPool = PoolId::Http1;

  explicit Http1Conn(FdOwner fd) noexcept : core(std::move(fd)) {}

  ConnCore core;

  void start(IoContext &ctx);
  void on_cqe(IoContext &ctx, const Cqe &cqe, BufferRing::Lease &lease);
  void on_timer(IoContext &ctx, DeadlineKind kind);
  void on_drain(IoContext &ctx);

private:
  void arm_recv(IoContext &ctx);
  void respond(IoContext &ctx, std::string_view response, Errc close_reason);

  // Points at static storage: it outlives any in-flight send.
  std::string_view response_{};
  uint32_t sent_{0};
  Errc close_reason_{Errc::Eof};
  bool responding_{false};
};

static_assert(ConnectionType<Http1Conn>);

} // namespace Coring5
