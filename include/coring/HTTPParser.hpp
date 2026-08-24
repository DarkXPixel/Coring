#pragma once
#include <expected>
#include <span>
#include <string_view>
#include "coring/HTTPRequest.hpp"


namespace Coring {

    namespace HTTP1_1 {
        enum class ParserError {
            Ok,
            NotEnough,
            BodyChunk
            //Timeout,
        };

        class Parser {
        public:
            std::expected<Request, ParserError> parse(std::string_view in);
            std::span<const char> get_body_chunk();

        };
    }
}