#pragma once

#include <expected>
#include <string>
namespace Coring2 {
    struct Rule {
        int listen_port{-1};
        struct {
            bool on{false};
            std::string ssl_cert_path;
            std::string ssl_cert_key_path;
        } tls;

        std::string server_name;
        std::string root_dir;

    };

    class Rules {
        Rules() = default;
    public:
        static std::expected<Rules, std::string> create_from_file();
        static Rules create_empty() {
            return Rules();
        }


    private:

    };
}