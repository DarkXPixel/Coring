#pragma once
#include <utility>
namespace Coring2::Utility {
    template<typename F>
    class Defer {
        F func;
    public:
        Defer(F&& f) noexcept : func(std::forward<F>(f)) {}
        ~Defer() {func();}
    };
}