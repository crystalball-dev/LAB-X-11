#pragma once

#include <algorithm>
#include <cmath>
#include <vector>
#include "Common.h"

namespace lab::dsp
{
    // One delay line for the whole suite (LAB X-19 DelayLine + LAB X-0 HistoryBuffer): mono or stereo, integer,
    // linear or Hermite reads. Delay 0 is the newest sample pushed.
    //
    // Two rules from the siblings' blowouts (X-3 NOOSPHERE, X-10 RESONATOR, X-19 ENSEMBLE): the whole and the
    // fractional part of a delay are split before any wrap-around, so every index is integer arithmetic; and one
    // slot past the end holds a sentinel that no correct read ever touches. If a read strays there, the self-test
    // sees a 1e6 error and the runaway guard a +120 dBFS spike.
    template <int Channels>
    class DelayLineN
    {
    public:
        static_assert (Channels == 1 || Channels == 2);
        static constexpr float sentinel = 1.0e6f;

        // Room for reads up to maxDelay samples back, plus the Hermite neighbours.
        void prepare (int maxDelay)
        {
            capacityValue = std::max (1, maxDelay);
            size = capacityValue + 4;
            for (auto& b : buffers)
            {
                b.assign ((size_t) size + 1, 0.0f);
                b[(size_t) size] = sentinel;
            }
            write = 0;
        }

        void reset() noexcept
        {
            for (auto& b : buffers)
                std::fill (b.begin(), b.begin() + size, 0.0f);
            write = 0;
        }

        int capacity() const noexcept { return capacityValue; }

        void push (float x) noexcept requires (Channels == 1)
        {
            buffers[0][(size_t) write] = x;
            advance();
        }

        void push (float l, float r) noexcept requires (Channels == 2)
        {
            buffers[0][(size_t) write] = l;
            buffers[1][(size_t) write] = r;
            advance();
        }

        // Exact integer delay, clamped to the capacity.
        float readInt (int channel, int delay) const noexcept
        {
            return buffers[(size_t) channel][(size_t) indexAt (std::clamp (delay, 0, capacityValue))];
        }

        // Linear between delays whole and whole + 1.
        float readLinear (int channel, float delay) const noexcept
        {
            delay = std::clamp (delay, 0.0f, (float) capacityValue);
            const int whole = std::min ((int) delay, capacityValue);
            const float f = delay - (float) whole;
            const auto& b = buffers[(size_t) channel];
            const float a = b[(size_t) indexAt (whole)];
            const float c = b[(size_t) indexAt (whole + 1)];
            return a + f * (c - a);
        }

        // 4-point Hermite between delays whole and whole + 1 (toward the past).
        float readHermite (int channel, double delay) const noexcept
        {
            delay = std::clamp (delay, 0.0, (double) capacityValue);
            const int whole = std::min ((int) delay, capacityValue);
            const float f = (float) (delay - (double) whole);
            const auto& b = buffers[(size_t) channel];
            const float x1 = b[(size_t) indexAt (whole)];
            const float x0 = whole >= 1 ? b[(size_t) indexAt (whole - 1)] : x1;
            const float x2 = b[(size_t) indexAt (whole + 1)];
            const float x3 = b[(size_t) indexAt (whole + 2)];
            return hermite4 (x0, x1, x2, x3, f);
        }

        // Pushes a ramp through a small line past several wraps and reads every integer and half-sample delay
        // with every interpolation. Returns the largest error (0 when the line is correct).
        static float selfTest() noexcept
        {
            DelayLineN line;
            line.prepare (37);
            float worst = 0.0f;
            for (int n = 0; n < 400; ++n)
            {
                if constexpr (Channels == 1)
                    line.push ((float) n);
                else
                    line.push ((float) n, (float) -n);

                const int available = std::min (n, line.capacity());
                for (int d = 0; d <= available; ++d)
                {
                    for (int ch = 0; ch < Channels; ++ch)
                    {
                        const float sign = ch == 0 ? 1.0f : -1.0f;
                        const float expected = sign * (float) (n - d);
                        worst = std::max (worst, std::abs (line.readInt (ch, d) - expected));
                        if (d < available)
                        {
                            const float half = sign * ((float) (n - d) - 0.5f);
                            worst = std::max (worst, std::abs (line.readLinear (ch, (float) d + 0.5f) - half));
                            if (d >= 1 && d + 2 <= available)
                                worst = std::max (worst, std::abs (line.readHermite (ch, (double) d + 0.5) - half));
                        }
                    }
                }
            }
            return worst;
        }

    private:
        void advance() noexcept
        {
            if (++write >= size)
                write = 0;
        }

        int indexAt (int delay) const noexcept   // delay 0 = the newest sample; pure integer arithmetic
        {
            int i = write - 1 - delay;
            while (i < 0)
                i += size;
            return i;
        }

        std::array<std::vector<float>, Channels> buffers;
        int capacityValue = 1, size = 5, write = 0;
    };

    using DelayLine = DelayLineN<1>;
    using StereoDelayLine = DelayLineN<2>;
}
