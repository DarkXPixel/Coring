#pragma once

// Per-worker routing state. Owned and touched by ONE worker thread only, so
// nothing here is atomic or locked.
//
// Why this is fast:
//   * the request path reads a plain pointer (`table_`) and a plain array
//     (`bindings_`); the compiled table is immutable, so there is nothing to
//     synchronise with;
//   * a table update is a single pointer swap done by the worker itself,
//     between two io_uring completions - readers and the writer are the same
//     thread. No RCU grace periods, no hazard pointers, no per-request
//     shared_ptr copies;
//   * the table is compiled once by the control plane and shared read-only by
//     all workers (read-only sharing costs nothing on the cache-coherence
//     side); the only atomic traffic is the shared_ptr refcount, touched once
//     per worker per update.
//
// Rules for callers:
//   * `Match::action` / `Match::path` etc. are valid until the next
//     install() on this worker, i.e. until you return to the event loop.
//     Copy what you need into the request context BEFORE submitting async I/O.
//     (If you really must keep the table alive across completions, use pin().)
//   * Sessions store a `RouteBinding` (4 bytes), never a RouteSetId or a
//     pointer into a table: bindings are stable across updates, so
//     keep-alive connections see new routes on their very next request.

#include "coring4/routing/RouteTable.hpp"
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Coring4 {

// Stable per-worker handle of a listener's route set. Index into
// WorkerRouting::bindings_.
struct RouteBinding {
  static constexpr uint32_t kInvalid = 0xFFFFFFFFu;
  uint32_t value{kInvalid};
  [[nodiscard]] bool is_valid() const noexcept { return value != kInvalid; }
};

class WorkerRouting {
public:
  // Registers a listener's route set by name (cold path, e.g. when a listener
  // is added). Resolves immediately if a table is already installed.
  RouteBinding bind(std::string name) {
    for (std::size_t i = 0; i < bindings_.size(); ++i) {
      if (bindings_[i].name == name) {
        return RouteBinding{static_cast<uint32_t>(i)};
      }
    }
    Binding b;
    b.name = std::move(name);
    if (table_) {
      b.id = table_->find_route_set(b.name);
    }
    bindings_.push_back(std::move(b));
    return RouteBinding{static_cast<uint32_t>(bindings_.size() - 1)};
  }

  // Names that must exist in every table installed on this worker.
  [[nodiscard]] std::vector<std::string> bound_names() const {
    std::vector<std::string> r;
    r.reserve(bindings_.size());
    for (const Binding &b : bindings_) {
      r.push_back(b.name);
    }
    return r;
  }

  // Swaps in a new table and re-resolves every binding. Rejects (and keeps
  // the old table) if some bound route set is missing: a listener must never
  // lose its routes because of a bad update.
  [[nodiscard]] std::expected<void, std::string>
  install(std::shared_ptr<const RouteTable> next) {
    if (!next) {
      return std::unexpected("null route table");
    }
    std::vector<RouteSetId> ids;
    ids.reserve(bindings_.size());
    for (const Binding &b : bindings_) {
      const RouteSetId id = next->find_route_set(b.name);
      if (!id.is_valid()) {
        return std::unexpected("route set '" + b.name +
                               "' is missing from the new table");
      }
      ids.push_back(id);
    }
    for (std::size_t i = 0; i < ids.size(); ++i) {
      bindings_[i].id = ids[i];
    }
    table_ = std::move(next);
    return {};
  }

  // Hot path.
  [[nodiscard]] Match match(RouteBinding binding, std::string_view host,
                            std::string_view target) noexcept {
    if (table_ == nullptr || binding.value >= bindings_.size()) [[unlikely]] {
      Match m;
      m.status = MatchStatus::Unavailable;
      return m;
    }
    return table_->match(bindings_[binding.value].id, host, target, scratch_);
  }

  // Keeps the current table alive beyond the next install(). One atomic
  // increment; use only when an async operation really needs the action.
  [[nodiscard]] std::shared_ptr<const RouteTable> pin() const noexcept {
    return table_;
  }

  [[nodiscard]] const RouteTable *table() const noexcept {
    return table_.get();
  }
  [[nodiscard]] uint32_t generation() const noexcept {
    return table_ ? table_->generation() : 0;
  }

private:
  struct Binding {
    std::string name;
    RouteSetId id{};
  };

  std::shared_ptr<const RouteTable> table_;
  std::vector<Binding> bindings_;
  MatchScratch scratch_;
};

} // namespace Coring4
