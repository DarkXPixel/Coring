#include "coring5/obs/Metrics.hpp"

#include <format>

namespace Coring5 {

namespace {

template <class F> uint64_t sum(std::span<const WorkerMetrics> s, F &&f) {
  uint64_t n = 0;
  for (const auto &m : s) {
    n += get(f(m));
  }
  return n;
}

} // namespace

std::string render_metrics(std::span<const WorkerMetrics> shards) {
  std::string out;
  auto line = [&](std::string_view name, uint64_t v) {
    out += std::format("coring_{} {}\n", name, v);
  };
  line("workers", shards.size());
  line("accepts_total",
       sum(shards, [](auto &m) -> auto & { return m.accepts; }));
  line("accept_errors_total",
       sum(shards, [](auto &m) -> auto & { return m.accept_errors; }));
  line("connections_rejected_total",
       sum(shards, [](auto &m) -> auto & { return m.conns_rejected; }));
  line("connections_live",
       sum(shards, [](auto &m) -> auto & { return m.conns_live; }));
  line("stale_events_total",
       sum(shards, [](auto &m) -> auto & { return m.stale_events; }));
  line("nobufs_total", sum(shards, [](auto &m) -> auto & { return m.nobufs; }));
  line("sq_full_total",
       sum(shards, [](auto &m) -> auto & { return m.sq_full; }));
  line("cq_overflow_total",
       sum(shards, [](auto &m) -> auto & { return m.cq_overflow; }));
  line("bytes_in_total",
       sum(shards, [](auto &m) -> auto & { return m.bytes_in; }));
  line("bytes_out_total",
       sum(shards, [](auto &m) -> auto & { return m.bytes_out; }));
  line("loop_iterations_total",
       sum(shards, [](auto &m) -> auto & { return m.loop_iterations; }));
  line("cqes_total", sum(shards, [](auto &m) -> auto & { return m.cqes; }));
  line("buffers_free",
       sum(shards, [](auto &m) -> auto & { return m.buffers_free; }));

  for (std::size_t i = 1; i < kErrcCount; ++i) {
    const uint64_t n =
        sum(shards, [i](auto &m) -> auto & { return m.closes[i]; });
    if (n != 0) {
      out += std::format("coring_closes_total{{reason=\"{}\"}} {}\n",
                         to_string(static_cast<Errc>(i)), n);
    }
  }

  uint64_t worst_p999 = 0;
  for (const auto &m : shards) {
    worst_p999 = std::max(worst_p999, m.loop_ns.quantile_upper_bound(0.999));
  }
  line("loop_ns_p999_upper_bound_max", worst_p999);
  return out;
}

} // namespace Coring5
