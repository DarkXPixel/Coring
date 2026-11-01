#include "coring5/runtime/ConfigManager.hpp"

#include "coring5/net/ListenSocket.hpp"
#include <format>

namespace Coring5 {

std::shared_ptr<const ConfigSnapshot> ConfigManager::current() const {
  std::lock_guard lock(mtx_);
  return current_;
}

std::expected<ReloadReport, std::string>
ConfigManager::apply(ParsedConfig parsed) {
  std::lock_guard lock(mtx_);

  // 1-2. validate; the runtime section is fixed for the process lifetime.
  if (auto v = validate_config(parsed); !v) {
    return std::unexpected(std::move(v.error()));
  }
  if (have_runtime_ && !(parsed.runtime == runtime_)) {
    return std::unexpected(
        "the [runtime] section changed: restart required (hot reload covers "
        "[limits] and [[listener]] only)");
  }
  if (!have_runtime_ && !workers_.running()) {
    return std::unexpected("workers are not running");
  }
  if (workers_.size() != parsed.runtime.workers) {
    return std::unexpected("worker count mismatch");
  }

  // 3. stable ids: same name AND address keep their id (and their sockets).
  auto next = std::make_shared<ConfigSnapshot>();
  next->limits = parsed.limits;
  for (auto &spec : parsed.listeners) {
    const ListenerSpec *prev = nullptr;
    if (current_) {
      for (const auto &old : current_->listeners) {
        if (old.name == spec.name && old.address == spec.address) {
          prev = &old;
          break;
        }
      }
    }
    spec.id = prev != nullptr ? prev->id : ListenerId{next_listener_id_++};
    next->listeners.push_back(std::move(spec));
  }
  next->generation = next_generation_;

  ConfigSnapshot empty;
  ConfigDiff diff = diff_snapshots(current_ ? *current_ : empty, *next);

  // 4. sockets for NEW listeners, one per worker. Anything that can fail
  //    happens here, before a single worker is touched (invariant I5).
  const std::size_t n = workers_.size();
  std::vector<ApplyConfig> msgs(n);
  auto ack = std::make_shared<ReloadAck>(static_cast<uint32_t>(n));
  for (auto &m : msgs) {
    m.snapshot = next;
    m.remove = diff.removed;
    m.ack = ack;
  }
  for (ListenerId id : diff.added) {
    ListenerSpec *spec = nullptr;
    for (auto &l : next->listeners) {
      if (l.id == id) {
        spec = &l;
      }
    }
    Endpoint ep = spec->endpoint;
    for (std::size_t w = 0; w < n; ++w) {
      auto fd = create_tcp_listener(ep, ListenOptions{});
      if (!fd) {
        return std::unexpected(
            std::format("listener '{}' ({}): cannot bind (errno {})",
                        spec->name, spec->address, fd.error().sys));
      }
      if (w == 0 && ep.port() == 0) {
        // Port 0: every worker must share the SAME ephemeral port.
        if (auto bound = local_endpoint(fd->get())) {
          ep = *bound;
          spec->endpoint = *bound;
        }
      }
      msgs[w].add.push_back(ListenerFd{id, std::move(*fd)});
    }
  }

  // 5-6. publish.
  for (std::size_t w = 0; w < n; ++w) {
    workers_.mailbox(w).post(std::move(msgs[w]));
  }

  // 7. wait. A timeout is reported; workers still converge on their own.
  const bool acked = ack->wait_for(parsed.runtime.reload_ack_timeout);

  // 8. retire the old snapshot on THIS thread.
  auto old = std::move(current_);
  current_ = std::move(next);
  runtime_ = parsed.runtime;
  have_runtime_ = true;
  ++next_generation_;
  old.reset();

  if (acked && ack->failures() != 0) {
    return std::unexpected(
        std::format("{} worker(s) failed to apply generation {}",
                    ack->failures(), current_->generation));
  }
  return ReloadReport{current_->generation, std::move(diff), acked};
}

} // namespace Coring5
