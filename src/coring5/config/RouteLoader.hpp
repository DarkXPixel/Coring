#pragma once

#include "coring5/config/Snapshot.hpp"
#include <expected>
#include <toml++/toml.hpp>

namespace Coring5 {

std::expected<RouteSets, std::string> parse_routes(const toml::table &root);
std::expected<void, std::string> validate_routes(const ParsedConfig &cfg);
bool canonical_route_path(std::string_view path);
bool valid_route_host(std::string_view name) noexcept;

} // namespace Coring5
