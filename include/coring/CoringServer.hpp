#pragma once
#include <string>
#include <variant>


namespace Coring {
    enum class HandlerType {
        StaticFile,
        HealthCheck,
        NotFound,
        Status,
    };

    struct StaticFileRule {
        std::string root;
        bool is_single_file{false};
        std::string index;
    };

    struct RouteRule {
        std::string path;
        HandlerType type;
        std::variant<std::monostate, StaticFileRule> rule;
    };


    class CoringServer {
    public:

    };

}