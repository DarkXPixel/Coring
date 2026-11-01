#pragma once

// Control-plane side of routing: compiles a RoutingConfig once and rolls the
// resulting immutable table out to every worker.
//
//   admin API / config reload
//        |  apply(config)
//        v
//   compile (validate; nothing is touched on error)
//        |  shared_ptr<const RouteTable>
//        v
//   WorkerChannel x N  ->  worker thread: WorkerRouting::install()   (swap)
//
// Guarantees:
//   * a broken config is rejected as a whole; the running table stays;
//   * workers never block on this: they just see one more manage request;
//   * a worker that is attached later gets the current table immediately, and
//     cannot miss a concurrent update (attach and apply share one mutex);
//   * apply() returns an ack that can be awaited if the caller wants to know
//     when every worker runs the new table (updates are eventually
//     consistent across workers; there is a short window where worker A
//     serves the new table and worker B the old one).

#include "coring4/ManageService.hpp"
#include "coring4/routing/RouteTable.hpp"
#include "coring4/routing/RouteUpdate.hpp"
#include "coring4/routing/RoutingConfig.hpp"
#include <algorithm>
#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace Coring4 {

class RoutingControl {
public:
  explicit RoutingControl(Manage::ManageService &manage) : manage_(manage) {}

  // A route set that live listeners point at. Every later apply() must still
  // contain it, otherwise it is rejected.
  void require_route_set(std::string name) {
    std::lock_guard lock(mtx_);
    if (std::ranges::find(required_, name) == required_.end()) {
      required_.push_back(std::move(name));
    }
  }

  // Compiles and rolls out `config`. `resolve_upstream` maps upstream pool
  // names to ids (needed only if the config contains proxy actions).
  [[nodiscard]] std::expected<std::shared_ptr<RouteUpdateAck>, std::string>
  apply(RoutingConfig config, CompileOptions options = {}) {
    std::lock_guard lock(mtx_);
    options.required_route_sets.insert(options.required_route_sets.end(),
                                       required_.begin(), required_.end());
    auto table = RouteTable::compile(config, options);
    if (!table) {
      return std::unexpected(std::move(table.error()));
    }
    table_ = *table;
    config_ = std::move(config);

    auto ack = std::make_shared<RouteUpdateAck>(
        static_cast<uint32_t>(channels_.size()));
    for (const auto &ch : channels_) {
      ch->push(make_update(table_, ack));
    }
    return ack;
  }

  // Registers a worker. It receives the current table right away (if any).
  void attach_worker(const std::shared_ptr<Manage::WorkerChannel> &channel) {
    std::lock_guard lock(mtx_);
    manage_.add_channel(channel);
    channels_.push_back(channel);
    if (table_) {
      channel->push(make_update(table_, nullptr));
    }
  }

  [[nodiscard]] std::shared_ptr<const RouteTable> current() const {
    std::lock_guard lock(mtx_);
    return table_;
  }

  [[nodiscard]] RoutingConfig current_config() const {
    std::lock_guard lock(mtx_);
    return config_;
  }

private:
  static Manage::ManageRequest
  make_update(std::shared_ptr<const RouteTable> table,
              std::shared_ptr<RouteUpdateAck> ack) {
    Manage::ManageRequest req;
    req.request = Manage::UpdateRoutesRequest{std::move(table), std::move(ack)};
    return req;
  }

  Manage::ManageService &manage_;
  mutable std::mutex mtx_;
  std::shared_ptr<const RouteTable> table_;
  RoutingConfig config_;
  std::vector<std::string> required_;
  std::vector<std::shared_ptr<Manage::WorkerChannel>> channels_;
};

} // namespace Coring4
