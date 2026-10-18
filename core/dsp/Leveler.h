#pragma once

#include "Common.h"
#include "SelfTest.h"

namespace lab::dsp
{
    // M1 placeholder: a pass-through LevelMatcher so the chain, the harness and the gate tests build. Milestone M2
    // replaces it with the real matcher (handoff section 6).
    class LevelMatcher
    {
    public:
        struct Settings { float amount = 1.0f; };
        void prepare (double) {}
        void reset() noexcept {}
        void set (const Settings&) noexcept {}
        void process (float*, float*, const float*, int) noexcept {}
        float gainDb() const noexcept { return 0.0f; }
        static SelfTestReport selfTest() { return {}; }
    };
}
