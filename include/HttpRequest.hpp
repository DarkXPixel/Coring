#pragma once

#include <cctype>
#include <string>
#include <string_view>
#include <unordered_map>
#include <format>
#include <chrono>

namespace Coring {
    struct HttpRequest {
        std::string method;
        std::string path;
        std::unordered_map<std::string, std::string> headers;
        std::string body;
    };

    inline HttpRequest parse_http_headers(std::string_view raw) {
        HttpRequest req;

        size_t line_end = raw.find("\r\n");
        if(line_end == std::string_view::npos) {
            return req;
        }

        std::string_view start_line = raw.substr(0, line_end);

        size_t m_end = start_line.find(' ');
        size_t p_end = start_line.find(' ', m_end+1);

        if(m_end != std::string_view::npos && p_end != std::string_view::npos) {
            req.method = std::string(start_line.substr(0, m_end));
            req.path = std::string(start_line.substr(m_end + 1, p_end - m_end - 1));
        }

        size_t pos = line_end + 2;
        while(pos < raw.size()) {
            size_t next_line = raw.find("\r\n", pos);
            if(next_line == std::string_view::npos || next_line == pos) {
                break;
            }

            std::string_view line = raw.substr(pos, next_line - pos);
            size_t colon = line.find(':');
            if(colon != std::string_view::npos) {
                std::string key(line.substr(0, colon));
                std::string value(line.substr(colon + 1));
                size_t val_start = value.find_first_not_of(" \t");
                if(val_start != std::string_view::npos) {
                    value = value.substr(val_start);
                }
                for(char& c : key) {
                    c = std::tolower(c);
                }
                req.headers[key] = value;
            }
            pos = next_line + 2;
        }
        return req;
    }

    [[nodiscard]] inline constexpr std::string get_http_413_response() {
        const auto now = std::chrono::system_clock::now(); 
        return std::format(
            R"(HTTP/1.1 413 Payload Too Large
            Date: {:%a, %d %b %Y %H:%M:%S GMT}
            Server: custom-cpp-server/1.0
            Content-Type: application/json; charset=utf-8
            Content-Length: 85
            Connection: close

            {{
            "error": "Payload Too Large",
            "message": "Payload too large"
            }})", now);
    }
}
