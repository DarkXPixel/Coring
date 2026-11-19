#pragma once

// Fixed-capacity pool of ONE concrete connection type (invariants I4, I7).
//
// The worker owns one ConnPool<T> per connection type (echo, l4, http1, http2,
// http3). Each slot is exactly sizeof(T), so a small connection type does not
// pay for the largest one, and every call below is resolved at compile time:
// the only runtime dispatch left on the data path is the `switch` on the pool id
// carried in the completion tag (see runtime/ConnPools.hpp).
//
// All lifecycle hooks of T are reached through `T::core` (io/ConnCore.hpp), which
// implements I2/I3 once for every type.

#include "coring5/io/ConnCore.hpp"
#include "coring5/mem/HandlePool.hpp"

namespace Coring5 {

template <ConnectionType T> class ConnPool {
public:
  explicit ConnPool(uint32_t capacity) : pool_(capacity) {}

  // Creates, binds and starts a connection. On success `fd` has been moved into
  // the connection. Returns nullptr and leaves `fd` untouched when the pool is
  // full (the caller counts the rejection; `fd` closes with its owner).
  [[nodiscard]] T *spawn(IoContext &ctx, FdOwner &fd) {
    auto [h, conn] = pool_.template create<T>(std::move(fd));
    if (conn == nullptr) [[unlikely]] {
      return nullptr;
    }
    conn->core.bind(ConnRef{T::kPool, h});
    conn->start(ctx);
    return conn;
  }

  // Completion for a connection of this pool. Returns false for a stale or
  // invalid handle (the caller bumps `stale_events`, which must stay 0).
  [[nodiscard]] bool on_cqe(IoContext &ctx, const Cqe &cqe,
                            BufferRing::Lease &lease) {
    T *c = pool_.get(cqe.tag.handle());
    if (c == nullptr) [[unlikely]] {
      return false;
    }
    if (c->core.begin_cqe(cqe)) {
      c->on_cqe(ctx, cqe, lease);
    }
    c->core.end_cqe(ctx);
    return true;
  }

  [[nodiscard]] bool on_timer(IoContext &ctx, Handle h, DeadlineKind kind) {
    T *c = pool_.get(h);
    if (c == nullptr) [[unlikely]] {
      return false;
    }
    if (!c->core.closing()) {
      c->on_timer(ctx, kind);
    }
    return true;
  }

  void begin_drain_all(IoContext &ctx) {
    pool_.for_each([&](Handle, T &c) {
      if (c.core.begin_drain()) {
        c.on_drain(ctx);
      }
    });
  }

  void close_all(IoContext &ctx, Errc why) {
    pool_.for_each([&](Handle, T &c) { c.core.close(ctx, why); });
  }

  // Only the worker, at a safe point, for connections that reached Closed.
  bool destroy(Handle h) noexcept { return pool_.destroy(h); }

  [[nodiscard]] uint32_t live() const noexcept { return pool_.live(); }
  [[nodiscard]] uint32_t capacity() const noexcept { return pool_.capacity(); }

private:
  HandlePool<T> pool_;
};

} // namespace Coring5
