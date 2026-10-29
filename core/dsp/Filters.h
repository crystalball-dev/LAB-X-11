#pragma once

#include "Common.h"

namespace lab::dsp
{
    // Topology-preserving state-variable filter (Zavalishin), from LAB X-10. Stable under per-sample modulation.
    struct SVF
    {
        float ic1 = 0.0f, ic2 = 0.0f;
        float g = 0.0f, k = 2.0f, a1 = 1.0f, a2 = 0.0f, a3 = 0.0f;

        void reset() noexcept { ic1 = ic2 = 0.0f; }

        void set (float newG, float newK) noexcept
        {
            g = newG;
            k = newK;
            a1 = 1.0f / (1.0f + g * (g + k));
            a2 = g * a1;
            a3 = g * a2;
        }

        // Cutoff in Hz and Q (k = 1 / Q).
        void setHz (float hz, float q, float sampleRate) noexcept { set (gFor (hz, sampleRate), 1.0f / std::max (0.05f, q)); }

        static float gFor (float hz, float sampleRate) noexcept
        {
            const float f = std::clamp (hz, 10.0f, sampleRate * 0.45f);
            return std::tan (pi * f / sampleRate);
        }

        void process (float v0, float& lp, float& bp, float& hp) noexcept
        {
            const float v3 = v0 - ic2;
            const float v1 = a1 * ic1 + a2 * v3;
            const float v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.0f * v1 - ic1;
            ic2 = 2.0f * v2 - ic2;
            lp = v2;
            bp = v1;
            hp = v0 - k * v1 - v2;
        }

        float lowpass (float v0) noexcept  { float lp, bp, hp; process (v0, lp, bp, hp); return lp; }
        float highpass (float v0) noexcept { float lp, bp, hp; process (v0, lp, bp, hp); return hp; }
        // Band-pass with unity peak gain (the SVF's band output scaled by k).
        float bandpass (float v0) noexcept { float lp, bp, hp; process (v0, lp, bp, hp); return bp * k; }
    };

    // Fourth-order Butterworth low- or high-pass, 24 dB per octave: two state-variable stages (Q 0.541 and 1.307).
    struct Butter4
    {
        SVF a, b;
        bool high = false;

        void setHz (float hz, bool highPass, float sampleRate) noexcept
        {
            high = highPass;
            a.setHz (hz, 0.5412f, sampleRate);
            b.setHz (hz, 1.3066f, sampleRate);
        }
        void reset() noexcept { a.reset(); b.reset(); }
        float process (float x) noexcept { return high ? b.highpass (a.highpass (x)) : b.lowpass (a.lowpass (x)); }
    };

    // One-pole low-pass / high-pass.
    struct OnePole
    {
        float z = 0.0f, coef = 1.0f;
        void reset (float v = 0.0f) noexcept { z = v; }
        void setHz (float hz, float sampleRate) noexcept { coef = onePoleCoefHz (std::clamp (hz, 1.0f, sampleRate * 0.45f), sampleRate); }
        float lowpass (float x) noexcept { z += (x - z) * coef; return z; }
        float highpass (float x) noexcept { z += (x - z) * coef; return x - z; }
    };

    // DC blocker (one-pole high-pass around 5-10 Hz), for chains that make DC (rectifiers, biased shapers).
    struct DcBlocker
    {
        float in = 0.0f, out = 0.0f, r = 0.9995f;
        void prepare (float hz, float sampleRate) noexcept { r = 1.0f - twoPi * hz / sampleRate; }
        void reset() noexcept { in = out = 0.0f; }
        float process (float x) noexcept
        {
            const float y = x - in + r * out;
            in = x;
            out = y;
            return y;
        }
    };

    // RBJ biquad (shelves and peaks for tone stages), transposed direct form II.
    struct Biquad
    {
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f, z1 = 0.0f, z2 = 0.0f;

        void reset() noexcept { z1 = z2 = 0.0f; }

        float process (float x) noexcept
        {
            const float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }

        enum Type { lowShelf, highShelf, peak, lowPass, highPass, bandPass };

        void set (Type type, float hz, float q, float gainDb, float sampleRate) noexcept
        {
            const float w = twoPi * std::clamp (hz, 10.0f, sampleRate * 0.45f) / sampleRate;
            const float cw = std::cos (w), sw = std::sin (w);
            const float alpha = sw / (2.0f * std::max (0.05f, q));
            const float A = std::pow (10.0f, gainDb / 40.0f);
            float B0 = 1, B1 = 0, B2 = 0, A0 = 1, A1 = 0, A2 = 0;
            switch (type)
            {
                case lowShelf:
                {
                    const float sq = 2.0f * std::sqrt (A) * alpha;
                    B0 = A * ((A + 1) - (A - 1) * cw + sq);
                    B1 = 2 * A * ((A - 1) - (A + 1) * cw);
                    B2 = A * ((A + 1) - (A - 1) * cw - sq);
                    A0 = (A + 1) + (A - 1) * cw + sq;
                    A1 = -2 * ((A - 1) + (A + 1) * cw);
                    A2 = (A + 1) + (A - 1) * cw - sq;
                    break;
                }
                case highShelf:
                {
                    const float sq = 2.0f * std::sqrt (A) * alpha;
                    B0 = A * ((A + 1) + (A - 1) * cw + sq);
                    B1 = -2 * A * ((A - 1) + (A + 1) * cw);
                    B2 = A * ((A + 1) + (A - 1) * cw - sq);
                    A0 = (A + 1) - (A - 1) * cw + sq;
                    A1 = 2 * ((A - 1) - (A + 1) * cw);
                    A2 = (A + 1) - (A - 1) * cw - sq;
                    break;
                }
                case peak:
                    B0 = 1 + alpha * A; B1 = -2 * cw; B2 = 1 - alpha * A;
                    A0 = 1 + alpha / A; A1 = -2 * cw; A2 = 1 - alpha / A;
                    break;
                case lowPass:
                    B0 = (1 - cw) * 0.5f; B1 = 1 - cw; B2 = (1 - cw) * 0.5f;
                    A0 = 1 + alpha; A1 = -2 * cw; A2 = 1 - alpha;
                    break;
                case highPass:
                    B0 = (1 + cw) * 0.5f; B1 = -(1 + cw); B2 = (1 + cw) * 0.5f;
                    A0 = 1 + alpha; A1 = -2 * cw; A2 = 1 - alpha;
                    break;
                case bandPass:
                    B0 = alpha; B1 = 0; B2 = -alpha;
                    A0 = 1 + alpha; A1 = -2 * cw; A2 = 1 - alpha;
                    break;
            }
            b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0; a1 = A1 / A0; a2 = A2 / A0;
        }
    };

    // Tilt EQ around a pivot (LAB X-10 EMITTER tone): tone -1 dark .. +1 bright, one pole.
    struct Tilt
    {
        float lp = 0.0f, coef = 0.1f;
        void prepare (float pivotHz, float sampleRate) noexcept { coef = onePoleCoefHz (pivotHz, sampleRate); }
        void reset() noexcept { lp = 0.0f; }
        float process (float x, float tone) noexcept
        {
            lp += (x - lp) * coef;
            return lp * (1.0f - tone) + (x - lp) * (1.0f + tone);
        }
    };
}
