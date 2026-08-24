#include "utility/CliParser.hpp"
#include "coring/StaticConfig.hpp"
#include <expected>


namespace Coring::Utility {
    std::expected<StaticConfig, CliParseError> CliParser::parse_args(int argc, char* argv[]) {
        StaticConfig cfg;
        for(int i = 1; i < argc; ++i) {
            std::string_view arg(argv[i]);

            if((arg == "-c" || arg == "--config") && i + 1 < argc) {
                cfg.config_dir = argv[++i];
            } else {
                return std::unexpected(CliParseError::InvalidArgument);
            }
        }
        return cfg;
    }
}