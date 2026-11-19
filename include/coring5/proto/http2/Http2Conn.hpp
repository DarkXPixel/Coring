#pragma once

// HTTP/2 connection (M8, nghttp2 adapter). STATUS: operational stub.
//
// Same shape as L4Conn: idle deadline + one counted recv; the first byte closes
// the connection with the labelled reason `unsupported`. The config validator
// does not accept `protocol = "http2"` yet, so this pool is sized 0 in real
// deployments; the type exists so the architecture (pool, dispatch, metrics)
// is complete and compiles/links.

#include "coring5/io/ConnCore.hpp"

namespace Coring5 {

class Http2Conn {
public:
  static constexpr PoolId kPool = PoolId::Http2;

  explicit Http2Conn(FdOwner fd) noexcept : core(std::move(fd)) {}

  ConnCore core;

  void start(IoContext &ctx);
  void on_cqe(IoContext &ctx, const Cqe &cqe, BufferRing::Lease &lease);
  void on_timer(IoContext &ctx, DeadlineKind kind);
  void on_drain(IoContext &ctx);
};

static_assert(ConnectionType<Http2Conn>);

} // namespace Coring5
