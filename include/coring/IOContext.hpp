#pragma once
#include "coring/IOEngine.hpp"
#include "coring/StaticConfig.hpp"

namespace Coring {
    class Coring {
    public:
        Coring(const StaticConfig& cfg);
        void run();

    private:
        template<IOEngineConcept Engine>
        void impl_run();
    private:
        const bool uring_available_{false};
        StaticConfig cfg_;
    };
}