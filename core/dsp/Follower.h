#pragma once

#include "Common.h"

namespace lab::dsp
{
    // Peak envelope follower with separate attack and release (the dual envelopes of LAB X-0's transient shaper).
    struct Follower
    {
        float env = 0.0f, attackCoef = 1.0f, releaseCoef = 0.001f;

        void set (float attackSeconds, float releaseSeconds, float sampleRate) noexcept
        {
            attackCoef = onePoleCoef (std::max (1.0e-5f, attackSeconds), sampleRate);
            releaseCoef = onePoleCoef (std::max (1.0e-4f, releaseSeconds), sampleRate);
        }

        void reset() noexcept { env = 0.0f; }

        float process (float x) noexcept
        {
            const float a = std::abs (x);
            env += (a - env) * (a > env ? attackCoef : releaseCoef);
            return env;
        }

        float processStereo (float l, float r) noexcept { return process (std::max (std::abs (l), std::abs (r))); }

        // The envelope normalised to 0..1 over -60..0 dBFS.
        static float normalised (float envelope) noexcept
        {
            return std::clamp ((gainToDb (envelope) + 60.0f) / 60.0f, 0.0f, 1.0f);
        }
    };
}
