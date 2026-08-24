#pragma once

#include <string>
namespace Coring {
    struct StaticConfig {
        bool use_epoll{true};
        std::string_view config_dir{"./coring/configs"};
        std::string_view static_dir{"./coring/www"};
    };
}