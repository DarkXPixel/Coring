#pragma once
#include "coring/StaticConfig.hpp"
#include <expected>

namespace Coring::Utility {
    enum class CliParseError {
        InvalidArgument,
    };

    constexpr std::string_view to_string(CliParseError err) noexcept {
        switch (err) {
            case CliParseError::InvalidArgument: return "Invalid argument"; 
        }
        return "Unknown";
    }

    class CliParser {
    public:
        static std::expected<StaticConfig, CliParseError> parse_args(int argc, char* argv[]);    
    };
}