#pragma once

// All connection pools of ONE worker: one typed ConnPool<T> per connection type.
//
// This is the single place that knows the concrete types. Everything it does is
// a `switch` on the pool id (carried in the completion tag / timer node) that
// jumps to a fully inlined, statically dispatched ConnPool<T> member: no
// virtual calls, no vptr in any connection, no slot sized for the largest type.
// Capacities come from the loaded configuration (RuntimeConfig::pool_capacity);
// a pool of capacity 0 allocates nothing and rejects every spawn.

#include "coring5/io/ConnPool.hpp"
#include "coring5/proto/http1conn/Http1Conn.hpp"
#include "coring5/proto/http2/Http2Conn.hpp"
#include "coring5/proto/http3/Http3Conn.hpp"
#include "coring5/proto/l4/EchoConn.hpp"
#include "coring5/proto/l4/L4Conn.hpp"

namespace Coring5 {

class ConnPools {
public:
  explicit ConnPools(const PoolCapacities &cap)
      : echo_(cap[pool_index(PoolId::Echo)]), l4_(cap[pool_index(PoolId::L4)]),
        http1_(cap[pool_index(PoolId::Http1)]),
        http2_(cap[pool_index(PoolId::Http2)]),
        http3_(cap[pool_index(PoolId::Http3)]) {}

  ConnPools(const ConnPools &) = delete;
  ConnPools &operator=(const ConnPools &) = delete;

  // Calls f(pool) with the typed pool for `id`.
  template <class F> decltype(auto) visit(PoolId id, F &&f) {
    switch (id) {
    case PoolId::Echo:
      return f(echo_);
    case PoolId::L4:
      return f(l4_);
    case PoolId::Http1:
      return f(http1_);
    case PoolId::Http2:
      return f(http2_);
    case PoolId::Http3:
    case PoolId::Count_:
      break;
    }
    return f(http3_);
  }

  // Calls f(pool) for every pool, in PoolId order.
  template <class F> void for_each_pool(F &&f) {
    f(echo_);
    f(l4_);
    f(http1_);
    f(http2_);
    f(http3_);
  }

  // Creates and starts a connection in the pool of `id`. False when that pool
  // is full or has capacity 0; `fd` is then untouched.
  [[nodiscard]] bool spawn(PoolId id, IoContext &ctx, FdOwner &fd) {
    return visit(id, [&](auto &p) { return p.spawn(ctx, fd) != nullptr; });
  }

  // False for a stale or invalid handle.
  [[nodiscard]] bool on_cqe(IoContext &ctx, const Cqe &cqe,
                            IoBufferRing::Lease &lease) {
    return visit(cqe.tag.pool(),
                 [&](auto &p) { return p.on_cqe(ctx, cqe, lease); });
  }

  // Delivers an exchange event to its ingress connection; false if stale.
  [[nodiscard]] bool on_exchange(IoContext &ctx, ConnRef c) {
    return visit(c.pool, [&](auto &p) { return p.on_exchange(ctx, c.handle); });
  }

  [[nodiscard]] bool on_timer(IoContext &ctx, PoolId id, Handle h,
                              DeadlineKind kind) {
    return visit(id, [&](auto &p) { return p.on_timer(ctx, h, kind); });
  }

  void begin_drain_all(IoContext &ctx) {
    for_each_pool([&](auto &p) { p.begin_drain_all(ctx); });
  }

  void close_all(IoContext &ctx, Errc why) {
    for_each_pool([&](auto &p) { p.close_all(ctx, why); });
  }

  bool destroy(ConnRef c) noexcept {
    return visit(c.pool, [&](auto &p) { return p.destroy(c.handle); });
  }

  [[nodiscard]] uint32_t live(PoolId id) {
    return visit(id, [](auto &p) { return p.live(); });
  }
  [[nodiscard]] uint32_t capacity(PoolId id) {
    return visit(id, [](auto &p) { return p.capacity(); });
  }
  [[nodiscard]] uint32_t live() {
    uint32_t n = 0;
    for_each_pool([&](auto &p) { n += p.live(); });
    return n;
  }
  [[nodiscard]] uint64_t total_capacity() {
    uint64_t n = 0;
    for_each_pool([&](auto &p) { n += p.capacity(); });
    return n;
  }

private:
  ConnPool<EchoConn> echo_;
  ConnPool<L4Conn> l4_;
  ConnPool<Http1Conn> http1_;
  ConnPool<Http2Conn> http2_;
  ConnPool<Http3Conn> http3_;
};

} // namespace Coring5
