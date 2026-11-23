#include "coring5/runtime/Daemon.hpp"

#include <cstring>
#include <format>
#include <new>

namespace Coring5 {

std::expected<void, std::string> Daemon::start() {
  if (workers_.running()) {
    return std::unexpected("daemon already started");
  }
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

  access_writer_.reset();
  std::span<AccessLogRing> access_logs;
  const auto &log = parsed->access_log;
  if (!log.path.empty()) {
    const auto n = parsed->runtime.workers;
    access_logs_.reset(new (std::nothrow) AccessLogRing[n]);
    if (!access_logs_) {
      return std::unexpected("[access_log] could not allocate worker rings");
    }
    access_logs = {access_logs_.get(), n};
    if (auto r = access_writer_.emplace().start(
            log.path.c_str(), access_logs, {log.rotate_bytes, log.rotate_files});
        !r) {
      access_logs_.reset();
      return std::unexpected(std::format("[access_log] '{}': {}", log.path,
                                         std::strerror(r.error().sys)));
    }
  }

  if (auto r = workers_.start(parsed->runtime, access_logs); !r) {
    (void)stop();
    return std::unexpected(r.error());
  }
  config_ = std::make_unique<ConfigManager>(workers_);
  auto applied = config_->apply(std::move(*parsed));
  if (!applied) {
    (void)stop();
    return std::unexpected(applied.error());
  }

  control_ = std::make_unique<ControlServer>(
      opts_.control_socket,
      ControlHandlers{.reload = [this] { return reload(); },
                      .stats = [this] { return stats(); },
                      .status = [this] { return status(); },
                      .shutdown = [this] { request_stop(); }});
  if (auto err = control_->start(); !err.empty()) {
    (void)stop();
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
  return std::format(
      "OK generation={} listeners_added={} removed={} "
      "limits_changed={} upstreams_changed={} routes_changed={} acked={}",
      r->generation, r->diff.added.size(), r->diff.removed.size(),
      r->diff.limits_changed, r->diff.upstreams_changed, r->diff.routes_changed,
      r->workers_acked);
}

std::string Daemon::stats() const {
  return render_metrics(workers_.metrics()) +
         std::format("coring_access_log_sink_errno {}\n",
                     access_writer_ ? access_writer_->error() : 0);
}

std::string Daemon::status() const {
  if (!config_ || !config_->current()) {
    return "starting";
  }
  const auto snap = config_->current();
  return std::format("running workers={} generation={} listeners={}",
                     workers_.size(), snap->generation, snap->listeners.size());
}

Result<> Daemon::stop() {
  if (control_) {
    control_->stop();
  }
  workers_.shutdown(drain_timeout_);
  auto result = access_writer_ ? access_writer_->stop() : Result<>{};
  access_logs_.reset();
  return result;
}

} // namespace Coring5
