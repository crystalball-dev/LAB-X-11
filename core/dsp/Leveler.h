#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>
#include "Common.h"
#include "Filters.h"
#include "SelfTest.h"

namespace lab::dsp
{
    // LevelMatcher: LEVEL // УРОВЕНЬ of LAB X-11, the "quantizer-style" level matching of the plan (handoff 0, D7, D8):
    // a voice-gated rider that holds the vocal's phrase level near a per-song TARGET, the fader ride the reference
    // projects do by hand with automation clips. The full design and its reasoning: docs/SPEC_LEVEL_MATCHER.md.
    //
    //   measure  the BS.1770 K-weighted loudness of the delayed, gated vocal, 5 ms hops, over the last 400 ms of
    //            ADMITTED voice. A hop is admitted only when the gate is Open or Hold, the audio is voiced (500 Hz band
    //            3.5 dB over the 6 kHz band across 20 ms and 3 dB in the hop itself), it is above -70 LUFS, and it is not
    //            more than 20 dB under the window (before a level exists: under TARGET - MAX BOOST - 20 dB).
    //   decide   goal = clamp (TARGET - level, -MAX CUT, +MAX BOOST).
    //   ride     a dB one-pole toward the goal (time RIDE TIME, slewing at 6 / RIDE TIME dB/s above 6 dB of error),
    //            moving only on admitted voiced samples: gaps, breaths, consonant bursts and room tone hold the gain
    //            bit-exactly. Under LAW Step the ride is quantised to 1.5 dB and placed at phrase onsets inside the
    //            gate's 5 ms pre-open window with a raised-cosine pre-roll.
    //   apply    AMOUNT x ride, in dB, slew-limited to 12 dB/s through AMOUNT. 0 dB is exactly 1.0f.
    //
    // No peak path, no lookahead of its own, no latency: fast peak control stays with the compressors after it.
    // Fixed std::array state only; nothing allocates after construction.
    class LevelMatcher
    {
    public:
        enum Param : int { amount = 0, target, time, boost, cut, law, paramCount };   // offsets from the first level_* row
        enum Law : int { ride = 0, step = 1 };
        enum GateState : uint8_t { closed = 0, attack = 1, open = 2, hold = 3, release = 4 };   // == VocalGate::State

        static constexpr double hopSeconds = 0.005, absFloorMs = 1.17247e-7, relGate = 0.01, guardRatio = 0.01;
        // Voicing (L.4): the 500 Hz band over the 6 kHz band by +3.5 dB across 20 ms and +3.01 dB in the hop alone. The
        // spec's +6.02 dB over 20 ms admitted only 43 % of the hops of breathy voice (HNR 6 dB, L32); +3.5 dB admits 66 %.
        // White and vocal-tract noise are never admitted; pink noise on about 0.02 % of hops (16 seeds; 0.1-0.25 % at
        // +3.01 dB), never enough for a level. The test reads spectral tilt only: noise falling 6 dB per octave or more
        // (brown room rumble, HVAC) reads as voiced, so with the gate open on it only the level guard keeps it out.
        static constexpr double burstCeiling = 10.0, voiceRatio = 2.2387211385683, hopVoiceRatio = 2.0;
        static constexpr double preFloorDb = 20.0;                                            // before a level: TARGET - MAX BOOST - 20 dB
        static constexpr int windowHops = 80, minFillHops = 20, voiceHops = 4;
        static constexpr float lowBandHz = 500.0f, highBandHz = 6000.0f, bandQ = 0.5f;
        static constexpr double slewKneeDb = 6.0, snapDb = 1.0e-6, amountCapDbPerSecond = 12.0, minTime = 0.5;
        static constexpr double gridDb = 1.5, commitDb = 1.0, lateSeconds = 4.0;
        static constexpr double log2PerDb = 0.16609640474436813;   // log2 (10) / 20
        static constexpr float noLevel = -100.0f;
        // The parameter defaults: must equal the level_* rows of EffectInfo::params (checked by the harness, L31). TARGET
        // -30 is the median voiced loudness of the reference lead takes (handoff 5.3); the self-tests pin -18 explicitly.
        static constexpr float defaults[paramCount] = { 0.75f, -30.0f, 1.0f, 6.0f, 9.0f, 0.0f };

        static int lookahead (double sr) noexcept { return std::max (1, (int) (sr * hopSeconds + 0.5)); }   // == TransientAlign::latency

        // Coefficients, hop length, the default settings, then reset(). Allocates nothing.
        void prepare (double sr) noexcept
        {
            sampleRate = sr > 0.0 ? sr : 48000.0;
            hopLen = la = lookahead (sampleRate);
            lateLen = (int64_t) (lateSeconds * sampleRate + 0.5);
            capPerSample = amountCapDbPerSecond / sampleRate;
            setKWeighting (sampleRate);
            for (int c = 0; c < 2; ++c)
            {
                lowBand[(size_t) c].setHz (lowBandHz, bandQ, (float) sampleRate);
                highBand[(size_t) c].setHz (highBandHz, bandQ, (float) sampleRate);
            }
            control (defaults);
            reset();
        }

        // Fault or prepareToPlay: forget everything; the gain is exactly 0 dB until 100 ms of voice is admitted again.
        void reset() noexcept
        {
            resetFilters();
            ring.fill (0.0);
            loRing.fill (0.0);
            hiRing.fill (0.0);
            write = filled = vWrite = hopCount = 0;
            kSum = loSum = hiSum = 0.0;
            gatedMean = 0.0;
            levelLufs = (double) noLevel;
            valid = admitting = wasPhrase = snapPending = false;
            preRollLeft = preRollK = 0;
            lateCount = 0;
            goalDb = rideDb = stepDb = outDb = appliedDb = lastApplied = 0.0;
            gLin = 1.0f;
            lamp = 0.0f;
            amountNow = amountTarget;
            hopsAdmitted = hopsRejected = 0;
        }

        // A preset change: keep the measurement (window, level, ride, filters); the next control() snaps the ride into
        // the new limits and the amount to its new value, on the first sample of the base's fade-in.
        void flush() noexcept { snapPending = true; }

        // Once per control block (<= 16 samples) with the base's smoothed values; v points at level_amount.
        void control (const float* v) noexcept
        {
            amountTarget = v[amount] < 1.0e-4f ? 0.0 : std::min ((double) v[amount], 1.0);
            targetDb = (double) v[target];
            boostDb = std::max (0.0, (double) v[boost]);
            cutDb = std::max (0.0, (double) v[cut]);
            preFloorMs = std::pow (10.0, (targetDb - boostDb - preFloorDb + 0.691) / 10.0);
            const double tau = std::max (minTime, (double) v[time]);
            coef = 1.0 - std::exp (-1.0 / (tau * sampleRate));
            slew = slewKneeDb * coef;
            const int newLaw = v[law] > 0.5f ? step : ride;
            if (newLaw != lawNow)
            {
                lawNow = newLaw;
                preRollLeft = 0;
                lateCount = 0;
                if (lawNow == step)
                    stepDb = quantise (rideDb);
            }
            stepDb = std::clamp (stepDb, -cutDb, boostDb);
            if (valid)
                goalDb = std::clamp (targetDb - levelLufs, -cutDb, boostDb);
            if (snapPending)
            {
                snapPending = false;
                rideDb = std::clamp (rideDb, -cutDb, boostDb);
                if (lawNow == step)
                    stepDb = quantise (rideDb);
                outDb = lawNow == step ? stepDb : rideDb;
                preRollLeft = 0;
                amountNow = amountTarget;
                applyGain();
            }
        }

        // In place, on the delayed, gated audio. gateState[i] is the gate's state for output sample i (nullptr: Closed).
        void process (float* l, float* r, const uint8_t* gateState, int n) noexcept
        {
            for (int i = 0; i < n; ++i)
            {
                // The filters run on every sample, so their state is continuous through gaps.
                const float kl = highPass[0].process (shelf[0].process (l[i]));
                const float kr = highPass[1].process (shelf[1].process (r[i]));
                const float bl = lowBand[0].bandpass (l[i]), br = lowBand[1].bandpass (r[i]);
                const float hl = highBand[0].bandpass (l[i]), hr = highBand[1].bandpass (r[i]);
                const int s = gateState != nullptr ? (int) gateState[i] : (int) closed;
                const bool voiced = s == open || s == hold;
                const bool phrase = voiced || s == attack;

                if (voiced)
                {
                    kSum += (double) kl * (double) kl + (double) kr * (double) kr;
                    loSum += (double) bl * (double) bl + (double) br * (double) br;
                    hiSum += (double) hl * (double) hl + (double) hr * (double) hr;
                    if (++hopCount >= hopLen)
                        endHop();   // before RIDE: the sample that completes an admitted hop is the first it moves on
                }
                else
                    admitting = false;

                // Step: a phrase onset on the key timeline (from Closed or Release); its audio arrives la samples later.
                if (lawNow == step && phrase && ! wasPhrase)
                {
                    if (std::abs (rideDb - stepDb) >= commitDb)
                        stepDb = quantise (rideDb);
                    if (std::abs (outDb - stepDb) > 0.0)
                    {
                        preFrom = outDb;
                        preTo = stepDb;
                        preRollK = 0;
                        preRollLeft = la;
                    }
                    lateCount = 0;
                }
                wasPhrase = phrase;

                if (voiced && admitting)   // RIDE
                {
                    const double e = goalDb - rideDb;
                    rideDb = std::abs (e) < snapDb ? goalDb : rideDb + coef * std::clamp (e, -slewKneeDb, slewKneeDb);
                    if (lawNow == step && ++lateCount >= lateLen)   // 4 s of voice with no gate gap: commit in-voice
                    {
                        lateCount = 0;
                        if (std::abs (rideDb - stepDb) >= commitDb)
                            stepDb = quantise (rideDb);
                    }
                }

                if (preRollLeft > 0)   // Step pre-roll: a raised cosine over la samples, landing exactly
                {
                    ++preRollK;
                    --preRollLeft;
                    outDb = preRollLeft == 0 ? preTo
                                             : preFrom + (preTo - preFrom) * 0.5 * (1.0 - std::cos (3.141592653589793 * (double) preRollK / (double) la));
                }
                else if (voiced)   // the slew limiter onto the law's aim, landing exactly
                {
                    const double aim = lawNow == step ? stepDb : rideDb;
                    const double d = aim - outDb;
                    outDb = std::abs (d) <= slew ? aim : outDb + std::copysign (slew, d);
                }

                if (std::abs (amountNow - amountTarget) > 0.0)   // the applied gain moves <= 12 dB/s through AMOUNT
                {
                    const double stepA = std::abs (outDb) > 0.0 ? capPerSample / std::abs (outDb) : std::numeric_limits<double>::infinity();
                    const double da = amountTarget - amountNow;
                    amountNow = std::abs (da) <= stepA ? amountTarget : amountNow + std::copysign (stepA, da);
                }

                applyGain();
                l[i] *= gLin;
                r[i] *= gLin;
                lamp = voiced ? (admitting ? 1.0f : 0.5f) : 0.0f;
            }
        }

        float gainDb() const noexcept    { return (float) appliedDb; }                       // the level-match needle
        float levelDb() const noexcept   { return valid ? (float) levelLufs : noLevel; }    // LEVEL, LUFS (-100: nothing yet)
        float voiceLamp() const noexcept { return lamp; }                                   // 1 measuring, 0.5 rejected, 0 gate closed

        static SelfTestReport selfTest();

    private:
        friend struct LevelMatcherTest;   // core/dsp/LevelerSelfTest.h reads the internals the cases assert on

        void resetFilters() noexcept
        {
            for (size_t c = 0; c < 2; ++c)
            {
                shelf[c].reset();
                highPass[c].reset();
                lowBand[c].reset();
                highBand[c].reset();
            }
        }

        // ITU-R BS.1770 K-weighting from its rate-independent formulas (double math, stored as float).
        void setKWeighting (double sr) noexcept
        {
            constexpr double piD = 3.141592653589793;
            {
                const double f0 = 1681.974450955533, g = 3.999843853973347, q = 0.7071752369554196;
                const double k = std::tan (piD * f0 / sr), vh = std::pow (10.0, g / 20.0), vb = std::pow (vh, 0.4996667741545416);
                const double a0 = 1.0 + k / q + k * k;
                for (auto& f : shelf)
                {
                    f.b0 = (float) ((vh + vb * k / q + k * k) / a0);
                    f.b1 = (float) (2.0 * (k * k - vh) / a0);
                    f.b2 = (float) ((vh - vb * k / q + k * k) / a0);
                    f.a1 = (float) (2.0 * (k * k - 1.0) / a0);
                    f.a2 = (float) ((1.0 - k / q + k * k) / a0);
                }
            }
            {
                const double f0 = 38.13547087602444, q = 0.5003270373238773;
                const double k = std::tan (piD * f0 / sr);
                const double a0 = 1.0 + k / q + k * k;
                for (auto& f : highPass)
                {
                    f.b0 = 1.0f;
                    f.b1 = -2.0f;
                    f.b2 = 1.0f;
                    f.a1 = (float) (2.0 * (k * k - 1.0) / a0);
                    f.a2 = (float) ((1.0 - k / q + k * k) / a0);
                }
            }
        }

        void applyGain() noexcept
        {
            appliedDb = amountNow * outDb;
            if (std::abs (appliedDb - lastApplied) > 0.0)
            {
                lastApplied = appliedDb;
                gLin = std::abs (appliedDb) > 0.0 ? (float) std::exp2 (appliedDb * log2PerDb) : 1.0f;
            }
        }

        double quantise (double x) const noexcept { return std::clamp (gridDb * std::round (x / gridDb), -cutDb, boostDb); }

        void endHop() noexcept
        {
            double ms = kSum / (double) hopLen;
            const double lo = loSum, hi = hiSum;
            kSum = loSum = hiSum = 0.0;
            hopCount = 0;
            if (! std::isfinite (ms) || ! std::isfinite (lo) || ! std::isfinite (hi))
            {
                resetFilters();   // self-heal, checked once per hop
                loRing.fill (0.0);
                hiRing.fill (0.0);
                admitting = false;
                ++hopsRejected;
                return;
            }
            loRing[(size_t) vWrite] = lo;
            hiRing[(size_t) vWrite] = hi;
            vWrite = (vWrite + 1) % voiceHops;
            double lo20 = 0.0, hi20 = 0.0;
            for (size_t k = 0; k < (size_t) voiceHops; ++k)
            {
                lo20 += loRing[k];
                hi20 += hiRing[k];
            }
            admitting = ms >= absFloorMs
                        && lo20 >= voiceRatio * hi20                                 // voiced over 20 ms
                        && lo >= hopVoiceRatio * hi                                  // and in this hop alone
                        && (valid ? ms >= guardRatio * gatedMean : ms >= preFloorMs);
            if (! admitting)
            {
                ++hopsRejected;
                return;
            }
            ++hopsAdmitted;
            if (valid)
                ms = std::min (ms, burstLimit * gatedMean);   // +10 dB burst clamp
            ring[(size_t) write] = ms;
            write = (write + 1) % windowHops;
            filled = std::min (filled + 1, windowHops);
            if (filled < minFillHops)
                return;
            if (! valid)   // the first fill after reset(): cap a start-of-song plosive at 10 x the median
            {
                std::array<double, minFillHops> sorted {};
                std::copy (ring.begin(), ring.begin() + minFillHops, sorted.begin());
                std::nth_element (sorted.begin(), sorted.begin() + minFillHops / 2, sorted.end());
                const double cap = startLimit * sorted[(size_t) (minFillHops / 2)];
                for (size_t k = 0; k < (size_t) minFillHops; ++k)
                    ring[k] = std::min (ring[k], cap);
            }
            double m0 = 0.0;
            for (size_t k = 0; k < (size_t) filled; ++k)
                m0 += ring[k];
            m0 /= (double) filled;
            double sum = 0.0;
            int count = 0;
            for (size_t k = 0; k < (size_t) filled; ++k)
                if (ring[k] >= relGate * m0)
                {
                    sum += ring[k];
                    ++count;
                }
            gatedMean = sum / (double) std::max (1, count);
            levelLufs = -0.691 + 10.0 * std::log10 (gatedMean);
            valid = true;
            goalDb = std::clamp (targetDb - levelLufs, -cutDb, boostDb);
        }

        std::array<Biquad, 2> shelf {}, highPass {};   // K-weighting, [0] L, [1] R
        std::array<SVF, 2> lowBand {}, highBand {};    // voicing bands, [0] L, [1] R
        std::array<double, windowHops> ring {};
        std::array<double, voiceHops> loRing {}, hiRing {};
        int hopLen = 240, la = 240, hopCount = 0, write = 0, filled = 0, vWrite = 0, lawNow = ride;
        int preRollLeft = 0, preRollK = 0;
        int64_t lateLen = 192000, lateCount = 0, hopsAdmitted = 0, hopsRejected = 0;
        double kSum = 0.0, loSum = 0.0, hiSum = 0.0, gatedMean = 0.0, levelLufs = -100.0;
        bool valid = false, admitting = false, wasPhrase = false, snapPending = false;
        double sampleRate = 48000.0, targetDb = -30.0, boostDb = 6.0, cutDb = 9.0;
        double coef = 0.0, slew = 0.0, capPerSample = 0.00025, amountTarget = 0.75, amountNow = 0.75, preFloorMs = 0.0;
        double goalDb = 0.0, rideDb = 0.0, stepDb = 0.0, outDb = 0.0, appliedDb = 0.0, lastApplied = 0.0;
        double preFrom = 0.0, preTo = 0.0;
        double burstLimit = burstCeiling, startLimit = burstCeiling;   // test switches: selfTest sets +inf on references (L28, L29)
        float gLin = 1.0f, lamp = 0.0f;
    };
}

#include "LevelerSelfTest.h"
