#include "coring5/upstream/UpstreamPool.hpp"

#include <algorithm>
#include <cerrno>
#include <utility>

namespace Coring5 {

UpstreamPool::Entry *UpstreamPool::find(UpstreamId id) noexcept {
  for (auto &e : entries_) {
    if (e.spec.id == id) {
      return &e;
    }
  }
  return nullptr;
}

const UpstreamPool::Entry *UpstreamPool::find(UpstreamId id) const noexcept {
  for (const auto &e : entries_) {
    if (e.spec.id == id) {
      return &e;
    }
  }
  return nullptr;
}

UpstreamPool::Entry *UpstreamPool::resolve(BackendRef ref) noexcept {
  if (!ref.valid()) {
    return nullptr;
  }
  Entry *e = find(ref.upstream);
  return (e != nullptr && e->epoch == ref.epoch &&
          ref.backend < e->state.size())
             ? e
             : nullptr;
}

const UpstreamPool::Entry *
UpstreamPool::resolve(BackendRef ref) const noexcept {
  return const_cast<UpstreamPool *>(this)->resolve(ref);
}

void UpstreamPool::apply_config(const ConfigSnapshot &snapshot) {
  std::vector<Entry> next;
  next.reserve(snapshot.upstreams.size());
  for (const auto &spec : snapshot.upstreams) {
    Entry *old = find(spec.id);
    if (old != nullptr && old->spec == spec) {
      next.push_back(std::move(*old)); // unchanged: keep state AND epoch
      continue;
    }
    Entry e;
    e.spec = spec;
    e.state.assign(spec.backends.size(), BackendState{});
    e.epoch = ++epoch_counter_;
    if (epoch_counter_ ==
        0) { // 2^32 reloads of changed upstreams: skip invalid
      e.epoch = ++epoch_counter_;
    }
    next.push_back(std::move(e));
  }
  entries_ = std::move(next);
}

uint32_t UpstreamPool::select(Entry &e, TimePoint now,
                              std::span<const uint32_t> exclude,
                              bool honour_ejection) noexcept {
  const auto n = static_cast<uint32_t>(e.state.size());
  if (n == 0) {
    return kNone;
  }
  const uint32_t start = e.rr_cursor % n;
  uint32_t best = kNone;
  for (uint32_t k = 0; k < n; ++k) {
    const uint32_t i = (start + k) % n;
    if (std::find(exclude.begin(), exclude.end(), i) != exclude.end()) {
      continue;
    }
    if (honour_ejection && is_ejected(e.state[i], now)) {
      continue;
    }
    if (e.spec.policy == LbPolicy::RoundRobin) {
      best = i;
      break; // first eligible in rotation order
    }
    // Least connections; strict '<' keeps the rotation order for ties.
    if (best == kNone || e.state[i].active < e.state[best].active) {
      best = i;
    }
  }
  if (best != kNone) {
    e.rr_cursor = (best + 1) % n;
  }
  return best;
}

Result<BackendRef>
UpstreamPool::pick(UpstreamId id, TimePoint now,
                   std::span<const uint32_t> exclude) noexcept {
  Entry *e = find(id);
  if (e == nullptr) {
    return unexpected_error(Errc::UpstreamUnavailable, ENOENT);
  }
  bool panic = false;
  uint32_t i = select(*e, now, exclude, /*honour_ejection=*/true);
  if (i == kNone) {
    // Everything left is ejected: serve anyway rather than refuse all traffic.
    i = select(*e, now, exclude, /*honour_ejection=*/false);
    panic = i != kNone;
  }
  if (i == kNone) {
    return unexpected_error(Errc::UpstreamUnavailable, 0);
  }
  return BackendRef{id, i, e->epoch, panic};
}

const Endpoint *UpstreamPool::endpoint(BackendRef ref) const noexcept {
  const Entry *e = resolve(ref);
  return e != nullptr ? &e->spec.backends[ref.backend] : nullptr;
}

uint32_t UpstreamPool::backend_count(UpstreamId id) const noexcept {
  const Entry *e = find(id);
  return e != nullptr ? static_cast<uint32_t>(e->spec.backends.size()) : 0U;
}

Millis UpstreamPool::connect_timeout(UpstreamId id,
                                     Millis fallback) const noexcept {
  const Entry *e = find(id);
  return (e != nullptr && e->spec.connect_timeout.count() > 0)
             ? e->spec.connect_timeout
             : fallback;
}

void UpstreamPool::acquire(BackendRef ref) noexcept {
  if (Entry *e = resolve(ref); e != nullptr) {
    ++e->state[ref.backend].active;
  }
}

void UpstreamPool::release(BackendRef ref) noexcept {
  if (Entry *e = resolve(ref); e != nullptr) {
    auto &a = e->state[ref.backend].active;
    if (a > 0) {
      --a;
    }
  }
}

bool UpstreamPool::report(BackendRef ref, bool ok, TimePoint now) noexcept {
  Entry *e = resolve(ref);
  if (e == nullptr) {
    return false;
  }
  BackendState &s = e->state[ref.backend];
  if (ok) {
    s.consecutive_fails = 0;
    s.ever_ejected = false;
    return false;
  }
  if (e->spec.max_fails == 0) {
    return false; // ejection disabled
  }
  if (s.consecutive_fails < UINT32_MAX) {
    ++s.consecutive_fails;
  }
  // Kept (not reset) after ejection: once the time passes, one more failure
  // re-ejects at once.
  if (s.consecutive_fails >= e->spec.max_fails && !is_ejected(s, now)) {
    s.ejected_until = now + e->spec.eject_time;
    s.ever_ejected = true;
    return true;
  }
  return false;
}

uint32_t UpstreamPool::active(BackendRef ref) const noexcept {
  const Entry *e = resolve(ref);
  return e != nullptr ? e->state[ref.backend].active : 0U;
}

bool UpstreamPool::ejected(BackendRef ref, TimePoint now) const noexcept {
  const Entry *e = resolve(ref);
  return e != nullptr && is_ejected(e->state[ref.backend], now);
}

} // namespace Coring5
