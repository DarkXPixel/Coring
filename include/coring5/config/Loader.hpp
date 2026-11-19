#pragma once

#include "coring5/config/Snapshot.hpp"
#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace Coring5 {

// Cold path: errors are human-readable strings.
using ConfigResult = std::expected<ParsedConfig, std::string>;

// Parses TOML text (see docs/CONFIG_AND_RELOAD.md for the schema). Syntax and
// type errors only; semantic checks are validate().
[[nodiscard]] ConfigResult parse_config(std::string_view toml_text);
[[nodiscard]] ConfigResult load_config_file(const std::string &path);

// Semantic checks: duplicate names/addresses, unsupported protocols, limit
// sanity, pool sizes. Returns the first problem found.
[[nodiscard]] std::expected<void, std::string>
validate_config(const ParsedConfig &cfg);

// Pure: effective per-worker pool capacities.
//   * a pool with an explicit [runtime.pools] size gets exactly that;
//   * otherwise it gets the sum of its listeners' `max_connections`;
//   * listeners without `max_connections` share what is left of
//     max_connections_per_worker, split evenly between the protocols that have
//     such listeners;
//   * a protocol with no listeners gets capacity 0 (no memory is allocated).
// Never fails; validate_config() reports an impossible result.
[[nodiscard]] PoolCapacities
derive_pool_capacities(const RuntimeConfig &rt,
                       std::span<const ListenerSpec> listeners);

// Reload gate (control thread, before any worker is touched, invariant I5):
// pools are sized when the process starts, so every listener of a new
// configuration must find a pool of its protocol with capacity > 0, and the
// explicit `max_connections` of a protocol must fit its pool.
// `running.pool_capacity` holds the capacities the workers were started with.
[[nodiscard]] std::expected<void, std::string>
check_pools_fit(const RuntimeConfig &running,
                std::span<const ListenerSpec> listeners);

} // namespace Coring5
