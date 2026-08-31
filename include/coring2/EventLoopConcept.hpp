#pragma once


#include <concepts>
#include <cstdint>
#include <optional>
#include <string>
namespace Coring {
    template<typename EventLoop>
    concept EventLoopConcept = requires(EventLoop loop, uint32_t queue_depth) {
        {loop.init(queue_depth)} -> std::same_as<std::optional<std::string>>;
        {loop.submit_read()};
    };
}