#include "coring5/runtime/Daemon.hpp"

#include <format>

namespace Coring5 {

std::expected<void, std::string> Daemon::start() {
  auto parsed = load_config_file(opts_.config_path);
  if (!parsed) {
    return std::unexpected(parsed.error());
  }
  if (auto v = validate_config(*parsed); !v) {
    return std::unexpected(v.error());
  }
  if (opts_.check_only) {
    return {};
  }
  drain_timeout_ = parsed->runtime.drain_timeout;

  if (auto r = workers_.start(parsed->runtime); !r) {
    return std::unexpected(r.error());
  }
  config_ = std::make_unique<ConfigManager>(workers_);
  auto applied = config_->apply(std::move(*parsed));
  if (!applied) {
    workers_.shutdown(Millis{1000});
    return std::unexpected(applied.error());
  }

  control_ = std::make_unique<ControlServer>(
      opts_.control_socket,
      ControlHandlers{.reload = [this] { return reload(); },
                      .stats = [this] { return stats(); },
                      .status = [this] { return status(); },
                      .shutdown = [this] { request_stop(); }});
  if (auto err = control_->start(); !err.empty()) {
    workers_.shutdown(Millis{1000});
    return std::unexpected(err);
  }
  return {};
}

std::string Daemon::reload() {
  auto parsed = load_config_file(opts_.config_path);
  if (!parsed) {
    return "ERR " + parsed.error();
  }
  auto r = config_->apply(std::move(*parsed));
  if (!r) {
    return "ERR " + r.error() +
           " (running configuration unchanged or partially applied; see "
           "status)";
  }
  return std::format("OK generation={} listeners_added={} removed={} "
                     "limits_changed={} upstreams_changed={} acked={}",
                     r->generation, r->diff.added.size(),
                     r->diff.removed.size(), r->diff.limits_changed,
                     r->diff.upstreams_changed, r->workers_acked);
}

std::string Daemon::stats() const { return render_metrics(workers_.metrics()); }

std::string Daemon::status() const {
  if (!config_ || !config_->current()) {
    return "starting";
  }
  const auto snap = config_->current();
  return std::format("running workers={} generation={} listeners={}",
                     workers_.size(), snap->generation, snap->listeners.size());
}

void Daemon::stop() {
  if (control_) {
    control_->stop();
  }
  workers_.shutdown(drain_timeout_);
}

} // namespace Coring5
