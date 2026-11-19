#pragma once

// HTTP/3 (QUIC) connection (M10, ngtcp2 + nghttp3). STATUS: operational stub.
//
// QUIC is UDP: connections are keyed by connection ID on a per-worker UDP
// socket, not by an accepted fd, so a real Http3Conn will usually own no fd of
// its own (ConnCore tolerates an invalid FdOwner). Nothing creates one yet (no
// UDP listener until M10); the type, its pool, its dispatch case and its
// metrics exist so the architecture is complete. A stub instance closes itself
// at once with the labelled reason `unsupported`.

#include "coring5/io/ConnCore.hpp"

namespace Coring5 {

class Http3Conn {
public:
  static constexpr PoolId kPool = PoolId::Http3;

  explicit Http3Conn(FdOwner fd) noexcept : core(std::move(fd)) {}

  ConnCore core;

  void start(IoContext &ctx);
  void on_cqe(IoContext &ctx, const Cqe &cqe, BufferRing::Lease &lease);
  void on_timer(IoContext &ctx, DeadlineKind kind);
  void on_drain(IoContext &ctx);
};

static_assert(ConnectionType<Http3Conn>);

} // namespace Coring5
