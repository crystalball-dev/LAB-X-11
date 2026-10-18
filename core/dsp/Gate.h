#pragma once

#include <algorithm>
#include <cmath>
#include <vector>
#include "Align.h"
#include "Common.h"
#include "Filters.h"
#include "Follower.h"
#include "SelfTest.h"

namespace lab::dsp
{
    // VocalGate: the LAB X-11 noise gate. It decides on the undelayed input (the key) and writes one gain per sample;
    // the chain applies that gain to the audio TransientAlign has delayed by the lookahead, so the gate is open before
    // a consonant arrives and the opening ramp lands on room tone, not on the word (docs/SPEC_LOOKAHEAD_GATE_AND_ALIGNMENT.md).
    //
    // Tuned for the reference chains (handoff 5.1): DD Gate at its lowest threshold (about -40 dBFS), short hold,
    // release near minimum; breaths and word tails pass, room tone between phrases goes.
    //
    // Detector: a low cut (SVF high-pass, 12 dB/oct) on each channel, then the louder channel's peak (instant attack,
    // DET RELEASE one-pole fall) or the RMS of both channels over 2.5 ms. Both detectors always run, so switching
    // DETECTOR never resets anything. Comparisons are in linear level; no log per sample.
    //
    // State machine with hysteresis (open above THRESHOLD, close below THRESHOLD - HYSTERESIS):
    //   Closed -> Attack (key > open) -> Open (gain reached 1) -> Hold (key < close) -> Release (hold expired)
    //   -> Closed (gain reached the floor). Attack falls to Release when the key drops below close before the gain
    //   reaches 1; Hold returns to Open and Release to Attack when the key rises above open again.
    //
    // Ramp: a constant slew in dB, one multiply and a clamp per sample. ATTACK is the time for the whole climb from the
    // floor (-RANGE) to 0 dB and RELEASE the time for the whole fall, exactly, at every sample rate.
    class VocalGate
    {
    public:
        enum class State { closed = 0, attack, open, hold, release };
        enum class Mode { peak = 0, rms };

        struct Settings
        {
            float thresholdDb = -40.0f, hysteresisDb = 4.0f;
            float attackMs = 1.0f, holdMs = 40.0f, releaseMs = 120.0f, rangeDb = 24.0f;
            float lowCutHz = 80.0f, detReleaseMs = 20.0f;
            Mode mode = Mode::peak;
        };

        static constexpr double rmsWindowSeconds = 0.0025;   // half the lookahead, so RMS mode still opens early (O9)
        static constexpr float lowCutOffHz = 20.5f;           // LOW CUT at its 20 Hz minimum: the filter is out

        static int lookaheadSamples (double sampleRate) noexcept { return TransientAlign::latency (sampleRate); }

        // Sizes the RMS ring and applies the default settings, so reset() and computeGain() are defined before the
        // first set() (a fault reset on the very first block reads nothing uninitialised).
        void prepare (double sampleRate)
        {
            sr = (float) sampleRate;
            window = std::max (1, (int) (sampleRate * rmsWindowSeconds + 0.5));
            squares.assign ((size_t) window, 0.0f);
            invWindow = 1.0 / (double) window;
            lastLowCut = -1.0f;
            set (Settings {});
            reset();
        }

        // State to zero, settings kept.
        void reset() noexcept
        {
            lowCutL.reset();
            lowCutR.reset();
            peak.reset();
            std::fill (squares.begin(), squares.end(), 0.0f);
            squareSum = 0.0;
            squareWrite = 0;
            state = State::closed;
            floorGain = gain = rangeGain;
            floorRemaining = 0;
            holdCounter = 0;
            keyLevel = 0.0f;
        }

        // Once per control block (the base's 16 samples) with smoothed values.
        void set (const Settings& s) noexcept
        {
            mode = s.mode;
            openLevel = dbToGain (s.thresholdDb);
            closeLevel = dbToGain (s.thresholdDb - std::max (0.0f, s.hysteresisDb));
            const float range = std::max (0.0f, s.rangeDb);
            rangeGain = dbToGain (-range);
            if (std::abs (rangeGain - floorGain) > 0.0f)
            {
                floorRemaining = floorGlide;
                floorStep = (rangeGain - floorGain) / (float) floorGlide;
            }
            attackSamples = std::max (1, (int) std::lround (s.attackMs * 0.001f * sr));
            releaseSamples = std::max (1, (int) std::lround (s.releaseMs * 0.001f * sr));
            holdSamples = std::max (0, (int) std::lround (s.holdMs * 0.001f * sr));
            upStep = dbToGain (range / (float) attackSamples);
            downStep = dbToGain (-range / (float) releaseSamples);
            peak.attackCoef = 1.0f;   // instant: the lookahead, not the detector, provides the early opening
            peak.releaseCoef = onePoleCoef (std::max (0.001f, s.detReleaseMs) * 0.001f, sr);
            lowCutOn = s.lowCutHz > lowCutOffHz;
            if (lowCutOn && std::abs (s.lowCutHz - lastLowCut) > 1.0e-3f)
            {
                lowCutL.setHz (s.lowCutHz, 0.7071f, sr);
                lowCutR.setHz (s.lowCutHz, 0.7071f, sr);
                lastLowCut = s.lowCutHz;
            }
        }

        // n gains (linear, rangeGain .. 1) from n samples of the undelayed key; voice (optional) receives 1 while the
        // gate is Open or in Hold, 0 otherwise. The audio is not touched.
        void computeGain (const float* keyL, const float* keyR, float* gainOut, float* voice, int n) noexcept
        {
            for (int i = 0; i < n; ++i)
            {
                float l = keyL[i], r = keyR[i];
                if (lowCutOn)
                {
                    l = lowCutL.highpass (l);
                    r = lowCutR.highpass (r);
                }

                // Both detectors run every sample; DETECTOR only picks one.
                const float peakLevel = peak.process (std::max (std::abs (l), std::abs (r)));
                const float sq = 0.5f * (l * l + r * r);
                squareSum += (double) sq - (double) squares[(size_t) squareWrite];
                squares[(size_t) squareWrite] = sq;
                if (++squareWrite >= window)
                    squareWrite = 0;
                const float rmsLevel = (float) std::sqrt (std::max (squareSum, 0.0) * invWindow);
                keyLevel = mode == Mode::peak ? peakLevel : rmsLevel;

                // The floor glides to a new RANGE over one control block (the base has already smoothed RANGE).
                if (floorRemaining > 0)
                {
                    floorGain = --floorRemaining == 0 ? rangeGain : floorGain + floorStep;
                }

                advance (keyLevel);

                // One rule for every state: slew up (Attack, Open, Hold) or down (Release, Closed), then clamp between
                // the floor and unity. The clamp is what keeps the gate from stalling: a floor that rises (RANGE turned
                // down) pushes the gain up with it, and RANGE 0 pins the gain at 1 whatever the steps are.
                gain = std::clamp (gain * (rising() ? upStep : downStep), floorGain, 1.0f);

                if (state == State::attack && gain >= 1.0f)
                    state = State::open;
                else if (state == State::release && gain <= floorGain)
                    state = State::closed;

                gainOut[i] = gain;
                if (voice != nullptr)
                    voice[i] = (state == State::open || state == State::hold) ? 1.0f : 0.0f;
            }
        }

        State getState() const noexcept { return state; }
        float getGain() const noexcept { return gain; }
        float gainReductionDb() const noexcept { return gainToDb (gain); }
        float keyDb() const noexcept { return gainToDb (std::max (keyLevel, 1.0e-6f)); }
        bool isOpen() const noexcept { return state == State::open || state == State::hold; }
        int getAttackSamples() const noexcept { return attackSamples; }
        int getReleaseSamples() const noexcept { return releaseSamples; }
        int getHoldSamples() const noexcept { return holdSamples; }

        // The editor's lamp: 1 open or opening, 0.5 holding or closing, 0 closed (O10).
        float lampValue() const noexcept
        {
            switch (state)
            {
                case State::open: case State::attack: return 1.0f;
                case State::hold: case State::release: return 0.5f;
                case State::closed: break;
            }
            return 0.0f;
        }

        static SelfTestReport selfTest();

    private:
        bool rising() const noexcept { return state == State::attack || state == State::open || state == State::hold; }

        // Sample-exact transitions (spec 4.5). Hold: entered with holdCounter = holdSamples, decremented first and
        // released at <= 0, so Release starts exactly holdSamples after the key fell below close; a hold of 0 goes
        // straight from Open to Release. The counter can never run away.
        void advance (float key) noexcept
        {
            switch (state)
            {
                case State::closed:
                    if (key > openLevel)
                        state = State::attack;
                    break;
                case State::attack:
                    if (key < closeLevel)
                        state = State::release;   // a false start gets no hold
                    break;
                case State::open:
                    if (key < closeLevel)
                    {
                        if (holdSamples > 0)
                        {
                            state = State::hold;
                            holdCounter = holdSamples;
                        }
                        else
                            state = State::release;
                    }
                    break;
                case State::hold:
                    if (key > openLevel)
                        state = State::open;
                    else if (--holdCounter <= 0)
                        state = State::release;
                    break;
                case State::release:
                    if (key > openLevel)
                        state = State::attack;   // the rise continues from the current gain: no jump
                    break;
            }
        }

        float sr = 48000.0f;
        SVF lowCutL, lowCutR;
        Follower peak;
        std::vector<float> squares;
        double squareSum = 0.0, invWindow = 1.0;
        int window = 120, squareWrite = 0;

        Mode mode = Mode::peak;
        State state = State::closed;
        static constexpr int floorGlide = 16;   // one control block

        float openLevel = 0.01f, closeLevel = 0.0063f, rangeGain = 0.063f, floorGain = 0.063f, floorStep = 0.0f;
        float upStep = 1.0f, downStep = 1.0f, gain = 0.063f, keyLevel = 0.0f, lastLowCut = -1.0f;
        int attackSamples = 48, releaseSamples = 5760, holdSamples = 1920, holdCounter = 0, floorRemaining = 0;
        bool lowCutOn = true;
    };

    // ---------------------------------------------------------------------------------------------------------------
    // G1-G13 (spec section 8.1). Synthetic, seeded signals; every case is deterministic.
    inline SelfTestReport VocalGate::selfTest()
    {
        SelfTestReport report;
        constexpr double pi2 = 6.283185307179586;

        struct Run
        {
            std::vector<float> gain, voice;
            std::vector<int> state;
        };

        // Runs a gate over a mono signal (duplicated to both channels) in blocks, recording gain and state per sample.
        // `change` (optional) is called before each block with the block's start sample and may change the settings.
        auto run = [] (double sampleRate, const std::vector<float>& x, Settings s, int block, auto change)
        {
            VocalGate g;
            g.prepare (sampleRate);
            g.set (s);
            g.reset();
            Run out;
            const int total = (int) x.size();
            out.gain.resize ((size_t) total);
            out.voice.resize ((size_t) total);
            out.state.resize ((size_t) total);
            for (int pos = 0; pos < total; pos += block)
            {
                const int n = std::min (block, total - pos);
                change (g, s, pos);
                for (int i = 0; i < n; ++i)
                {
                    g.computeGain (x.data() + pos + i, x.data() + pos + i, out.gain.data() + pos + i, out.voice.data() + pos + i, 1);
                    out.state[(size_t) (pos + i)] = (int) g.getState();
                }
            }
            return out;
        };
        auto none = [] (VocalGate&, Settings&, int) {};

        auto sine = [&] (double sampleRate, double seconds, double hz, float levelDb, double start, double end)
        {
            std::vector<float> x ((size_t) (seconds * sampleRate), 0.0f);
            const float a = dbToGain (levelDb);
            for (size_t i = (size_t) (start * sampleRate); i < std::min (x.size(), (size_t) (end * sampleRate)); ++i)
                x[i] = a * (float) std::sin (pi2 * hz * (double) i / sampleRate);
            return x;
        };
        auto firstIndex = [] (const std::vector<int>& st, int from, auto pred)
        {
            for (int i = std::max (0, from); i < (int) st.size(); ++i)
                if (pred (st[(size_t) i]))
                    return i;
            return -1;
        };
        const int closedS = (int) State::closed, attackS = (int) State::attack, openS = (int) State::open,
                  holdS = (int) State::hold, releaseS = (int) State::release;

        // G1: -60 dBFS noise under a -40 dB threshold: closed throughout, gain exactly the floor.
        {
            std::vector<float> x (24000);
            Rng rng;
            rng.seed (0x61u);
            for (auto& v : x)
                v = dbToGain (-60.0f) * rng.bi();
            const auto r = run (48000.0, x, Settings {}, 16, none);
            float worst = 0.0f;
            bool closed = true;
            for (size_t i = 0; i < x.size(); ++i)
            {
                worst = std::max (worst, std::abs (r.gain[i] - dbToGain (-24.0f)));
                closed = closed && r.state[i] == closedS;
            }
            report.add ("g1_closed_floor", (double) worst, closed && worst <= 0.0f);
        }

        // G2: a -20 dBFS sine from silence: gain reaches exactly 1 attackSamples after the gate starts opening.
        {
            Settings s;
            const auto x = sine (48000.0, 0.3, 1000.0, -20.0f, 0.1, 0.3);
            const auto r = run (48000.0, x, s, 16, none);
            const int start = firstIndex (r.state, 0, [&] (int v) { return v == attackS; });
            int full = -1;
            for (int i = std::max (0, start); i < (int) r.gain.size(); ++i)
                if (r.gain[(size_t) i] >= 1.0f) { full = i; break; }
            const int expected = (int) std::lround (s.attackMs * 48.0f);
            const int diff = full - start + 1;   // the first rise happens on the Attack sample itself
            report.add ("g2_attack_samples", (double) diff, start >= 0 && std::abs (diff - expected) <= 1
                                                             && r.state[(size_t) full] == openS);
        }

        // G3: hold 40 ms and 1 ms: Release starts exactly holdSamples after the key fell below close.
        for (const float holdMs : { 40.0f, 1.0f })
        {
            Settings s;
            s.holdMs = holdMs;
            const auto x = sine (48000.0, 0.6, 1000.0, -20.0f, 0.05, 0.25);
            const auto r = run (48000.0, x, s, 16, none);
            const int holdAt = firstIndex (r.state, 0, [&] (int v) { return v == holdS; });
            const int releaseAt = firstIndex (r.state, holdAt, [&] (int v) { return v == releaseS; });
            const int expected = (int) std::lround (holdMs * 48.0f);
            report.add (holdMs > 2.0f ? "g3_hold_40ms" : "g3_hold_1ms", (double) (releaseAt - holdAt),
                        holdAt >= 0 && releaseAt - holdAt == expected);
        }

        // G3b: hold 0: Open goes straight to Release, and the gate reaches Closed (never sticks).
        {
            Settings s;
            s.holdMs = 0.0f;
            const auto x = sine (48000.0, 0.8, 1000.0, -20.0f, 0.05, 0.25);
            const auto r = run (48000.0, x, s, 16, none);
            const int holdAt = firstIndex (r.state, 0, [&] (int v) { return v == holdS; });
            const int releaseAt = firstIndex (r.state, 0, [&] (int v) { return v == releaseS; });
            report.add ("g3b_hold_zero_closes", (double) r.state.back(), holdAt < 0 && releaseAt > 0 && r.state.back() == closedS);
        }

        // G4: release: the gain reaches the floor exactly releaseSamples after Release starts, then Closed.
        {
            Settings s;
            const auto x = sine (48000.0, 0.8, 1000.0, -20.0f, 0.05, 0.25);
            const auto r = run (48000.0, x, s, 16, none);
            const int releaseAt = firstIndex (r.state, 0, [&] (int v) { return v == releaseS; });
            int floorAt = -1;
            for (int i = std::max (0, releaseAt); i < (int) r.gain.size(); ++i)
                if (r.gain[(size_t) i] <= dbToGain (-24.0f)) { floorAt = i; break; }
            const int expected = (int) std::lround (s.releaseMs * 48.0f);
            const int diff = floorAt - releaseAt + 1;   // the first fall happens on the Release sample itself
            report.add ("g4_release_samples", (double) diff, releaseAt > 0 && std::abs (diff - expected) <= 1
                                                             && r.state[(size_t) floorAt] == closedS);
        }

        // G5: hysteresis: after opening, a sine at THRESHOLD - HYSTERESIS / 2 keeps the gate open.
        {
            Settings s;
            auto x = sine (48000.0, 1.0, 1000.0, -20.0f, 0.05, 0.2);
            const auto low = sine (48000.0, 1.0, 1000.0, s.thresholdDb - 0.5f * s.hysteresisDb, 0.2, 1.0);
            for (size_t i = 0; i < x.size(); ++i)
                x[i] += low[i];
            const auto r = run (48000.0, x, s, 16, none);
            int notOpen = 0;
            for (size_t i = (size_t) (0.1 * 48000); i < x.size(); ++i)
                notOpen += r.state[i] != openS ? 1 : 0;
            report.add ("g5_hysteresis_holds_open", (double) notOpen, notOpen == 0);
        }

        // G6: a sine whose level swings +-1 dB around THRESHOLD at 200 Hz: at most 2 state changes per 100 ms.
        {
            Settings s;
            std::vector<float> x (48000);
            for (size_t i = 0; i < x.size(); ++i)
            {
                const double t = (double) i / 48000.0;
                const float db = s.thresholdDb + (float) std::sin (pi2 * 200.0 * t);
                x[i] = dbToGain (db) * (float) std::sin (pi2 * 1000.0 * t);
            }
            const auto r = run (48000.0, x, s, 16, none);
            int worst = 0;
            for (int w = 0; w + 4800 <= (int) x.size(); w += 4800)
            {
                int changes = 0;
                for (int i = w + 1; i < w + 4800; ++i)
                    changes += r.state[(size_t) i] != r.state[(size_t) i - 1] ? 1 : 0;
                worst = std::max (worst, changes);
            }
            report.add ("g6_chatter_changes_per_100ms", (double) worst, worst <= 2);
        }

        // G7 / G7b: the lookahead, composed here from the gate, TransientAlign and the multiply (the chain itself lives
        // in plugins/x11/LabX11.cpp). A sine onset at t comes out at t + LOOKAHEAD with the gain already at 1.
        for (const Mode m : { Mode::peak, Mode::rms })
        {
            Settings s;
            s.mode = m;
            const double rate = 48000.0;
            auto x = sine (rate, 0.3, 1000.0, -20.0f, 0.1, 0.3);
            const int onset = (int) (0.1 * rate);
            VocalGate g;
            g.prepare (rate);
            g.set (s);
            g.reset();
            TransientAlign a;
            a.prepare (rate);
            std::vector<float> l = x, r = x, gains (x.size());
            for (int pos = 0; pos < (int) x.size(); pos += 16)
            {
                const int n = std::min (16, (int) x.size() - pos);
                g.computeGain (l.data() + pos, r.data() + pos, gains.data() + pos, nullptr, n);
                a.process (l.data() + pos, r.data() + pos, n);
            }
            const int out = onset + a.samples();
            // Every output sample from the onset's arrival on carries gain 1 (the onset itself is exactly 0 for a sine
            // starting at phase 0, so the gain is checked rather than the sample).
            float worst = 1.0f;
            for (int i = out; i < out + 480; ++i)
                worst = std::min (worst, gains[(size_t) i]);
            report.add (m == Mode::peak ? "g7_preopen_peak" : "g7b_preopen_rms", (double) worst, worst >= 1.0f);
        }

        // G8: RANGE 0: the gate is transparent.
        {
            Settings s;
            s.rangeDb = 0.0f;
            const auto x = sine (48000.0, 0.5, 1000.0, -20.0f, 0.1, 0.3);
            const auto r = run (48000.0, x, s, 16, none);
            float worst = 0.0f;
            for (const float v : r.gain)
                worst = std::max (worst, std::abs (v - 1.0f));
            report.add ("g8_range_zero_transparent", (double) worst, worst <= 0.0f);
        }

        // G8b: RANGE 24 -> 48 dB while closed: the gain falls to the new floor at the release rate (24 dB of the 48 dB
        // span takes half of RELEASE) and the state stays Closed.
        {
            Settings s;
            const std::vector<float> x (48000, 0.0f);
            const int at = 4800;
            auto change = [&] (VocalGate& g, Settings& st, int pos)
            {
                if (pos == at)
                {
                    st.rangeDb = 48.0f;
                    g.set (st);
                }
            };
            const auto r = run (48000.0, x, s, 16, change);
            int reached = -1;
            for (int i = at; i < (int) x.size(); ++i)
                if (r.gain[(size_t) i] <= dbToGain (-48.0f)) { reached = i; break; }
            const int expected = (int) std::lround (s.releaseMs * 48.0f) / 2;
            bool closed = true;
            for (const int v : r.state)
                closed = closed && v == closedS;
            report.add ("g8b_range_change_closed", (double) (reached - at + 1), reached > 0 && std::abs (reached - at + 1 - expected) <= 2 && closed);
        }

        // G8c: RMS mode, 10 s of a loud signal then silence: the key stays finite and non-negative, the gate closes.
        {
            Settings s;
            s.mode = Mode::rms;
            auto x = sine (48000.0, 11.0, 220.0, -6.0f, 0.0, 10.0);
            VocalGate g;
            g.prepare (48000.0);
            g.set (s);
            g.reset();
            std::vector<float> gains (x.size());
            bool finite = true;
            for (int pos = 0; pos < (int) x.size(); pos += 16)
            {
                const int n = std::min (16, (int) x.size() - pos);
                g.computeGain (x.data() + pos, x.data() + pos, gains.data() + pos, nullptr, n);
                finite = finite && std::isfinite (g.keyDb()) && std::isfinite (g.getGain());
            }
            report.add ("g8c_rms_silence_finite", (double) g.keyDb(), finite && g.getState() == State::closed);
        }

        // G9: block equivalence: blocks of 1, 7 and 16 give identical gains.
        {
            Settings s;
            auto x = sine (48000.0, 2.0, 330.0, -18.0f, 0.2, 0.9);
            const auto b = sine (48000.0, 2.0, 1200.0, -45.0f, 0.9, 2.0);
            for (size_t i = 0; i < x.size(); ++i)
                x[i] += b[i];
            auto runBlocks = [&] (int block)
            {
                VocalGate g;
                g.prepare (48000.0);
                g.set (s);
                g.reset();
                std::vector<float> gains (x.size());
                for (int pos = 0; pos < (int) x.size(); pos += block)
                    g.computeGain (x.data() + pos, x.data() + pos, gains.data() + pos, nullptr, std::min (block, (int) x.size() - pos));
                return gains;
            };
            const auto a = runBlocks (16);
            float worst = 0.0f;
            for (const int block : { 1, 7 })
            {
                const auto c = runBlocks (block);
                for (size_t i = 0; i < a.size(); ++i)
                    worst = std::max (worst, std::abs (a[i] - c[i]));
            }
            report.add ("g9_block_equivalence", (double) worst, worst <= 0.0f);
        }

        // G10: rate equivalence: the hold time in ms is the same at 44.1, 48 and 96 kHz (within 0.05 ms).
        {
            Settings s;
            double lo = 1.0e9, hi = -1.0e9;
            for (const double rate : { 44100.0, 48000.0, 96000.0 })
            {
                const auto x = sine (rate, 0.6, 1000.0, -20.0f, 0.05, 0.25);
                const auto r = run (rate, x, s, 16, none);
                const int holdAt = firstIndex (r.state, 0, [&] (int v) { return v == holdS; });
                const int releaseAt = firstIndex (r.state, holdAt, [&] (int v) { return v == releaseS; });
                const double ms = (double) (releaseAt - holdAt) * 1000.0 / rate;
                lo = std::min (lo, ms);
                hi = std::max (hi, ms);
            }
            report.add ("g10_rate_equivalence_ms", hi - lo, hi - lo <= 0.05);
        }

        // G11: DETECTOR Peak -> RMS mid-signal: no jump in the gain, nothing non-finite.
        {
            Settings s;
            const auto x = sine (48000.0, 1.0, 440.0, -20.0f, 0.05, 1.0);
            auto change = [&] (VocalGate& g, Settings& st, int pos)
            {
                if (pos == 24000)
                {
                    st.mode = Mode::rms;
                    g.set (st);
                }
            };
            const auto r = run (48000.0, x, s, 16, change);
            float jump = 0.0f;
            bool finite = true;
            for (size_t i = 24000; i < r.gain.size(); ++i)
            {
                jump = std::max (jump, std::abs (r.gain[i] - r.gain[i - 1]));
                finite = finite && std::isfinite (r.gain[i]);
            }
            report.add ("g11_mode_switch_jump", (double) jump, finite && jump <= 0.1f);
        }

        // G12: denormal-sized input for 10 s: finite, closed, the floor exactly.
        {
            const std::vector<float> x (480000, 1.0e-30f);
            const auto r = run (48000.0, x, Settings {}, 16, none);
            bool ok = true;
            for (size_t i = 0; i < x.size(); ++i)
                ok = ok && std::isfinite (r.gain[i]) && r.state[i] == closedS;
            report.add ("g12_denormal_input", (double) r.gain.back(), ok);
        }

        // G13: determinism: the same input twice after reset gives identical gains.
        {
            std::vector<float> x (96000);
            Rng rng;
            rng.seed (0x13u);
            for (size_t i = 0; i < x.size(); ++i)
                x[i] = ((i / 9000) % 2 == 0 ? 0.2f : 0.001f) * rng.bi();
            VocalGate g;
            g.prepare (48000.0);
            g.set (Settings {});
            std::vector<float> a (x.size()), b (x.size());
            g.reset();
            g.computeGain (x.data(), x.data(), a.data(), nullptr, (int) x.size());
            g.reset();
            g.computeGain (x.data(), x.data(), b.data(), nullptr, (int) x.size());
            float worst = 0.0f;
            for (size_t i = 0; i < a.size(); ++i)
                worst = std::max (worst, std::abs (a[i] - b[i]));
            report.add ("g13_determinism", (double) worst, worst <= 0.0f);
        }

        return report;
    }
}
