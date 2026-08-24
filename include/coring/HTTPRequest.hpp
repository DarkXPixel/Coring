#pragma once

#include <algorithm>
#include <cctype>
#include <optional>
#include <string_view>
#include <vector>
#include <memory_resource>


namespace Coring {
    namespace HTTP1_1 {
        enum class Method {
            GET,
            POST
        };
        
        struct Header {
            std::string_view name;
            std::string_view value;
        };

        class Headers {
        private:
            std::vector<Header> headers_;

            static bool iequals(std::string_view a, std::string_view b) {
                return std::ranges::equal(a, b, [](char ca, char cb) -> bool {
                    return std::tolower(ca) == std::tolower(cb);
                });
            }

        public:
            Headers() = default;

            void add(std::string_view name, std::string_view value) {
                headers_.push_back({name, value});
            }

            std::optional<std::string_view> get(std::string_view name) const {
                for(const auto& h : headers_) {
                    if(iequals(h.name, name)) {
                        return h.value;
                    }
                }
                return std::nullopt;
            }

            bool contains(std::string_view name) const {
                return get(name).has_value();
            }


            auto begin() const {
                return headers_.begin();
            }

            auto end() const {
                return headers_.end();
            }

            void clear() {
                headers_.clear();
            }

            void reserve(size_t capacity) {
                headers_.reserve(capacity);
            }
        };

        struct Request {
            Method method;
            std::string_view path;
            Headers headers;
            std::string_view body;
        };
    }
}