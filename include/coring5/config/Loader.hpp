#pragma once

#include "coring5/config/Snapshot.hpp"
#include <expected>
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

} // namespace Coring5
