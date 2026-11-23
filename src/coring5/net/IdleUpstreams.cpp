#include "coring5/net/IdleUpstreams.hpp"

#include <cerrno>
#include <sys/socket.h>

namespace Coring5 {
namespace {

bool same_backend(BackendRef a, BackendRef b) noexcept {
  return a.upstream == b.upstream && a.backend == b.backend &&
         a.epoch == b.epoch;
}

} // namespace

FdOwner IdleUpstreams::take(BackendRef backend, TimePoint now) noexcept {
  for (auto &entry : entries_) {
    if (!entry.fd || !same_backend(backend, entry.backend)) {
      continue;
    }
    auto fd = std::move(entry.fd);
    if (entry.expires <= now) {
      continue;
    }
    // EOF, socket errors and unsolicited bytes make the connection unusable.
    // A close can still race with this probe. The send path owns retry policy.
    char byte;
    if (::recv(fd.get(), &byte, 1, MSG_DONTWAIT | MSG_PEEK) < 0 &&
        (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return fd;
    }
  }
  return {};
}

void IdleUpstreams::park(BackendRef backend, TimePoint expires,
                         FdOwner fd) noexcept {
  if (!running_ || !fd || !backend.valid()) {
    return;
  }
  Entry *free = nullptr;
  std::size_t matches = 0;
  for (auto &entry : entries_) {
    if (!entry.fd) {
      if (!free) {
        free = &entry;
      }
    } else if (same_backend(backend, entry.backend)) {
      ++matches;
    }
  }
  if (free && matches < kPerBackend) {
    *free = {backend, expires, std::move(fd)};
  }
}

void IdleUpstreams::expire(const UpstreamPool &pool, TimePoint now) noexcept {
  for (auto &entry : entries_) {
    if (entry.fd && (entry.expires <= now || !pool.endpoint(entry.backend))) {
      entry.fd.reset();
    }
  }
}

void IdleUpstreams::stop() noexcept {
  running_ = false;
  for (auto &entry : entries_) {
    entry.fd.reset();
  }
}

} // namespace Coring5
