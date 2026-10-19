#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
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
    //   -> Closed (gain reached the floor). Hold returns to Open (or Attack, if the gain is not yet up) and Release to
    //   Attack when the key rises above open again. A false start (the key falls below close during Attack) also goes
    //   through Hold, for the lookahead only, so the audio that triggered it still passes.
    //
    // Timing in output time. The key runs LOOKAHEAD samples ahead of the audio it governs, so every hold is lengthened by
    // the lookahead: Release starts HOLD after the audio itself fell below close, not HOLD - 5 ms after.
    //
    // Ramp: a constant slew in dB, kept in double and snapped when within half a step of its end, so ATTACK is exactly
    // the time for the whole climb from the floor (-RANGE) to 0 dB and RELEASE exactly the time for the whole fall.
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
        static constexpr int floorGlide = 16;                 // a RANGE change reaches the floor over one control block

        static int lookaheadSamples (double sampleRate) noexcept { return TransientAlign::latency (sampleRate); }

        // Sizes the RMS ring and applies the default settings, so reset() and computeGain() are defined before the
        // first set() (a fault reset on the very first block reads nothing uninitialised).
        void prepare (double sampleRate)
        {
            sr = (float) sampleRate;
            lookahead = lookaheadSamples (sampleRate);
            window = std::max (1, (int) (sampleRate * rmsWindowSeconds + 0.5));
            squares.assign ((size_t) window, 0.0f);
            invWindow = 1.0 / (double) window;
            lastLowCut = -1.0f;
            lowCutOn = false;
            rangeGain = -1.0;   // forces set() to take the floor
            set (Settings {});
            reset();
        }

        // State to zero, settings kept. The next set() places the floor directly (no glide from a stale RANGE).
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
            fresh = true;
        }

        // Once per control block (the base's 16 samples, fewer at the end of a host block) with smoothed values.
        void set (const Settings& s) noexcept
        {
            mode = s.mode;
            openLevel = dbToGain (s.thresholdDb);
            closeLevel = dbToGain (s.thresholdDb - std::max (0.0f, s.hysteresisDb));

            // RANGE: the floor glides to a new value over 16 samples; the glide restarts only when RANGE changes, so
            // it always lands exactly, whatever the size of the control blocks. After reset() it is placed directly.
            const double range = (double) std::max (0.0f, s.rangeDb);
            const double newRange = std::pow (10.0, -range / 20.0);
            if (fresh)
            {
                rangeGain = floorGain = gain = newRange;
                floorRemaining = 0;
            }
            else if (std::abs (newRange - rangeGain) > 0.0)
            {
                rangeGain = newRange;
                floorRemaining = floorGlide;
                floorStep = (rangeGain - floorGain) / (double) floorGlide;
            }

            attackSamples = std::max (1, (int) std::lround (s.attackMs * 0.001f * sr));
            releaseSamples = std::max (1, (int) std::lround (s.releaseMs * 0.001f * sr));
            holdSamples = std::max (0, (int) std::lround (s.holdMs * 0.001f * sr));
            if (state == State::hold)   // HOLD turned down during a hold takes effect now
                holdCounter = std::min (holdCounter, holdSamples + lookahead);
            upStep = std::pow (10.0, range / (double) attackSamples / 20.0);
            downStep = std::pow (10.0, -range / (double) releaseSamples / 20.0);
            upSnap = std::sqrt (upStep);
            downSnap = std::sqrt (downStep);

            peak.attackCoef = 1.0f;   // instant: the lookahead, not the detector, provides the early opening
            peak.releaseCoef = onePoleCoef (std::max (0.001f, s.detReleaseMs) * 0.001f, sr);

            const bool wasOn = lowCutOn;
            lowCutOn = s.lowCutHz > lowCutOffHz;
            if (lowCutOn && ! wasOn)
            {
                lowCutL.reset();   // no stale integrator state from the last time the filter ran
                lowCutR.reset();
            }
            if (lowCutOn && std::abs (s.lowCutHz - lastLowCut) > 1.0e-3f)
            {
                lowCutL.setHz (s.lowCutHz, 0.7071f, sr);
                lowCutR.setHz (s.lowCutHz, 0.7071f, sr);
                lastLowCut = s.lowCutHz;
            }
        }

        // n gains (linear, floor .. 1) from n samples of the undelayed key. stateOut (optional) receives the state after
        // each sample's update (State as uint8_t: 0 Closed, 1 Attack, 2 Open, 3 Hold, 4 Release), index-aligned with the
        // gain: the level matcher reads it as its voice flag (docs/SPEC_LEVEL_MATCHER.md L.4). The audio is not touched.
        void computeGain (const float* keyL, const float* keyR, float* gainOut, uint8_t* stateOut, int n) noexcept
        {
            fresh = false;
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

                if (floorRemaining > 0)
                    floorGain = --floorRemaining == 0 ? rangeGain : floorGain + floorStep;

                advance (keyLevel);

                // One rule for every state: slew up (Attack, Open, Hold) or down (Release, Closed), snap within half a
                // step of the end, then clamp between the floor and unity. The clamp keeps the gate from stalling: a
                // floor that rises (RANGE turned down) pushes the gain up with it; RANGE 0 pins the gain at 1.
                double next;
                if (rising())
                {
                    next = gain * upStep;
                    if (next * upSnap >= 1.0)
                        next = 1.0;
                }
                else
                {
                    next = gain * downStep;
                    if (next * downSnap <= floorGain)
                        next = floorGain;
                }
                gain = std::clamp (next, floorGain, 1.0);

                if (state == State::attack && gain >= 1.0)
                    state = State::open;
                else if (state == State::release && gain <= floorGain)
                    state = State::closed;

                gainOut[i] = (float) gain;
                if (stateOut != nullptr)
                    stateOut[i] = (uint8_t) state;
            }
        }

        State getState() const noexcept { return state; }
        float getGain() const noexcept { return (float) gain; }
        float gainReductionDb() const noexcept { return gainToDb ((float) gain); }
        float keyDb() const noexcept { return gainToDb (std::max (keyLevel, 1.0e-6f)); }
        bool isOpen() const noexcept { return state == State::open || state == State::hold; }
        int getAttackSamples() const noexcept { return attackSamples; }
        int getReleaseSamples() const noexcept { return releaseSamples; }
        int getHoldSamples() const noexcept { return holdSamples; }
        int getLookahead() const noexcept { return lookahead; }

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

        // Sample-exact transitions (spec 4.5). Hold is entered with a count, decremented first and released at <= 0,
        // so Release starts exactly that many samples later; the counter can never run away.
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
                    {
                        state = State::hold;          // a false start: no user hold, but the lookahead's worth, so the
                        holdCounter = lookahead;      // audio that opened the gate still reaches the output open
                    }
                    break;
                case State::open:
                    if (key < closeLevel)
                    {
                        state = State::hold;
                        holdCounter = holdSamples + lookahead;
                    }
                    break;
                case State::hold:
                    if (key > openLevel)
                        state = gain >= 1.0 ? State::open : State::attack;
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
        int lookahead = 240;
        SVF lowCutL, lowCutR;
        Follower peak;
        std::vector<float> squares;
        double squareSum = 0.0, invWindow = 1.0;
        int window = 120, squareWrite = 0;

        Mode mode = Mode::peak;
        State state = State::closed;
        float openLevel = 0.01f, closeLevel = 0.0063f, keyLevel = 0.0f, lastLowCut = -1.0f;
        double rangeGain = 0.063, floorGain = 0.063, floorStep = 0.0, gain = 0.063;
        double upStep = 1.0, downStep = 1.0, upSnap = 1.0, downSnap = 1.0;
        int attackSamples = 48, releaseSamples = 5760, holdSamples = 1920, holdCounter = 0, floorRemaining = 0;
        bool lowCutOn = false, fresh = true;
    };

    // ---------------------------------------------------------------------------------------------------------------
    // Self-tests (spec section 8.1, extended after review). Synthetic, seeded signals; every case is deterministic.
    // Each case is built so that a plausible mistake in the state machine, the detector or the ramp fails it.
    inline SelfTestReport VocalGate::selfTest()
    {
        SelfTestReport report;
        constexpr double pi2 = 6.283185307179586;
        using S = State;

        struct Run
        {
            std::vector<float> gain, key;
            std::vector<uint8_t> states;
            std::vector<int> state;
        };

        // Runs a gate over a mono signal (duplicated to both channels), calling set() before every block as the base
        // calls control(), recording gain, the state output, key and state per sample. `change` may edit the settings per block.
        auto run = [] (double sampleRate, const std::vector<float>& x, Settings s, int block, auto change)
        {
            VocalGate g;
            g.prepare (sampleRate);
            g.reset();
            Run out;
            const int total = (int) x.size();
            out.gain.resize ((size_t) total);
            out.states.resize ((size_t) total);
            out.key.resize ((size_t) total);
            out.state.resize ((size_t) total);
            for (int pos = 0; pos < total; pos += block)
            {
                const int n = std::min (block, total - pos);
                change (s, pos);
                g.set (s);
                for (int i = 0; i < n; ++i)
                {
                    const int k = pos + i;
                    g.computeGain (x.data() + k, x.data() + k, out.gain.data() + k, out.states.data() + k, 1);
                    out.state[(size_t) k] = (int) g.getState();
                    out.key[(size_t) k] = g.keyDb();
                }
            }
            return out;
        };
        auto none = [] (Settings&, int) {};

        auto sine = [&] (double sampleRate, double seconds, double hz, float levelDb, double start, double end)
        {
            std::vector<float> x ((size_t) (seconds * sampleRate), 0.0f);
            const float a = dbToGain (levelDb);
            for (size_t i = (size_t) (start * sampleRate); i < std::min (x.size(), (size_t) (end * sampleRate)); ++i)
                x[i] = a * (float) std::sin (pi2 * hz * (double) i / sampleRate);
            return x;
        };
        auto add = [] (std::vector<float> a, const std::vector<float>& b)
        {
            for (size_t i = 0; i < a.size() && i < b.size(); ++i)
                a[i] += b[i];
            return a;
        };
        auto first = [] (const std::vector<int>& st, int from, int value)
        {
            for (int i = std::max (0, from); i < (int) st.size(); ++i)
                if (st[(size_t) i] == value)
                    return i;
            return -1;
        };
        auto ever = [] (const std::vector<int>& st, int from, int to, int value)
        {
            for (int i = std::max (0, from); i < std::min (to, (int) st.size()); ++i)
                if (st[(size_t) i] == value)
                    return true;
            return false;
        };
        auto samples = [] (float ms, double rate) { return (int) std::lround (ms * 0.001f * (float) rate); };
        const int closedS = (int) S::closed, attackS = (int) S::attack, openS = (int) S::open, holdS = (int) S::hold,
                  releaseS = (int) S::release;
        const float floor24 = (float) std::pow (10.0, -24.0 / 20.0);

        // G1: -60 dBFS noise under a -40 dB threshold: closed throughout, the gain exactly the floor from sample 0.
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
                worst = std::max (worst, std::abs (r.gain[i] - floor24));
                closed = closed && r.state[i] == closedS;
            }
            report.add ("g1_closed_floor", (double) worst, closed && worst <= 0.0f);
        }

        // G2: a -20 dBFS sine from silence: the gain reaches exactly 1 attackSamples after the gate starts opening
        // (the first rise is on the Attack sample itself), at 44.1, 48 and 96 kHz.
        {
            bool ok = true;
            int shown = 0;
            for (const double rate : { 44100.0, 48000.0, 96000.0 })
            {
                Settings s;
                const auto x = sine (rate, 0.3, 1000.0, -20.0f, 0.1, 0.3);
                const auto r = run (rate, x, s, 16, none);
                const int start = first (r.state, 0, attackS);
                const int full = first (r.state, start, openS);
                const int diff = full - start + 1;
                ok = ok && start > 0 && diff == samples (s.attackMs, rate) && r.gain[(size_t) full] >= 1.0f
                     && r.gain[(size_t) full - 1] < 1.0f;
                if (rate > 47999.0 && rate < 48001.0)
                    shown = diff;
            }
            report.add ("g2_attack_samples_exact", (double) shown, ok);
        }

        // G3: HOLD 40, 1 and 0 ms: Release starts exactly HOLD + LOOKAHEAD after the key fell below close (HOLD after
        // the audio did).
        for (const float holdMs : { 40.0f, 1.0f, 0.0f })
        {
            Settings s;
            s.holdMs = holdMs;
            const auto x = sine (48000.0, 0.8, 1000.0, -20.0f, 0.05, 0.25);
            const auto r = run (48000.0, x, s, 16, none);
            const int holdAt = first (r.state, 0, holdS);
            const int releaseAt = first (r.state, holdAt, releaseS);
            const int expected = samples (holdMs, 48000.0) + 240;
            const char* name = holdMs > 2.0f ? "g3_hold_40ms" : holdMs > 0.5f ? "g3_hold_1ms" : "g3_hold_0ms";
            report.add (name, (double) (releaseAt - holdAt), holdAt > 0 && releaseAt - holdAt == expected && r.state.back() == closedS);
        }

        // G4: RELEASE: the gain reaches the floor exactly releaseSamples after Release starts, then Closed; 44.1/48/96.
        {
            bool ok = true;
            int shown = 0;
            for (const double rate : { 44100.0, 48000.0, 96000.0 })
            {
                Settings s;
                const auto x = sine (rate, 0.8, 1000.0, -20.0f, 0.05, 0.25);
                const auto r = run (rate, x, s, 16, none);
                const int releaseAt = first (r.state, 0, releaseS);
                const int closedAt = first (r.state, releaseAt, closedS);
                const int diff = closedAt - releaseAt + 1;
                ok = ok && releaseAt > 0 && diff == samples (s.releaseMs, rate) && std::abs (r.gain[(size_t) closedAt] - floor24) <= 0.0f;
                if (rate > 47999.0 && rate < 48001.0)
                    shown = diff;
            }
            report.add ("g4_release_samples_exact", (double) shown, ok);
        }

        // G5: hysteresis: after opening, a sine at THRESHOLD - HYSTERESIS / 2 keeps the gate open.
        {
            Settings s;
            const auto x = add (sine (48000.0, 1.0, 1000.0, -20.0f, 0.05, 0.2),
                                sine (48000.0, 1.0, 1000.0, s.thresholdDb - 0.5f * s.hysteresisDb, 0.2, 1.0));
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
                x[i] = dbToGain (s.thresholdDb + (float) std::sin (pi2 * 200.0 * t)) * (float) std::sin (pi2 * 1000.0 * t);
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

        // G7 / G7b: the lookahead, composed from the gate, TransientAlign and the multiply (the chain itself lives in
        // plugins/x11/LabX11.cpp): from a closed gate, every output sample of a sine burst equals the input sample
        // LOOKAHEAD earlier, exactly, from the burst's first sample on (Peak and RMS detectors).
        for (const Mode m : { Mode::peak, Mode::rms })
        {
            Settings s;
            s.mode = m;
            const double rate = 48000.0;
            const auto x = sine (rate, 0.3, 1000.0, -20.0f, 0.1, 0.3);
            const int onset = (int) (0.1 * rate);
            VocalGate g;
            g.prepare (rate);
            g.reset();
            TransientAlign a;
            a.prepare (rate);
            std::vector<float> l = x, r = x, gains (x.size());
            for (int pos = 0; pos < (int) x.size(); pos += 16)
            {
                const int n = std::min (16, (int) x.size() - pos);
                g.set (s);
                g.computeGain (l.data() + pos, r.data() + pos, gains.data() + pos, nullptr, n);
                a.process (l.data() + pos, r.data() + pos, n);
                for (int i = pos; i < pos + n; ++i)
                {
                    l[(size_t) i] *= gains[(size_t) i];
                    r[(size_t) i] *= gains[(size_t) i];
                }
            }
            float worst = 0.0f;
            for (int k = 0; k < 4800; ++k)
                worst = std::max (worst, std::abs (l[(size_t) (onset + a.samples() + k)] - x[(size_t) (onset + k)]));
            report.add (m == Mode::peak ? "g7_preopen_peak" : "g7b_preopen_rms", (double) worst, worst <= 0.0f);
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
        // span is exactly half of RELEASE) and the state stays Closed.
        {
            Settings s;
            const std::vector<float> x (48000, 0.0f);
            const int at = 4800;
            const auto r = run (48000.0, x, s, 16, [&] (Settings& st, int pos) { if (pos == at) st.rangeDb = 48.0f; });
            const float floor48 = (float) std::pow (10.0, -48.0 / 20.0);
            int reached = -1;
            for (int i = at; i < (int) x.size(); ++i)
                if (r.gain[(size_t) i] <= floor48) { reached = i; break; }
            bool closed = true;
            for (const int v : r.state)
                closed = closed && v == closedS;
            report.add ("g8b_range_deeper_closed", (double) (reached - at + 1), reached - at + 1 == samples (120.0f, 48000.0) / 2 && closed);
        }

        // G8c: RANGE 48 -> 24 dB while closed: the gain follows the floor up within the 16-sample glide.
        {
            Settings s;
            s.rangeDb = 48.0f;
            const std::vector<float> x (9600, 0.0f);
            const int at = 4800;
            const auto r = run (48000.0, x, s, 16, [&] (Settings& st, int pos) { if (pos == at) st.rangeDb = 24.0f; });
            report.add ("g8c_range_shallower_closed", (double) r.gain[(size_t) at + 15],
                        std::abs (r.gain[(size_t) at + 15] - floor24) <= 0.0f && r.gain[(size_t) at] > (float) std::pow (10.0, -48.0 / 20.0));
        }

        // G8d: RMS mode, 10 s of a loud signal then silence: the key stays finite, the gate closes.
        {
            Settings s;
            s.mode = Mode::rms;
            const auto x = sine (48000.0, 11.0, 220.0, -6.0f, 0.0, 10.0);
            const auto r = run (48000.0, x, s, 16, none);
            bool finite = true;
            for (size_t i = 0; i < x.size(); ++i)
                finite = finite && std::isfinite (r.key[i]) && std::isfinite (r.gain[i]);
            report.add ("g8d_rms_silence_finite", (double) r.key.back(), finite && r.state.back() == closedS);
        }

        // G9: block equivalence: control blocks of 1, 7 and 16 samples (set() before each, as the base) give identical
        // gains on a signal with opens, holds, releases and a RANGE change.
        {
            auto x = add (sine (48000.0, 2.0, 330.0, -18.0f, 0.2, 0.9), sine (48000.0, 2.0, 1200.0, -45.0f, 0.9, 2.0));
            // The change lands on sample 60032, a multiple of 7 and of 16, so every block size sees it at the same sample.
            auto change = [] (Settings& st, int pos) { st.rangeDb = pos < 60032 ? 24.0f : 40.0f; };
            const auto a = run (48000.0, x, Settings {}, 16, change);
            float worst = 0.0f;
            for (const int block : { 1, 7 })
            {
                const auto c = run (48000.0, x, Settings {}, block, change);
                for (size_t i = 0; i < a.gain.size(); ++i)
                    worst = std::max (worst, std::abs (a.gain[i] - c.gain[i]));
            }
            report.add ("g9_block_equivalence", (double) worst, worst <= 0.0f);
        }

        // G9b: RANGE 0 with control blocks of 1 and 7 samples: the gate still reaches Open and its state output says so.
        {
            Settings s;
            s.rangeDb = 0.0f;
            const auto x = sine (48000.0, 1.0, 220.0, -12.0f, 0.0, 1.0);
            bool ok = true;
            for (const int block : { 1, 7 })
            {
                const auto r = run (48000.0, x, s, block, none);
                ok = ok && r.state.back() == openS && r.states.back() == (uint8_t) S::open;
            }
            report.add ("g9b_range_zero_small_blocks_open", ok ? 1.0 : 0.0, ok);
        }

        // G10: rate equivalence: HOLD + LOOKAHEAD in ms is the same at 44.1, 48 and 96 kHz within 0.05 ms.
        {
            Settings s;
            double lo = 1.0e9, hi = -1.0e9;
            for (const double rate : { 44100.0, 48000.0, 96000.0 })
            {
                const auto x = sine (rate, 0.6, 1000.0, -20.0f, 0.05, 0.25);
                const auto r = run (rate, x, s, 16, none);
                const int holdAt = first (r.state, 0, holdS);
                const int releaseAt = first (r.state, holdAt, releaseS);
                const double ms = (double) (releaseAt - holdAt) * 1000.0 / rate;
                lo = std::min (lo, ms);
                hi = std::max (hi, ms);
            }
            report.add ("g10_rate_equivalence_ms", hi - lo, hi - lo <= 0.05);
        }

        // G11: DETECTOR Peak -> RMS mid-signal: no jump in the gain, nothing non-finite.
        {
            const auto x = sine (48000.0, 1.0, 440.0, -20.0f, 0.05, 1.0);
            const auto r = run (48000.0, x, Settings {}, 16, [] (Settings& st, int pos) { if (pos >= 24000) st.mode = Mode::rms; });
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
            report.add ("g12_denormal_input", (double) r.gain.back(), ok && std::abs (r.gain.back() - floor24) <= 0.0f);
        }

        // G13: determinism: the same input twice after reset gives identical gains.
        {
            std::vector<float> x (96000);
            Rng rng;
            rng.seed (0x13u);
            for (size_t i = 0; i < x.size(); ++i)
                x[i] = ((i / 9000) % 2 == 0 ? 0.2f : 0.001f) * rng.bi();
            const auto a = run (48000.0, x, Settings {}, 16, none);
            const auto b = run (48000.0, x, Settings {}, 16, none);
            float worst = 0.0f;
            for (size_t i = 0; i < a.gain.size(); ++i)
                worst = std::max (worst, std::abs (a.gain[i] - b.gain[i]));
            report.add ("g13_determinism", (double) worst, worst <= 0.0f);
        }

        // G14: retrigger: a second burst during Release goes Release -> Attack (never Closed in between) and the gain
        // rises from where it was, never falling and never faster than the attack slew.
        {
            Settings s;
            // Burst 1 ends at 0.20 s; the key falls below close about 55 ms later, Hold lasts 45 ms, Release 120 ms:
            // a second burst at 0.35 s lands about 50 ms into Release.
            const auto x = add (sine (48000.0, 0.8, 1000.0, -20.0f, 0.05, 0.20), sine (48000.0, 0.8, 1000.0, -20.0f, 0.35, 0.6));
            const auto r = run (48000.0, x, s, 16, none);
            const int releaseAt = first (r.state, 0, releaseS);
            const int againAt = first (r.state, releaseAt, attackS);
            bool ok = releaseAt > 0 && againAt > releaseAt && ! ever (r.state, releaseAt, againAt, closedS);
            const double up = std::pow (10.0, 24.0 / 48.0 / 20.0) * (1.0 + 1.0e-6);
            for (int i = againAt; ok && i < (int) r.gain.size() && r.gain[(size_t) i - 1] < 1.0f; ++i)
                ok = r.gain[(size_t) i] >= r.gain[(size_t) i - 1] && (double) r.gain[(size_t) i] <= (double) r.gain[(size_t) i - 1] * up;
            report.add ("g14_retrigger_from_release", ok ? 1.0 : 0.0, ok);
        }

        // G15: a dip shorter than HOLD: the key falls below close (fast detector) but returns in time: Hold -> Open,
        // no Release, the gain stays 1.
        {
            Settings s;
            s.detReleaseMs = 2.0f;
            const auto x = add (sine (48000.0, 0.6, 1000.0, -20.0f, 0.05, 0.25), sine (48000.0, 0.6, 1000.0, -20.0f, 0.27, 0.45));
            const auto r = run (48000.0, x, s, 16, none);
            const int openAt = first (r.state, 0, openS);
            const int holdAt = first (r.state, openAt, holdS);
            const int backAt = first (r.state, holdAt, openS);
            bool ok = holdAt > 0 && backAt > holdAt && backAt < (int) (0.28 * 48000) && ! ever (r.state, openAt, (int) (0.44 * 48000), releaseS);
            for (int i = openAt; ok && i < (int) (0.44 * 48000); ++i)
                ok = r.gain[(size_t) i] >= 1.0f;
            report.add ("g15_short_dip_holds", (double) (backAt - holdAt), ok);
        }

        // G16: a false start: ATTACK 20 ms and a 2 ms click: Attack -> Hold (the lookahead only) -> Release -> Closed,
        // never Open.
        {
            Settings s;
            s.attackMs = 20.0f;
            s.detReleaseMs = 2.0f;
            const auto x = sine (48000.0, 0.5, 1000.0, -20.0f, 0.1, 0.102);
            const auto r = run (48000.0, x, s, 16, none);
            const int attackAt = first (r.state, 0, attackS);
            const int holdAt = first (r.state, attackAt, holdS);
            const int releaseAt = first (r.state, holdAt, releaseS);
            const bool ok = attackAt > 0 && holdAt > attackAt && releaseAt - holdAt == 240 && ! ever (r.state, 0, (int) r.state.size(), openS)
                            && r.state.back() == closedS;
            report.add ("g16_false_start", (double) (releaseAt - holdAt), ok);
        }

        // G17: LOW CUT: a -30 dBFS 40 Hz sine opens the gate with LOW CUT off (20 Hz) and leaves it closed at 300 Hz.
        {
            const auto x = sine (48000.0, 0.5, 40.0, -30.0f, 0.0, 0.5);
            Settings off, on;
            off.lowCutHz = 20.0f;
            on.lowCutHz = 300.0f;
            const auto a = run (48000.0, x, off, 16, none);
            const auto b = run (48000.0, x, on, 16, none);
            const bool ok = ever (a.state, 0, (int) x.size(), openS) && ! ever (b.state, 0, (int) x.size(), attackS);
            report.add ("g17_low_cut", ok ? 1.0 : 0.0, ok);
        }

        // G17b: LOW CUT switched back on after a loud low passage: no stale filter state opens the gate on silence.
        {
            auto x = sine (48000.0, 1.6, 110.0, -14.0f, 0.0, 0.6);
            const auto r = run (48000.0, x, Settings {}, 16, [] (Settings& st, int pos)
            {
                st.lowCutHz = pos >= (int) (0.3 * 48000) && pos < (int) (1.2 * 48000) ? 20.0f : 80.0f;
            });
            const bool ok = ! ever (r.state, (int) (1.2 * 48000), (int) x.size(), attackS) && r.state.back() == closedS;
            report.add ("g17b_low_cut_reenable", ok ? 1.0 : 0.0, ok);
        }

        // G18: calibration: a 400 Hz sine at -20 dBFS reads -20 dB on the Peak key (its maximum over a cycle) and
        // -23.01 dB on the RMS key (2.5 ms is exactly one period), LOW CUT off.
        {
            Settings s;
            s.lowCutHz = 20.0f;
            const auto x = sine (48000.0, 0.3, 400.0, -20.0f, 0.0, 0.3);
            const auto p = run (48000.0, x, s, 16, none);
            s.mode = Mode::rms;
            const auto q = run (48000.0, x, s, 16, none);
            float peakMax = -200.0f, rmsLo = 200.0f, rmsHi = -200.0f;
            for (size_t i = (size_t) (0.2 * 48000); i < x.size(); ++i)
            {
                peakMax = std::max (peakMax, p.key[i]);
                rmsLo = std::min (rmsLo, q.key[i]);
                rmsHi = std::max (rmsHi, q.key[i]);
            }
            const bool ok = std::abs (peakMax + 20.0f) <= 0.1f && std::abs (rmsLo + 23.01f) <= 0.1f && std::abs (rmsHi + 23.01f) <= 0.1f;
            report.add ("g18_key_calibration_peak_db", (double) peakMax, ok);
        }

        // G19: DET RELEASE: after a DC step to zero, the Peak key falls 20 dB in DET RELEASE x ln 10 samples (+-1).
        {
            Settings s;
            s.lowCutHz = 20.0f;
            s.detReleaseMs = 10.0f;
            std::vector<float> x (24000, 0.0f);
            std::fill (x.begin(), x.begin() + 4800, 0.1f);
            const auto r = run (48000.0, x, s, 16, none);
            int fell = -1;
            for (int i = 4800; i < (int) x.size(); ++i)
                if (r.key[(size_t) i] <= -40.0f) { fell = i; break; }
            const int expected = (int) std::lround (0.010 * 48000.0 * std::log (10.0));
            report.add ("g19_det_release_samples", (double) (fell - 4800 + 1), std::abs (fell - 4800 + 1 - expected) <= 1);
        }

        // G20: HOLD turned down during a hold takes effect at once (500 ms -> 1 ms).
        {
            Settings s;
            s.holdMs = 500.0f;
            const auto x = sine (48000.0, 1.2, 1000.0, -20.0f, 0.05, 0.2);
            int changedAt = -1;
            const auto r = run (48000.0, x, s, 16, [&] (Settings& st, int pos)
            {
                if (pos >= (int) (0.3 * 48000))
                {
                    if (changedAt < 0)
                        changedAt = pos;
                    st.holdMs = 1.0f;
                }
            });
            const int releaseAt = first (r.state, changedAt, releaseS);
            const bool ok = changedAt > 0 && releaseAt > 0 && releaseAt - changedAt <= samples (1.0f, 48000.0) + 240 + 1;
            report.add ("g20_hold_change_during_hold", (double) (releaseAt - changedAt), ok);
        }

        return report;
    }
}
