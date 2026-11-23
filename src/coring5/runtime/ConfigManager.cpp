#include "coring5/runtime/ConfigManager.hpp"

#include "coring5/net/ListenSocket.hpp"
#include "coring5/handlers/StaticFileRoot.hpp"
#include <format>

namespace Coring5 {

std::shared_ptr<const ConfigSnapshot> ConfigManager::current() const {
  std::lock_guard lock(mtx_);
  return current_;
}

void ConfigManager::retire_route_text(
    const std::shared_ptr<const ConfigSnapshot> &old) {
  std::erase_if(retired_text_,
                [](const auto &text) { return text.use_count() == 1; });
  if (!old) {
    return;
  }
  // On an ack timeout a worker may still start a response from this snapshot.
  const bool snapshot_in_use = old.use_count() > 1;
  const auto retain = [this, snapshot_in_use](const auto &text) {
    if (text && (snapshot_in_use || text.use_count() > 1)) {
      retired_text_.insert(text);
    }
  };
  for (const auto &[name, set] : old->routes) {
    for (const auto &host : set.vhosts) {
      for (const auto *paths : {&host.exact, &host.prefix}) {
        for (const auto &[path, action] : *paths) {
          if (const auto *respond = std::get_if<RespondRoute>(&action)) {
            retain(respond->text);
          } else if (const auto *redirect = std::get_if<RedirectRoute>(&action)) {
            retain(redirect->text);
          }
        }
      }
    }
  }
}

std::expected<ReloadReport, std::string>
ConfigManager::apply(ParsedConfig parsed) {
  std::lock_guard lock(mtx_);

  // 1-2. Validate and check settings fixed for the process lifetime.
  if (auto v = validate_config(parsed); !v) {
    return std::unexpected(std::move(v.error()));
  }
  if (have_runtime_ && !(parsed.runtime == runtime_)) {
    return std::unexpected("the [runtime] section changed: restart required");
  }
  if (have_runtime_ && parsed.access_log != access_log_) {
    return std::unexpected("the [access_log] section changed: restart required");
  }
  if (have_runtime_ &&
      static_cast<uint64_t>(parsed.limits.max_head_bytes) +
              parsed.runtime.buffer_size >
          runtime_.http1_stage_effective) {
    return std::unexpected(
        "[limits] max_head_bytes + [runtime] buffer_size exceeds the HTTP/1 "
        "stage size workers started with (http1_stage_bytes): restart required");
  }
  if (!have_runtime_ && !workers_.running()) {
    return std::unexpected("workers are not running");
  }
  if (workers_.size() != parsed.runtime.workers) {
    return std::unexpected("worker count mismatch");
  }
  // Pools were sized from the FIRST configuration. Reject, before any worker
  // is touched (invariant I5), a reload whose listeners do not fit them.
  if (auto fit = check_pools_fit(have_runtime_ ? runtime_ : parsed.runtime,
                                 parsed.listeners);
      !fit) {
    return std::unexpected(std::move(fit.error()));
  }

  // 3. stable ids: same name AND address keep their id (and their sockets).
  auto next = std::make_shared<ConfigSnapshot>();
  next->limits = parsed.limits;
  // Upstreams: the name is the identity, so an unchanged name keeps its id.
  for (auto &u : parsed.upstreams) {
    const UpstreamSpec *prev = nullptr;
    if (current_) {
      for (const auto &old : current_->upstreams) {
        if (old.name == u.name) {
          prev = &old;
          break;
        }
      }
    }
    u.id = prev != nullptr ? prev->id : UpstreamId{next_upstream_id_++};
    next->upstreams.push_back(std::move(u));
  }
  next->install_routes(std::move(parsed.routes));
  for (auto &spec : parsed.listeners) {
    if (!spec.upstream.empty()) {
      for (const auto &u :
           next->upstreams) { // validate_config checked it exists
        if (u.name == spec.upstream) {
          spec.upstream_id = u.id;
        }
      }
    }
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
    if (prev != nullptr) {
      // A kept socket retains its bound endpoint, including an ephemeral port.
      spec.endpoint = prev->endpoint;
    }
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
  auto roots = std::make_shared<StaticRoots>();
  for (const auto &[name, set] : next->routes) {
    for (const auto &host : set.vhosts) {
      for (const auto *paths : {&host.exact, &host.prefix}) {
        for (const auto &[path, action] : *paths) {
          const auto *file = std::get_if<StaticRoute>(&action);
          if (!file || roots->directories.contains(file->root)) {
            continue;
          }
          auto root = StaticFileRoot::open(file->root);
          if (!root) {
            return std::unexpected(
                std::format("static root '{}': open failed (errno {})",
                            file->root, root.error().sys));
          }
          roots->directories.emplace(file->root, std::move(*root));
        }
      }
    }
  }
  for (auto &m : msgs) {
    m.roots = roots;
    m.upstreams = UpstreamPool::prepare(*next);
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
    if (spec == nullptr) {
      return std::unexpected(
          std::format("no listener spec for newly added id {}", id.value));
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
  current_roots_ = std::move(roots);
  if (!have_runtime_) {
    runtime_ = parsed.runtime; // keeps the pool capacities workers started with
    access_log_ = std::move(parsed.access_log);
  }
  have_runtime_ = true;
  ++next_generation_;
  retire_route_text(old);
  old.reset();

  if (acked && ack->failures() != 0) {
    return std::unexpected(
        std::format("{} worker(s) failed to apply generation {}",
                    ack->failures(), current_->generation));
  }
  return ReloadReport{current_->generation, std::move(diff), acked};
}

} // namespace Coring5
