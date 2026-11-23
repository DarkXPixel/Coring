#pragma once

#include "coring5/io/FdOwner.hpp"
#include "coring5/upstream/UpstreamPool.hpp"

#include <array>

namespace Coring5 {

// Worker-local HTTP/1 sockets. Idle fds own no SQEs and hold no backend lease.
class IdleUpstreams {
public:
  [[nodiscard]] FdOwner take(BackendRef backend, TimePoint now) noexcept;
  void park(BackendRef backend, TimePoint expires, FdOwner fd) noexcept;
  void expire(const UpstreamPool &pool, TimePoint now) noexcept;
  void stop() noexcept;

private:
  struct Entry {
    BackendRef backend;
    TimePoint expires;
    FdOwner fd;
  };

  static constexpr std::size_t kPerBackend = 4;
  std::array<Entry, 64> entries_{};
  bool running_{true};
};

} // namespace Coring5
