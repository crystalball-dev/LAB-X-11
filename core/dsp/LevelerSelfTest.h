#pragma once

// The LevelMatcher's C++ self-tests, L1-L30 and L32 of docs/SPEC_LEVEL_MATCHER.md section L.12 (L31, the defaults
// against the parameter table, runs in test/harness_main.cpp, where EffectInfo is visible). Included at the end of
// Leveler.h; printed by LabX11Render --selftest as selftest_level_<case>=<value> and selftest_level_<case>_ok=<0|1>.
//
// Every signal is synthesised and seeded; every run is deterministic. The driver calls control() before every
// 16-sample block, as the base does, and process() one sample at a time so each case can read the gain per sample
// (L10 checks that real chunk sizes give the same output). Off the audio thread only: it allocates freely.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>
#include "Filters.h"
#include "Leveler.h"

namespace lab::dsp
{
    struct LevelMatcherTest
    {
        using M = LevelMatcher;
        using V = std::array<float, M::paramCount>;
        using Sig = std::vector<float>;
        using States = std::vector<uint8_t>;
        static constexpr double pi = 3.141592653589793, twoPi = 6.283185307179586;
        static constexpr uint8_t openS = M::open, closedS = M::closed;

        static V settings (float a, float t, float time, float b, float c, float lawValue) { return V { a, t, time, b, c, lawValue }; }
        static V defaultsV() { return settings (0.75f, -18.0f, 1.0f, 6.0f, 9.0f, 0.0f); }
        static V cV (float lawValue = 0.0f) { return settings (1.0f, -18.0f, 1.0f, 18.0f, 18.0f, lawValue); }   // C of L.12

        static bool same (double a, double b) noexcept { return ! (a < b) && ! (b < a); }
        static size_t samples (double sr, double seconds) { return (size_t) std::llround (seconds * sr); }
        static double db (double ratio) { return 10.0 * std::log10 (std::max (ratio, 1.0e-300)); }
        static std::string rateName (double sr) { return sr < 45000.0 ? "44k" : sr < 50000.0 ? "48k" : "96k"; }

        // ------------------------------------------------------------------------------------------------------------
        // Signals

        static Sig sine (double sr, double seconds, double hz, double peak)
        {
            Sig x (samples (sr, seconds));
            for (size_t i = 0; i < x.size(); ++i)
                x[i] = (float) (peak * std::sin (twoPi * hz * (double) i / sr));
            return x;
        }

        static double dbfs (double d) { return std::pow (10.0, d / 20.0); }

        // VOICE(seed, scale) of L.12: a 140 Hz harmonic stack at 1/h with seeded phases, times a 5 Hz syllable envelope
        // (0 to -10 dB), times 0.25 x scale. VOICE(., 10^(-6/20)) reads -20.486 LUFS for any seed.
        static Sig voice (double sr, double seconds, uint32_t seed, double scale, bool envelope = true)
        {
            Rng rng;
            rng.seed (seed);
            std::array<double, 24> phase {};
            for (auto& p : phase)
                p = twoPi * (double) rng.uni();
            Sig x (samples (sr, seconds));
            for (size_t i = 0; i < x.size(); ++i)
            {
                const double t = (double) i / sr;
                double s = 0.0;
                for (int h = 1; h <= 24 && 140.0 * h < 0.45 * sr; ++h)
                    s += std::sin (twoPi * 140.0 * h * t + phase[(size_t) h - 1]) / (double) h;
                const double env = envelope ? std::pow (10.0, -5.0 * (1.0 - std::cos (twoPi * 5.0 * t)) / 20.0) : 1.0;
                x[i] = (float) (s * env * 0.25 * scale);
            }
            return x;
        }

        static Sig white (Rng& rng, size_t n, double peak)
        {
            Sig x (n);
            for (auto& v : x)
                v = (float) (peak * (double) rng.bi());
            return x;
        }

        static Sig pink (Rng& rng, size_t n, double gain)   // Paul Kellet's economy filter, as the harness's "pink"
        {
            Sig x (n);
            float b0 = 0.0f, b1 = 0.0f, b2 = 0.0f;
            for (auto& v : x)
            {
                const float w = rng.bi();
                b0 = 0.99765f * b0 + w * 0.0990460f;
                b1 = 0.96300f * b1 + w * 0.2965164f;
                b2 = 0.57000f * b2 + w * 1.0526913f;
                v = (float) gain * (b0 + b1 + b2 + w * 0.1848f) * 0.25f;
            }
            return x;
        }

        // Vocal-tract noise: white through three band-passes (500 / 1500 / 2500 Hz, Q 3) summed, then a 4 kHz one-pole.
        static Sig tract (Rng& rng, size_t n, double sr)
        {
            SVF a, b, c;
            a.setHz (500.0f, 3.0f, (float) sr);
            b.setHz (1500.0f, 3.0f, (float) sr);
            c.setHz (2500.0f, 3.0f, (float) sr);
            OnePole lp;
            lp.setHz (4000.0f, (float) sr);
            Sig x (n);
            for (auto& v : x)
            {
                const float w = rng.bi();
                v = lp.lowpass (a.bandpass (w) + b.bandpass (w) + c.bandpass (w));
            }
            return x;
        }

        static double rms (const Sig& x, size_t from = 0, size_t to = std::numeric_limits<size_t>::max())
        {
            to = std::min (to, x.size());
            double s = 0.0;
            for (size_t i = from; i < to; ++i)
                s += (double) x[i] * (double) x[i];
            return std::sqrt (s / (double) std::max<size_t> (1, to - from));
        }

        static void scaleTo (Sig& x, double wantRms)
        {
            const double g = wantRms / std::max (rms (x), 1.0e-30);
            for (auto& v : x)
                v = (float) (v * g);
        }

        static void append (Sig& dst, States& st, const Sig& src, uint8_t state)
        {
            dst.insert (dst.end(), src.begin(), src.end());
            st.insert (st.end(), src.size(), state);
        }

        // ------------------------------------------------------------------------------------------------------------
        // The driver

        struct Run
        {
            Sig outL, outR, lamp, level;
            std::vector<double> applied, amountNow, ride;
            std::vector<int32_t> admitted, rejected;   // cumulative hop counts after each sample
        };

        // Runs m over (inL, inR, st) from its current state: settings v (copied), ctl (m, pos, v) before each block.
        template <typename Ctl>
        static Run drive (M& m, const Sig& inL, const Sig& inR, const States& st, V v, Ctl&& ctl, int block = 16)
        {
            Run out;
            const size_t total = inL.size();
            out.outL.resize (total);
            out.outR.resize (total);
            out.lamp.resize (total);
            out.level.resize (total);
            out.applied.resize (total);
            out.amountNow.resize (total);
            out.ride.resize (total);
            out.admitted.resize (total);
            out.rejected.resize (total);
            for (size_t pos = 0; pos < total; pos += (size_t) block)
            {
                const size_t n = std::min ((size_t) block, total - pos);
                ctl (m, pos, v);
                m.control (v.data());
                for (size_t i = pos; i < pos + n; ++i)
                {
                    float l = inL[i], r = inR[i];
                    const uint8_t s = st[i];
                    m.process (&l, &r, &s, 1);
                    out.outL[i] = l;
                    out.outR[i] = r;
                    out.lamp[i] = m.voiceLamp();
                    out.level[i] = m.levelDb();
                    out.applied[i] = m.appliedDb;
                    out.amountNow[i] = m.amountNow;
                    out.ride[i] = m.rideDb;
                    out.admitted[i] = (int32_t) m.hopsAdmitted;
                    out.rejected[i] = (int32_t) m.hopsRejected;
                }
            }
            return out;
        }

        static Run drive (M& m, const Sig& in, const States& st, V v)
        {
            return drive (m, in, in, st, v, [] (M&, size_t, V&) {});
        }

        static M fresh (double sr)
        {
            M m;
            m.prepare (sr);
            return m;
        }

        // Largest per-sample |change of appliedDb| over the slew cap 6 (1 - e^(-1 / (tau sr))) (L5).
        static double slewRatio (const Run& r, double sr, double tau = 1.0)
        {
            const double cap = 6.0 * (1.0 - std::exp (-1.0 / (tau * sr)));
            double worst = 0.0;
            for (size_t i = 1; i < r.applied.size(); ++i)
                worst = std::max (worst, std::abs (r.applied[i] - r.applied[i - 1]) / cap);
            return worst;
        }

        // ------------------------------------------------------------------------------------------------------------

        static SelfTestReport run();
    };

    inline SelfTestReport LevelMatcher::selfTest() { return LevelMatcherTest::run(); }

    inline SelfTestReport LevelMatcherTest::run()
    {
        SelfTestReport rep;
        auto name = [] (const std::string& s) { return "level_" + s; };
        auto add = [&] (const std::string& s, double value, bool ok) { rep.add (name (s).c_str(), value, ok); };
        auto none = [] (M&, size_t, V&) {};
        const double rates[] = { 44100.0, 48000.0, 96000.0 };
        double slewWorst = 0.0;   // L5, across the L4, L8, L15 and L20 runs

        // L1: K-weighted level of a 997 Hz sine at -20 dBFS peak reads -20.0 LUFS (L+R summed, -0.691).
        for (const double sr : rates)
        {
            M m = fresh (sr);
            const auto x = sine (sr, 3.0, 997.0, 0.1);
            drive (m, x, States (x.size(), openS), defaultsV());
            add ("k997_" + rateName (sr), m.levelDb(), std::abs (m.levelDb() + 20.0f) <= 0.05f);
        }

        // L2: the K-weighting's shape, run directly on sines (gain of the 2nd second's RMS).
        for (const double sr : rates)
        {
            auto kGain = [&] (double hz)
            {
                M m = fresh (sr);
                const auto x = sine (sr, 2.0, hz, 0.5);
                Sig y (x.size());
                for (size_t i = 0; i < x.size(); ++i)
                    y[i] = m.highPass[0].process (m.shelf[0].process (x[i]));
                return 20.0 * std::log10 (rms (y, samples (sr, 1.0)) / rms (x, samples (sr, 1.0)));
            };
            const double k997 = kGain (997.0), k100 = kGain (100.0) - k997, k4k = kGain (4000.0) - k997;
            add ("kshape_100_" + rateName (sr), k100, std::abs (k100 + 1.82) <= 0.02);
            add ("kshape_4k_" + rateName (sr), k4k, std::abs (k4k - 3.28) <= 0.02);
            add ("kshape_997_" + rateName (sr), k997, std::abs (k997 - 0.69) <= 0.03);
        }

        // L3: voicing. Tones and the harmonic voice are admitted on every hop; 2 / 4 kHz tones and noise on none.
        for (const double sr : rates)
        {
            const size_t n = samples (sr, 10.0);
            const int hops = (int) (n / (size_t) M::lookahead (sr));
            struct Case { const char* label; Sig x; bool voiced; };
            Rng w, p;
            w.seed (1);
            p.seed (1);
            std::vector<Case> cases;
            cases.push_back ({ "997", sine (sr, 10.0, 997.0, 0.1), true });
            cases.push_back ({ "300", sine (sr, 10.0, 300.0, 0.1), true });
            cases.push_back ({ "voice", voice (sr, 10.0, 7, 0.5), true });
            cases.push_back ({ "2k", sine (sr, 10.0, 2000.0, 0.1), false });
            cases.push_back ({ "4k", sine (sr, 10.0, 4000.0, 0.1), false });
            cases.push_back ({ "white", white (w, n, 0.1), false });
            cases.push_back ({ "pink", pink (p, n, 0.05), false });
            for (const auto& c : cases)
            {
                M m = fresh (sr);
                drive (m, c.x, States (c.x.size(), openS), cV());
                const bool ok = c.voiced ? (m.hopsAdmitted == hops && m.hopsRejected == 0 && m.valid)
                                         : (m.hopsAdmitted == 0 && same (m.levelDb(), M::noLevel));
                add (std::string ("voicing_") + c.label + "_" + rateName (sr), (double) m.hopsAdmitted, ok);
                if (std::string (c.label) == "997")
                {
                    double lo = 0.0, hi = 0.0;
                    for (size_t k = 0; k < (size_t) M::voiceHops; ++k)
                    {
                        lo += m.loRing[k];
                        hi += m.hiRing[k];
                    }
                    const double ratio = db (lo / hi);
                    add ("voicing_ratio997_" + rateName (sr), ratio, std::abs (ratio - 8.3) <= 0.4);
                }
            }
        }

        // L4: convergence on a -30 dBFS sine toward -18: +12 dB, within 1 dB at 2.89 s and 0.1 dB at 5.19 s, no
        // overshoot. L18 continues the 48 kHz run after a reset().
        for (const double sr : rates)
        {
            M m = fresh (sr);
            const auto x = sine (sr, 10.2, 997.0, dbfs (-30.0));
            const size_t n10 = samples (sr, 10.0);
            const Sig first (x.begin(), x.begin() + (long) n10);
            const auto r = drive (m, first, States (n10, openS), cV());
            const double final = r.applied.back();
            double t1 = 0.0, t01 = 0.0, peak = -1.0e9;
            for (size_t i = 0; i < n10; ++i)
            {
                if (std::abs (r.applied[i] - final) > 1.0) t1 = (double) i / sr;
                if (std::abs (r.applied[i] - final) > 0.1) t01 = (double) i / sr;
                peak = std::max (peak, r.applied[i]);
            }
            const auto rn = rateName (sr);
            add ("converge_gain_" + rn, final, std::abs (final - 12.0) <= 0.03);
            add ("converge_t1db_" + rn, t1, std::abs (t1 - 2.89) <= 0.01);
            add ("converge_t01db_" + rn, t01, std::abs (t01 - 5.19) <= 0.02);
            add ("converge_over_" + rn, peak - final, peak - final <= 0.001);
            if (rn == "48k")
            {
                slewWorst = std::max (slewWorst, slewRatio (r, sr));

                // L18: reset() mid-signal: 0 dB and no level at once; the next 4799 samples pass unchanged and sample
                // 4799 (the end of the 20th admitted hop) is the first the ride moves.
                m.reset();
                const Sig rest (x.begin() + (long) n10, x.end());
                const bool zero = same (m.gainDb(), 0.0) && same (m.levelDb(), M::noLevel);
                const auto r2 = drive (m, rest, States (rest.size(), openS), cV());
                const size_t firstMove = (size_t) (M::lookahead (sr) * M::minFillHops - 1);
                bool unchanged = true;
                for (size_t i = 0; i < firstMove; ++i)
                    unchanged = unchanged && same (r2.outL[i], rest[i]);
                const bool moved = ! same (r2.outL[firstMove], rest[firstMove]);
                add ("reset", (double) firstMove, zero && unchanged && moved);
            }
        }

        // L6: the limits: a -36 dBFS sine boosts to exactly +6, a -6 dBFS sine cuts to exactly -9.
        {
            const double sr = 48000.0;
            const V v = settings (1.0f, -18.0f, 1.0f, 6.0f, 9.0f, 0.0f);
            M a = fresh (sr), b = fresh (sr);
            const auto lo = sine (sr, 10.0, 997.0, dbfs (-36.0)), hi = sine (sr, 10.0, 997.0, dbfs (-6.0));
            const auto ra = drive (a, lo, States (lo.size(), openS), v);
            const auto rb = drive (b, hi, States (hi.size(), openS), v);
            const double maxA = *std::max_element (ra.applied.begin(), ra.applied.end());
            const double minB = *std::min_element (rb.applied.begin(), rb.applied.end());
            add ("limit_boost", ra.applied.back(), maxA <= 6.0 + 1.0e-9 && ra.applied.back() >= 5.99);
            add ("limit_cut", rb.applied.back(), minB >= -9.0 - 1.0e-9 && rb.applied.back() <= -8.99);
        }

        // L7: AMOUNT scales the ride: 0.5 and 0.75 of +12 dB; 0 passes the signal bit-exactly.
        {
            const double sr = 48000.0;
            const auto x = sine (sr, 10.0, 997.0, dbfs (-30.0));
            for (const float a : { 0.5f, 0.75f, 0.0f })
            {
                M m = fresh (sr);
                V v = cV();
                v[M::amount] = a;
                const auto r = drive (m, x, States (x.size(), openS), v);
                if (a > 0.0f)
                {
                    const double want = 12.0 * a;
                    add (a < 0.6f ? "amount_050" : "amount_075", r.applied.back(), std::abs (r.applied.back() - want) <= 0.03);
                }
                else
                {
                    float worst = 0.0f;
                    for (size_t i = 0; i < x.size(); ++i)
                        worst = std::max (worst, std::abs (r.outL[i] - x[i]));
                    add ("amount_000", (double) worst, worst <= 0.0f && same (m.gainDb(), 0.0));
                }
            }
        }

        // L8: freeze. 5 s Closed between two -30 dBFS spans: the gain holds bit-exactly, the level too.
        {
            const double sr = 48000.0;
            auto x = sine (sr, 9.5, 997.0, dbfs (-30.0));
            States st (x.size(), openS);
            Rng rng;
            rng.seed (1);
            const size_t a = samples (sr, 1.5), b = samples (sr, 6.5);
            for (size_t i = a; i < b; ++i)
            {
                x[i] = (float) (dbfs (-60.0) * (double) rng.bi());
                st[i] = closedS;
            }
            M m = fresh (sr);
            const auto r = drive (m, x, st, cV());
            double lo = 1.0e9, hi = -1.0e9;
            bool levelSame = true;
            for (size_t i = a; i < b; ++i)
            {
                lo = std::min (lo, r.applied[i]);
                hi = std::max (hi, r.applied[i]);
                levelSame = levelSame && same (r.level[i], r.level[a - 1]);
            }
            add ("freeze_pp", hi - lo, same (hi, lo) && levelSame);
            add ("freeze_value", r.applied[a - 1], same (r.applied[a], r.applied[a - 1]) && std::abs (r.applied[a - 1] - 7.978) <= 0.005);
            slewWorst = std::max (slewWorst, slewRatio (r, sr));
        }

        // L9: consonant-like bursts (15 ms, peaks 20 dB over a sine at TARGET) neither pump nor crush. L19 repeats it.
        auto burstSignal = [] (double sr)
        {
            auto x = sine (sr, 12.0, 997.0, dbfs (-24.0));
            Rng rng;
            rng.seed (12345);
            const int len = (int) std::lround (0.015 * sr);
            for (int k = 0; 0.25 + 0.923 * k < 12.0; ++k)
            {
                const size_t at = samples (sr, 0.25 + 0.923 * k);
                for (int j = 0; j < len && at + (size_t) j < x.size(); ++j)
                    x[at + (size_t) j] += (float) ((double) rng.bi() * dbfs (-4.0) * std::exp (-(double) j / (0.004 * sr)));
            }
            return x;
        };
        {
            const double sr = 48000.0;
            const auto x = burstSignal (sr);
            const V v = settings (1.0f, -24.0f, 1.0f, 6.0f, 9.0f, 0.0f);
            M m = fresh (sr);
            const auto r = drive (m, x, States (x.size(), openS), v);
            double lo = 1.0e9, hi = -1.0e9, d10 = 0.0;
            const size_t from = samples (sr, 3.0), ten = samples (sr, 0.01);
            for (size_t i = from; i < x.size(); ++i)
            {
                lo = std::min (lo, r.applied[i]);
                hi = std::max (hi, r.applied[i]);
                if (i + ten < x.size())
                    d10 = std::max (d10, std::abs (r.applied[i + ten] - r.applied[i]));
            }
            add ("burst_min", lo, lo >= -0.01);
            add ("burst_max", hi, hi <= 0.01);
            add ("burst_pp", hi - lo, hi - lo <= 0.002);
            add ("burst_d10", d10, d10 <= 0.0005);
            add ("burst_rej", (double) m.hopsRejected, m.hopsRejected >= 13);

            // L19: the same run after reset() is bit-identical.
            m.reset();
            const auto r2 = drive (m, x, States (x.size(), openS), v);
            bool identical = true;
            for (size_t i = 0; i < x.size(); ++i)
                identical = identical && same (r.outL[i], r2.outL[i]) && same (r.applied[i], r2.applied[i]);
            add ("determinism", identical ? 1.0 : 0.0, identical);
        }

        // L10: block independence: chunks of 1, 7, 64, 512 and random 1..33 against 16, Ride and Step.
        for (const float lawValue : { 0.0f, 1.0f })
        {
            const double sr = 48000.0;
            Rng noise;
            noise.seed (3);
            auto x = white (noise, samples (sr, 4.0), dbfs (-60.0));
            States st (x.size(), closedS);
            const double starts[] = { 0.0, 1.3, 2.6 }, levels[] = { -30.0, -20.0, -26.0 };
            for (int k = 0; k < 3; ++k)
            {
                const size_t a = samples (sr, starts[k]);
                const auto s = sine (sr, 1.0, 997.0, dbfs (levels[k]));
                for (size_t i = 0; i < s.size() && a + i < x.size(); ++i)
                {
                    x[a + i] += s[i];
                    st[a + i] = openS;
                }
            }
            auto chunked = [&] (int fixed)
            {
                M m = fresh (sr);
                Sig l = x, r = x;
                Rng sizes;
                sizes.seed (99);
                const V v = cV (lawValue);
                for (size_t pos = 0; pos < x.size();)
                {
                    const size_t n = std::min (x.size() - pos, fixed > 0 ? (size_t) fixed : (size_t) (1 + sizes.next() % 33u));
                    m.control (v.data());
                    m.process (l.data() + pos, r.data() + pos, st.data() + pos, (int) n);
                    pos += n;
                }
                return l;
            };
            const auto reference = chunked (16);
            float worst = 0.0f;
            for (const int c : { 1, 7, 64, 512, 0 })
            {
                const auto other = chunked (c);
                for (size_t i = 0; i < x.size(); ++i)
                    worst = std::max (worst, std::abs (other[i] - reference[i]));
            }
            add (lawValue > 0.5f ? "blocks_law1" : "blocks_law0", (double) worst, worst <= 0.0f);
        }

        // L11 / L13: Step. 8 phrases, the state Open from LA before the audio (the gate's pre-open window); the gain
        // moves only inside those windows, onto the 1.5 dB grid, with a raised-cosine pre-roll.
        auto stepPhrases = [] (double sr, const std::vector<double>& levels, Sig& x, States& st, std::vector<size_t>& onsets)
        {
            const size_t la = (size_t) M::lookahead (sr);
            const size_t total = samples (sr, 0.2 + 2.3 * (double) (levels.size() - 1) + 2.0 + 0.1) + la;
            x.assign (total, 0.0f);
            st.assign (total, closedS);
            onsets.clear();
            for (size_t k = 0; k < levels.size(); ++k)
            {
                const size_t ks = samples (sr, 0.2 + 2.3 * (double) k), ke = ks + samples (sr, 2.0);
                onsets.push_back (ks);
                for (size_t i = ks; i < ke + la; ++i)
                    st[i] = openS;
                const double peak = dbfs (levels[k]);
                for (size_t i = ks + la; i < ke + la; ++i)
                    x[i] = (float) (peak * std::sin (twoPi * 997.0 * (double) (i - ks - la) / sr));
            }
        };
        for (const double sr : rates)
        {
            Sig x;
            States st;
            std::vector<size_t> on;
            stepPhrases (sr, { -30, -30, -30, -30, -29.4, -29.4, -29.4, -29.4 }, x, st, on);
            M m = fresh (sr);
            const auto r = drive (m, x, st, cV (1.0f));
            const size_t la = (size_t) M::lookahead (sr), len = samples (sr, 2.0);
            const double want[] = { 0, 9, 12, 12, 12, 12, 12, 12 };
            bool onsetsOk = true, flat = true;
            for (size_t k = 0; k < on.size(); ++k)
            {
                onsetsOk = onsetsOk && same (r.applied[on[k] + la], want[k]);
                for (size_t i = on[k] + la + 1; i < on[k] + la + len; ++i)
                    flat = flat && same (r.applied[i], r.applied[i - 1]);
            }
            int outside = 0;
            for (size_t i = 1; i < x.size(); ++i)
                if (! same (r.applied[i], r.applied[i - 1]))
                {
                    bool inside = false;
                    for (const size_t o : on)
                        inside = inside || (i >= o && i < o + la);
                    outside += inside ? 0 : 1;
                }
            const auto rn = rateName (sr);
            add ("step_grid_onsets_" + rn, r.applied[on[2] + la], onsetsOk && flat && outside == 0);
            add ("step_grid_ride_" + rn, m.rideDb, std::abs (m.rideDb - 11.40) <= 0.03);

            // L13: the second onset's pre-roll, sample by sample.
            const size_t o = on[1];
            double worst = 0.0, peakStep = 0.0;
            for (size_t k = 1; k <= la; ++k)
            {
                const double expect = 9.0 * 0.5 * (1.0 - std::cos (pi * (double) k / (double) la));
                worst = std::max (worst, std::abs (r.applied[o + k - 1] - expect));
                peakStep = std::max (peakStep, std::abs (r.applied[o + k - 1] - r.applied[o + k - 2]));
            }
            const bool lands = same (r.applied[o + la - 1], 9.0);
            add ("step_preroll_" + rn, worst, worst <= 1.0e-9 && lands && peakStep <= 9.0 * std::sin (pi / (2.0 * (double) la)) + 1.0e-9);
        }

        // L12: Step holds a +0.6 dB change (under the 1 dB commit) and places a +1.6 dB one in one pre-roll.
        for (const double second : { -29.4, -28.4 })
        {
            const double sr = 48000.0;
            std::vector<double> levels (16, -30.0);
            for (size_t k = 8; k < 16; ++k)
                levels[k] = second;
            Sig x;
            States st;
            std::vector<size_t> on;
            stepPhrases (sr, levels, x, st, on);
            M m = fresh (sr);
            const auto r = drive (m, x, st, cV (1.0f));
            int changed = 0;
            for (size_t i = on[8]; i < x.size(); ++i)
                changed += same (r.applied[i], r.applied[i - 1]) ? 0 : 1;
            if (second < -29.0)
                add ("step_hold", (double) changed, changed == 0 && same (r.applied.back(), 12.0));
            else
                add ("step_move", (double) changed, changed == M::lookahead (sr) && same (r.applied.back(), 10.5)
                                                     && std::abs (m.rideDb - 10.40) <= 0.03);
        }

        // L14: Step's late commit after 4 s of voice with no gap: three moves, each starting at hopLen - 2 + k lateLen,
        // each at exactly the slew per sample: 0 -> 12, 12 -> 6, 6 -> 4.5.
        {
            const double sr = 48000.0;
            auto x = sine (sr, 16.0, 997.0, dbfs (-30.0));
            const size_t six = samples (sr, 6.0);
            for (size_t i = six; i < x.size(); ++i)
                x[i] = (float) (dbfs (-22.5) * std::sin (twoPi * 997.0 * (double) i / sr));
            M m = fresh (sr);
            const auto r = drive (m, x, States (x.size(), openS), cV (1.0f));
            const double cap = 6.0 * (1.0 - std::exp (-1.0 / sr));
            const int64_t hop = M::lookahead (sr), late = m.lateLen;
            const double ends[] = { 12.0, 6.0, 4.5 };
            bool ok = true;
            size_t i = 1;
            for (int k = 1; k <= 3; ++k)
            {
                while (i < x.size() && same (r.applied[i], r.applied[i - 1]))
                    ++i;
                ok = ok && (int64_t) i == hop - 2 + k * late;
                // the move: every step the full slew, the last one landing exactly on the grid value
                size_t j = i;
                while (j < x.size() && ! same (r.applied[j], r.applied[j - 1]))
                {
                    const double d = std::abs (r.applied[j] - r.applied[j - 1]);
                    const bool last = j + 1 >= x.size() || same (r.applied[j + 1], r.applied[j]);
                    ok = ok && (last ? d <= cap * (1.0 + 1.0e-9) : std::abs (d - cap) <= 1.0e-12);
                    ++j;
                }
                ok = ok && same (r.applied[j - 1], ends[k - 1]);
                i = j + 1;
            }
            add ("step_late", r.applied.back(), ok && same (r.applied.back(), 4.5) && std::abs (m.rideDb - 4.5) <= 0.01);
        }

        // L15: Ride -> Step -> Ride on a converging ride: every move within the slew cap; Step holds 7.5 dB.
        {
            const double sr = 48000.0;
            const auto x = sine (sr, 10.0, 997.0, dbfs (-30.0));
            M m = fresh (sr);
            const auto r = drive (m, x, x, States (x.size(), openS), cV(), [&] (M&, size_t pos, V& v)
            {
                const double t = (double) pos / sr;
                v[M::law] = t >= 1.5 && t < 4.0 ? 1.0f : 0.0f;
            });
            const double ratio = slewRatio (r, sr);
            slewWorst = std::max (slewWorst, ratio);
            add ("law_switch", r.applied[samples (sr, 3.0)], ratio <= 1.0 + 1.0e-9 && same (r.applied[samples (sr, 3.0)], 7.5)
                                                              && std::abs (r.applied.back() - 12.0) <= 0.03);
        }

        // L16: silence and denormal-sized input: no level, 0 dB, output == input, all finite.
        {
            const double sr = 48000.0;
            Sig x (samples (sr, 10.0), 0.0f);
            std::fill (x.begin() + (long) samples (sr, 5.0), x.end(), 1.0e-30f);
            M m = fresh (sr);
            const auto r = drive (m, x, States (x.size(), openS), cV());
            bool ok = same (m.levelDb(), M::noLevel);
            for (size_t i = 0; i < x.size(); ++i)
                ok = ok && same (r.applied[i], 0.0) && same (r.outL[i], x[i]) && std::isfinite (r.outL[i]);
            add ("silence", (double) m.levelDb(), ok);
        }

        // L17: the gate's hold tails (40 ms of room noise after each line) are rejected: the level stays the voice's.
        {
            const double sr = 48000.0;
            Rng rng;
            rng.seed (5);
            auto x = white (rng, samples (sr, 13.0), dbfs (-65.0));
            States st (x.size(), closedS);
            const auto tone = sine (sr, 1.0, 997.0, 0.1);
            std::vector<std::pair<size_t, size_t>> tails;
            for (int k = 0; k < 10; ++k)
            {
                const size_t a = samples (sr, 1.3 * k);
                for (size_t i = 0; i < tone.size(); ++i)
                    x[a + i] += tone[i];
                for (size_t i = a; i < a + samples (sr, 1.04); ++i)
                    st[i] = openS;
                tails.push_back ({ a + tone.size(), a + samples (sr, 1.04) });
            }
            M m = fresh (sr);
            const auto r = drive (m, x, st, cV());
            float lo = 1.0e9f, hi = -1.0e9f;
            for (size_t i = samples (sr, 6.0); i < x.size(); ++i)
            {
                lo = std::min (lo, r.level[i]);
                hi = std::max (hi, r.level[i]);
            }
            int fewest = 1000;
            for (const auto& [a, b] : tails)
                fewest = std::min (fewest, r.rejected[b - 1] - r.rejected[a - 1]);
            add ("holdtail_level", (double) hi, std::abs (lo + 20.0f) <= 0.01f && std::abs (hi + 20.0f) <= 0.01f);
            add ("holdtail_gain", r.applied.back(), std::abs (r.applied.back() - 2.0) <= 0.01);
            add ("holdtail_rejected", (double) fewest, fewest >= 7);
        }

        // L20: MAX BOOST lowered from 18 to 4 dB at 10 s: the ride glides down within the slew cap and is within 1 dB
        // of 4 after 2.125 s; the same change inside a Closed span leaves the gain untouched until voice returns.
        {
            const double sr = 48000.0;
            const auto x = sine (sr, 16.0, 997.0, dbfs (-30.0));
            auto lower = [&] (M&, size_t pos, V& v) { v[M::boost] = (double) pos / sr >= 10.0 ? 4.0f : 18.0f; };
            M m = fresh (sr);
            const auto r = drive (m, x, x, States (x.size(), openS), cV(), lower);
            double last = 0.0;
            for (size_t i = samples (sr, 10.0); i < x.size(); ++i)
                if (std::abs (r.applied[i] - 4.0) > 1.0)
                    last = (double) i / sr - 10.0;
            const double ratio = slewRatio (r, sr);
            slewWorst = std::max (slewWorst, ratio);
            add ("limit_change", last, ratio <= 1.0 + 1.0e-9 && std::abs (last - 2.125) <= 0.01 && r.applied.back() <= 4.03);

            States st (x.size(), openS);
            for (size_t i = samples (sr, 9.5); i < samples (sr, 11.0); ++i)
                st[i] = closedS;
            M m2 = fresh (sr);
            const auto r2 = drive (m2, x, x, st, cV(), lower);
            bool held = true;
            for (size_t i = samples (sr, 9.5); i < samples (sr, 11.0); ++i)
                held = held && same (r2.applied[i], r2.applied[samples (sr, 9.5) - 1]);
            add ("limit_change_closed", held ? 1.0 : 0.0, held);
        }

        // L21: one NaN input sample: exactly one non-finite output sample, the filters heal at the hop's end.
        {
            const double sr = 48000.0;
            auto l = sine (sr, 10.0, 997.0, dbfs (-30.0));
            const auto rr = l;
            l[samples (sr, 2.0)] = std::numeric_limits<float>::quiet_NaN();
            M m = fresh (sr);
            const auto r = drive (m, l, rr, States (l.size(), openS), cV(), none);
            int bad = 0;
            bool finiteGain = true;
            for (size_t i = 0; i < l.size(); ++i)
            {
                bad += (std::isfinite (r.outL[i]) ? 0 : 1) + (std::isfinite (r.outR[i]) ? 0 : 1);
                finiteGain = finiteGain && std::isfinite (r.applied[i]);
            }
            add ("nan_heal", (double) bad, bad == 1 && finiteGain && std::abs (r.applied.back() - 12.0) <= 0.03);
        }

        // L22, L22b: breaths through an open gate (D6) between phrases. Swelling breaths change nothing; abrupt ones
        // leak at most one hop.
        {
            const double sr = 48000.0;
            const auto phraseLoud = voice (sr, 2.0, 11, dbfs (-6.0));
            const auto phraseSoft = voice (sr, 2.0, 11, dbfs (-6.0) * 0.5);
            const size_t gap = samples (sr, 0.15), breathLen = samples (sr, 0.4);
            const V v = settings (1.0f, -18.0f, 1.0f, 12.0f, 12.0f, 0.0f);
            const char* types[] = { "white", "pink", "tract" };
            struct Layout { Sig x; States st; std::vector<size_t> phraseStarts; std::vector<std::pair<size_t, size_t>> breaths; };
            auto build = [&] (int type, double rel, bool withBreath, bool abrupt)
            {
                Layout lay;
                for (int k = 0; k < 6; ++k)
                {
                    const auto& p = (k % 2 == 1) ? phraseSoft : phraseLoud;
                    lay.phraseStarts.push_back (lay.x.size());
                    append (lay.x, lay.st, p, openS);
                    append (lay.x, lay.st, Sig (gap, 0.0f), closedS);
                    Rng rng;
                    rng.seed ((uint32_t) (77 + k));
                    Sig b = type == 0 ? white (rng, breathLen, 1.0) : type == 1 ? pink (rng, breathLen, 1.0) : tract (rng, breathLen, sr);
                    for (size_t i = 0; i < b.size(); ++i)
                    {
                        const double t = (double) i / sr;
                        const double env = abrupt ? std::min (1.0, t / 0.010) : std::sin (pi * (double) i / (double) b.size());
                        b[i] = (float) (b[i] * env);
                    }
                    scaleTo (b, rms (p) * dbfs (rel));
                    lay.breaths.push_back ({ lay.x.size(), lay.x.size() + breathLen });
                    if (withBreath)
                        append (lay.x, lay.st, b, openS);
                    else
                        append (lay.x, lay.st, Sig (breathLen, 0.0f), closedS);
                    append (lay.x, lay.st, Sig (gap, 0.0f), closedS);
                }
                return lay;
            };
            for (int type = 0; type < 3; ++type)
                for (const double rel : { -10.0, -15.0, -22.0 })
                {
                    const auto a = build (type, rel, true, false), b = build (type, rel, false, false);
                    M ma = fresh (sr), mb = fresh (sr);
                    const auto ra = drive (ma, a.x, a.st, v), rb = drive (mb, b.x, b.st, v);
                    double worst = 0.0;
                    for (const size_t s : a.phraseStarts)
                        for (size_t i = s; i < s + phraseLoud.size(); ++i)
                            worst = std::max (worst, std::abs (ra.applied[i] - rb.applied[i]));
                    add (std::string ("breath_") + types[type] + "_" + std::to_string ((int) -rel), worst, worst <= 1.0e-6);
                }
            for (int type = 0; type < 3; ++type)
                for (const double rel : { -10.0, -15.0 })
                {
                    const auto a = build (type, rel, true, true), b = build (type, rel, false, true);
                    M ma = fresh (sr), mb = fresh (sr);
                    const auto ra = drive (ma, a.x, a.st, v), rb = drive (mb, b.x, b.st, v);
                    double worst = 0.0, atOnset = 0.0;
                    for (size_t i = 0; i < a.x.size(); ++i)
                        worst = std::max (worst, std::abs (ra.applied[i] - rb.applied[i]));
                    for (size_t k = 1; k < a.phraseStarts.size(); ++k)
                        atOnset = std::max (atOnset, std::abs (ra.applied[a.phraseStarts[k]] - rb.applied[a.phraseStarts[k]]));
                    int leaked = 0;
                    for (const auto& [s, e] : a.breaths)
                        leaked = std::max (leaked, ra.admitted[e - 1] - ra.admitted[s - 1]);
                    add (std::string ("breath_abrupt_") + types[type] + "_" + std::to_string ((int) -rel), worst,
                         leaked <= 1 && worst <= 0.05 && atOnset <= 0.01);
                }
        }

        // L22c: an "s" at the start of a line, after a gap, is not admitted.
        {
            const double sr = 48000.0;
            const auto vowel = voice (sr, 2.0, 11, dbfs (-6.0));
            for (const double rel : { -10.0, -6.0 })
            {
                Sig x;
                States st;
                std::vector<std::pair<size_t, size_t>> ss;
                Rng rng;
                rng.seed (41);
                for (int k = 0; k < 8; ++k)
                {
                    append (x, st, Sig (samples (sr, 0.3), 0.0f), closedS);
                    Sig s = white (rng, samples (sr, 0.06), 1.0);
                    SVF h1, h2;
                    h1.setHz (4000.0f, 0.707f, (float) sr);
                    h2.setHz (4000.0f, 0.707f, (float) sr);
                    for (auto& y : s)
                        y = h2.highpass (h1.highpass (y));
                    scaleTo (s, rms (vowel) * dbfs (rel));
                    ss.push_back ({ x.size(), x.size() + s.size() });
                    append (x, st, s, openS);
                    append (x, st, vowel, openS);
                }
                M m = fresh (sr);
                const auto r = drive (m, x, st, settings (1.0f, -18.0f, 1.0f, 12.0f, 12.0f, 0.0f));
                int admitted = 0;
                for (const auto& [a, b] : ss)
                    admitted += r.admitted[b - 1] - r.admitted[a - 1];
                add ("onset_s_" + std::to_string ((int) -rel), (double) admitted, admitted == 0);
            }
        }

        // L23, L23c: a misset gate (Open everywhere). Broadband room tone and hum 30 dB under the voice hold the gain
        // over an 8 s gap; hum 22 dB under it is ridden toward MAX BOOST (the guard's documented reach).
        {
            const double sr = 48000.0;
            const auto phrase = voice (sr, 2.0, 13, dbfs (-12.0));
            auto hum = [&] (size_t n, double scale)
            {
                Sig h (n);
                for (size_t i = 0; i < n; ++i)
                {
                    const double t = (double) i / sr;
                    double s = 0.0;
                    for (int k = 1; k <= 7; ++k)
                        s += std::sin (twoPi * 60.0 * k * t) / k;
                    h[i] = (float) (s * scale);
                }
                return h;
            };
            const double humScale = 0.9 * dbfs (-55.0);
            auto layout = [&] (int kind, double gapSeconds, double scale, size_t& g0, size_t& g1, size_t& p3end)
            {
                Sig x;
                States st;
                Rng rng;
                rng.seed (9);
                auto filler = [&] (size_t n) { return kind == 0 ? white (rng, n, dbfs (-60.0) * std::sqrt (3.0)) : hum (n, scale); };
                for (int k = 0; k < 3; ++k)
                {
                    append (x, st, phrase, openS);
                    if (k == 2)
                        p3end = x.size();
                    if (k < 2)
                        append (x, st, filler (samples (sr, 0.3)), openS);
                }
                g0 = x.size();
                append (x, st, filler (samples (sr, gapSeconds)), openS);
                g1 = x.size();
                append (x, st, Sig (phrase.begin(), phrase.begin() + (long) samples (sr, 1.0)), openS);
                return std::make_pair (x, st);
            };
            for (const int kind : { 0, 1 })
            {
                size_t g0 = 0, g1 = 0, p3 = 0;
                const auto [x, st] = layout (kind, 8.0, humScale, g0, g1, p3);
                M cal = fresh (sr);
                const auto rc = drive (cal, x, st, settings (1.0f, -18.0f, 1.0f, 6.0f, 9.0f, 0.0f));
                const float target = rc.level[p3 - 1];
                M m = fresh (sr);
                const auto r = drive (m, x, st, settings (1.0f, target, 1.0f, 6.0f, 9.0f, 0.0f));
                double lo = 1.0e9, hi = -1.0e9;
                bool late = true;
                const size_t settle = g0 + samples (sr, 0.25);
                for (size_t i = g0; i < g1; ++i)
                {
                    lo = std::min (lo, r.applied[i]);
                    hi = std::max (hi, r.applied[i]);
                    if (i >= settle)
                        late = late && same (r.applied[i], r.applied[settle]);
                }
                const float lampMid = r.lamp[(g0 + g1) / 2];
                add (kind == 0 ? "open_gap_white" : "open_gap_hum", hi - lo, hi - lo <= 0.001 && late && same (lampMid, 0.5f));
            }

            // L23c: the same hum 22 dB under the voice's K-loudness, over a 3 s gap.
            {
                M humMeter = fresh (sr);
                const auto h = hum (samples (sr, 2.0), humScale);
                drive (humMeter, h, States (h.size(), openS), settings (1.0f, -60.0f, 1.0f, 18.0f, 9.0f, 0.0f));
                size_t g0 = 0, g1 = 0, p3 = 0;
                const auto probe = layout (1, 3.0, humScale, g0, g1, p3);
                M cal = fresh (sr);
                const auto rc = drive (cal, probe.first, probe.second, settings (1.0f, -18.0f, 1.0f, 6.0f, 9.0f, 0.0f));
                const float target = rc.level[p3 - 1];
                const double scale = humScale * dbfs ((double) target - 22.0 - (double) humMeter.levelDb());
                const auto [x, st] = layout (1, 3.0, scale, g0, g1, p3);
                M m = fresh (sr);
                const auto r = drive (m, x, st, settings (1.0f, target, 1.0f, 6.0f, 9.0f, 0.0f));
                const double rise = r.applied[g1 - 1] - r.applied[g0];
                add ("open_gap_bleed22", rise, rise >= 5.0 && same (r.lamp[(g0 + g1) / 2], 1.0f));
            }
        }

        // L24: a take that drops d dB: acquired within one phrase up to 20 dB, two at 24 dB; at 28 dB the guard holds.
        {
            const double sr = 48000.0;
            std::vector<Sig> phrases;
            for (uint32_t k = 0; k < 16; ++k)
                phrases.push_back (voice (sr, 2.0, 17 + k, 1.0));
            for (const double d : { 12.0, 20.0, 24.0, 28.0 })
            {
                Sig x;
                States st;
                std::vector<size_t> ends;
                for (size_t k = 0; k < 16; ++k)
                {
                    Sig p = phrases[k];
                    const double s = dbfs (-6.0) * (k >= 6 ? dbfs (-d) : 1.0);
                    for (auto& y : p)
                        y = (float) (y * s);
                    append (x, st, p, openS);
                    ends.push_back (x.size() - 1);
                    append (x, st, Sig (samples (sr, 0.3), 0.0f), closedS);
                }
                M m = fresh (sr);
                const auto r = drive (m, x, st, cV());
                const std::string label = "quiet_take_d" + std::to_string ((int) d);
                if (d < 22.0)
                    add (label, r.level[ends[6]], std::abs ((double) r.level[ends[6]] - (-20.486 - d)) <= 0.05);
                else if (d < 26.0)
                    add (label, r.level[ends[7]], std::abs ((double) r.level[ends[7]] - (-44.486)) <= 0.05);
                else
                    add (label, r.level[ends[15]], same (r.level[ends[15]], r.level[ends[5]]));
            }
        }

        // L25: AMOUNT 1 -> 0 -> 1 unsmoothed: the applied gain moves at <= 12 dB/s, reaches 0 dB in 1 s, passes the
        // signal bit-exactly while at 0, and is back on the ride 1 s after AMOUNT returns.
        {
            const double sr = 48000.0;
            const auto x = sine (sr, 16.0, 997.0, dbfs (-30.0));
            M m = fresh (sr);
            const auto r = drive (m, x, x, States (x.size(), openS), cV(), [&] (M&, size_t pos, V& v)
            {
                const double t = (double) pos / sr;
                v[M::amount] = t >= 10.0 && t < 13.0 ? 0.0f : 1.0f;
            });
            const size_t t10 = samples (sr, 10.0), t13 = samples (sr, 13.0);
            size_t zeroAt = t10;
            while (zeroAt < t13 && ! same (r.applied[zeroAt], 0.0))
                ++zeroAt;
            bool passes = true;
            for (size_t i = zeroAt; i < t13; ++i)
                passes = passes && same (r.outL[i], x[i]);
            size_t backAt = t13;
            while (backAt < x.size() && ! same (r.amountNow[backAt], 1.0))
                ++backAt;
            double worst = 0.0;
            for (size_t i = 1; i < x.size(); ++i)
                worst = std::max (worst, std::abs (r.applied[i] - r.applied[i - 1]));
            const double down = (double) (zeroAt - t10 + 1) / sr, up = (double) (backAt - t13 + 1) / sr;
            const double bound = (12.0 + 6.0) / sr * (1.0 + 1.0e-6);
            add ("amount_slew_down_s", down, std::abs (down - 1.0) <= 0.001 && passes && worst <= bound);
            add ("amount_slew_up_s", up, std::abs (up - 1.0) <= 0.001);
        }

        // L26: before any level exists, every knob may move and the output stays the input, bit for bit.
        {
            const double sr = 48000.0;
            auto x = sine (sr, 2.0, 997.0, dbfs (-30.0));
            States st (x.size(), closedS);
            Rng rng;
            rng.seed (31);
            append (x, st, white (rng, samples (sr, 2.0), 0.1), openS);
            M m = fresh (sr);
            const auto r = drive (m, x, x, st, cV(), [&] (M&, size_t pos, V& v)
            {
                const double t = (double) pos / sr;
                auto frac = [] (double y) { return y - std::floor (y); };
                v[M::target] = (float) (-40.0 + 34.0 * frac (t));
                v[M::boost] = (float) (18.0 * frac (1.3 * t));
                v[M::cut] = (float) (18.0 * frac (0.9 * t));
                v[M::amount] = (float) frac (0.7 * t);
            });
            bool ok = same (m.levelDb(), M::noLevel);
            for (size_t i = 0; i < x.size(); ++i)
                ok = ok && same (r.outL[i], x[i]) && same (r.applied[i], 0.0);
            add ("neutral_start", ok ? 1.0 : 0.0, ok);
        }

        // L27: a preset change keeps the measurement: flush(), then AMOUNT 0.5 and MAX BOOST 4: exactly +2 dB from the
        // first sample after the flush, the level unchanged.
        {
            const double sr = 48000.0;
            const auto x = sine (sr, 12.0, 997.0, dbfs (-30.0));
            const size_t t10 = samples (sr, 10.0);
            M m = fresh (sr);
            const auto r = drive (m, x, x, States (x.size(), openS), cV(), [&] (M& mm, size_t pos, V& v)
            {
                if (pos == t10)
                {
                    mm.flush();
                    v[M::amount] = 0.5f;
                    v[M::boost] = 4.0f;
                }
            });
            bool exact = true;
            for (size_t i = t10; i < x.size(); ++i)
                exact = exact && same (r.applied[i], 2.0);
            const bool before = ! same (r.applied[t10 - 1], 2.0);
            add ("flush", r.applied[t10], exact && before && std::abs (r.level[t10] - r.level[t10 - 1]) <= 1.0e-3f);
        }

        // L28 / L29: the burst clamp and the first-fill cap against reference instances with the switches off.
        auto staccato = [] (double sr, double seconds)
        {
            auto x = voice (sr, seconds, 19, 1.0, false);
            const double period = 0.1875;
            for (size_t i = 0; i < x.size(); ++i)
            {
                const double p = std::fmod ((double) i / sr, period);
                const double env = p < 0.003 ? p / 0.003 : std::max (std::exp (-(p - 0.003) / 0.020), 0.1);
                x[i] = (float) (x[i] * env);
            }
            return x;
        };
        {
            const double sr = 48000.0;
            const auto x = staccato (sr, 12.0);
            M m = fresh (sr), ref = fresh (sr);
            ref.burstLimit = std::numeric_limits<double>::infinity();
            drive (m, x, States (x.size(), openS), cV());
            drive (ref, x, States (x.size(), openS), cV());
            const double diff = (double) m.levelDb() - (double) ref.levelDb();
            add ("staccato", diff, std::abs (diff) <= 0.6);
        }
        {
            const double sr = 48000.0;
            const auto v0 = voice (sr, 2.0, 19, dbfs (-6.0));
            const double voiceRms = rms (v0);
            const V v = settings (1.0f, -20.49f, 1.0f, 6.0f, 9.0f, 0.0f);
            for (const double over : { 20.0, 26.0, 32.0 })
                for (const double start : { 0.02, 0.06 })
                {
                    auto x = v0;
                    const size_t a = samples (sr, start), len = samples (sr, 0.025);
                    Sig thump (len);
                    double peak = 0.0;
                    for (size_t i = 0; i < len; ++i)
                    {
                        const double w = 0.5 * (1.0 - std::cos (twoPi * (double) i / (double) (len - 1)));
                        thump[i] = (float) (std::sin (twoPi * 50.0 * (double) i / sr) * w);
                        peak = std::max (peak, std::abs ((double) thump[i]));
                    }
                    for (size_t i = 0; i < len; ++i)
                        x[a + i] += (float) (thump[i] / peak * voiceRms * dbfs (over));
                    M m = fresh (sr), ref = fresh (sr);
                    ref.startLimit = std::numeric_limits<double>::infinity();
                    const auto r = drive (m, x, States (x.size(), openS), v);
                    const auto rr = drive (ref, x, States (x.size(), openS), v);
                    const double mine = *std::min_element (r.applied.begin(), r.applied.end());
                    const double theirs = *std::min_element (rr.applied.begin(), rr.applied.end());
                    const bool asserted = over > 23.0;
                    const bool ok = ! asserted || (theirs < 0.0 ? mine >= 0.75 * theirs : mine >= theirs - 1.0e-9);
                    add ("start_pop_" + std::to_string ((int) over) + (start < 0.04 ? "_t02" : "_t06"), theirs < 0.0 ? mine / theirs : 0.0, ok);
                }

            // (b) a staccato song start, TARGET at its unclamped level: the cap boosts at most 0.5 dB more.
            const auto s = staccato (sr, 2.0);
            M probe = fresh (sr);
            probe.burstLimit = probe.startLimit = std::numeric_limits<double>::infinity();
            drive (probe, s, States (s.size(), openS), cV());
            const V vb = settings (1.0f, probe.levelDb(), 1.0f, 6.0f, 9.0f, 0.0f);
            M m = fresh (sr), ref = fresh (sr);
            ref.startLimit = std::numeric_limits<double>::infinity();
            const auto r = drive (m, s, States (s.size(), openS), vb);
            const auto rr = drive (ref, s, States (s.size(), openS), vb);
            const double extra = *std::max_element (r.applied.begin(), r.applied.end()) - *std::max_element (rr.applied.begin(), rr.applied.end());
            add ("start_pop_staccato", extra, extra <= 0.5);
        }

        // L30: hum before the first phrase (a misset gate at song start) is under the pre-level floor: no level, 0 dB.
        {
            const double sr = 48000.0;
            Sig x (samples (sr, 3.0));
            for (size_t i = 0; i < x.size(); ++i)
            {
                const double t = (double) i / sr;
                double s = 0.0;
                for (int k = 1; k <= 7; ++k)
                    s += std::sin (twoPi * 60.0 * k * t) / k;
                x[i] = (float) (s * 0.9 * dbfs (-55.0));
            }
            States st (x.size(), openS);
            const size_t voiceStart = x.size();
            append (x, st, voice (sr, 2.0, 13, dbfs (-6.0)), openS);
            M m = fresh (sr);
            const auto r = drive (m, x, st, settings (1.0f, -18.0f, 1.0f, 6.0f, 9.0f, 0.0f));
            bool held = true;
            for (size_t i = 0; i < voiceStart; ++i)
                held = held && same (r.applied[i], 0.0) && same (r.level[i], M::noLevel);
            size_t validAt = voiceStart;
            while (validAt < x.size() && same (r.level[validAt], M::noLevel))
                ++validAt;
            const size_t hop = (size_t) M::lookahead (sr);
            add ("hum_first", (double) (validAt - voiceStart), held && same (r.lamp[voiceStart / 2], 0.5f)
                                                              && validAt - voiceStart <= hop * (M::minFillHops + 1));
        }

        // L32: breathy voice (vocal-tract noise at HNR 12 / 6 / 0 dB) under Soft Take's settings: at least 95 % of hops
        // admitted at HNR 12 and 60 % at HNR 6. HNR 0 (whispered) is reported, not asserted. The spec asked 80 % at HNR 6;
        // no voicing threshold reaches it without admitting pink noise, because in each syllable's dip the voice falls
        // 10 dB under the noise (docs/SPEC_LEVEL_MATCHER.md, implementation record).
        {
            const double sr = 48000.0;
            const auto v0 = voice (sr, 10.0, 23, dbfs (-6.0));
            Rng rng;
            rng.seed (29);
            auto noise = tract (rng, v0.size(), sr);
            const double voiceE = rms (v0);
            for (const double hnr : { 12.0, 6.0, 0.0 })
            {
                auto n = noise;
                scaleTo (n, voiceE * dbfs (-hnr));
                Sig x (v0.size());
                for (size_t i = 0; i < x.size(); ++i)
                    x[i] = v0[i] + n[i];
                M m = fresh (sr);
                drive (m, x, States (x.size(), openS), settings (0.75f, -18.0f, 1.0f, 12.0f, 6.0f, 0.0f));
                const double fraction = (double) m.hopsAdmitted / (double) std::max<int64_t> (1, m.hopsAdmitted + m.hopsRejected);
                add ("breathy_hnr" + std::to_string ((int) hnr), fraction, hnr < 3.0 || fraction >= (hnr > 9.0 ? 0.95 : 0.6));
            }
        }

        // L5: the slew cap over the L4, L8, L15 and L20 runs.
        add ("slew_ratio", slewWorst, slewWorst <= 1.0 + 1.0e-9);
        return rep;
    }
}
