#pragma once

#include <algorithm>
#include <vector>
#include "DelayLine.h"
#include "SelfTest.h"

namespace lab::dsp
{
    // TransientAlign: the wet path's fixed micro-delay. The gate decides on the undelayed signal and its gain is
    // applied to this delayed copy, so the gate is already open when a consonant arrives (the lookahead). It is the
    // plugin's only latency: one integer per sample rate, the same function the chain reports to the host and the
    // base uses for the dry path, so MIX and BYPASS stay aligned to the sample (docs/SPEC_LOOKAHEAD_GATE_AND_ALIGNMENT.md
    // section 5). The ring is LAB X-16's StereoDelayLine (with its past-the-end sentinel), not a new one.
    struct TransientAlign
    {
        static constexpr double lookaheadSeconds = 0.005;   // 5 ms (decision D4 / O6)

        // floor (sr * 0.005 + 0.5), at least 1: 221 at 44.1 kHz (220.5 rounds up), 240 at 48 kHz, 480 at 96 kHz.
        // test/python/x11_tests.py uses the same rule, int (sr * 0.005 + 0.5), never Python's round() (half to even).
        static int latency (double sampleRate) noexcept
        {
            return std::max (1, (int) (sampleRate * lookaheadSeconds + 0.5));
        }

        void prepare (double sampleRate)
        {
            delay = latency (sampleRate);
            line.prepare (delay);
        }

        void reset() noexcept { line.reset(); }

        int samples() const noexcept { return delay; }

        // In place: l and r become the input of `samples()` samples ago. Any n; allocation-free.
        void process (float* l, float* r, int n) noexcept
        {
            for (int i = 0; i < n; ++i)
            {
                line.push (l[i], r[i]);
                l[i] = line.readInt (0, delay);
                r[i] = line.readInt (1, delay);
            }
        }

        // A1-A4 (spec section 8.1).
        static SelfTestReport selfTest()
        {
            SelfTestReport report;

            // A1: the latency rule at the three host rates.
            const bool a1 = latency (44100.0) == 221 && latency (48000.0) == 240 && latency (96000.0) == 480 && latency (1.0) == 1;
            report.add ("a1_latency_44k1", (double) latency (44100.0), a1);

            // A2: an impulse comes out exactly latency samples later and nowhere else (block sizes 1 and 7).
            for (const int block : { 1, 7 })
            {
                TransientAlign a;
                a.prepare (48000.0);
                const int total = 1000, at = 17;
                std::vector<float> l ((size_t) total, 0.0f), r ((size_t) total, 0.0f);
                l[(size_t) at] = 1.0f;
                r[(size_t) at] = -0.5f;
                for (int pos = 0; pos < total; pos += block)
                    a.process (l.data() + pos, r.data() + pos, std::min (block, total - pos));
                int found = -1, extra = 0;
                for (int i = 0; i < total; ++i)
                    if (std::abs (l[(size_t) i]) > 0.0f)
                    {
                        if (found < 0) found = i; else ++extra;
                    }
                const bool ok = found == at + 240 && extra == 0 && std::abs (r[(size_t) (at + 240)] + 0.5f) <= 0.0f;
                report.add (block == 1 ? "a2_impulse_offset_b1" : "a2_impulse_offset_b7", (double) (found - at), ok);
            }

            // A3: block independence. Seeded noise through blocks 1, 7, 64, 3000 and a varying size equals block 512.
            {
                const int total = 3 * 48000;
                std::vector<float> src ((size_t) total);
                Rng rng;
                rng.seed (0xA11Cu);
                for (auto& s : src)
                    s = rng.bi();
                auto run = [&] (int block, bool vary)
                {
                    TransientAlign a;
                    a.prepare (48000.0);
                    std::vector<float> l = src, r = src;
                    Rng sizes;
                    sizes.seed (0xB10Cu);
                    for (int pos = 0; pos < total;)
                    {
                        const int n = std::min (total - pos, vary ? 1 + (int) (sizes.next() % 3000u) : block);
                        a.process (l.data() + pos, r.data() + pos, n);
                        pos += n;
                    }
                    return l;
                };
                const auto reference = run (512, false);
                float worst = 0.0f;
                for (const int block : { 1, 7, 64, 3000 })
                {
                    const auto other = run (block, false);
                    for (size_t i = 0; i < other.size(); ++i)
                        worst = std::max (worst, std::abs (other[i] - reference[i]));
                }
                const auto varied = run (0, true);
                for (size_t i = 0; i < varied.size(); ++i)
                    worst = std::max (worst, std::abs (varied[i] - reference[i]));
                report.add ("a3_block_independence", (double) worst, worst <= 0.0f);
            }

            // A4: reset empties the ring.
            {
                TransientAlign a;
                a.prepare (48000.0);
                std::vector<float> l (300, 0.7f), r (300, 0.7f);
                a.process (l.data(), r.data(), 300);
                a.reset();
                std::fill (l.begin(), l.end(), 0.0f);
                std::fill (r.begin(), r.end(), 0.0f);
                a.process (l.data(), r.data(), 300);
                float worst = 0.0f;
                for (size_t i = 0; i < l.size(); ++i)
                    worst = std::max (worst, std::max (std::abs (l[i]), std::abs (r[i])));
                report.add ("a4_reset_silence", (double) worst, worst <= 0.0f);
            }
            return report;
        }

    private:
        StereoDelayLine line;
        int delay = 1;
    };
}
