// LAB X-11 // ВОКАЛ — the vocal insert of the LAB X line: a lookahead noise gate that takes the room tone out between
// phrases and keeps breaths and word tails, then a level matcher that holds each phrase at the level the song needs,
// the fader ride the reference projects do by hand. One effect for slot 1 of the vocal insert (handoff 5.1), on the
// LAB X-16 effect base: EffectInfo, the chain, the processor and the editor in one file, as every X-16 effect.
//
//   key (undelayed) -> LOW CUT -> DETECTOR -> gate state machine -> gain and state per sample
//   audio -> TransientAlign (5 ms) -> x gate gain -> guard GATE -> LevelMatcher (voice = the gate's state)
//         -> guard LEVEL -> base (wet safety, MIX against the delayed dry signal, OUTPUT, BYPASS)
//
// Designs: docs/SPEC_LOOKAHEAD_GATE_AND_ALIGNMENT.md (gate, alignment) and docs/SPEC_LEVEL_MATCHER.md (matcher).

#include "core/host/EffectProcessor.h"
#include "core/dsp/Align.h"
#include "core/dsp/Gate.h"
#include "core/dsp/Leveler.h"
#include "ui/EffectEditor.h"
#include "ui/Views.h"

namespace
{
    using namespace lab;
    using host::Kind;
    using host::Smooth;

    // The effect's own parameters, after mix, output and bypass. Append-only once installed (FL saves indices).
    enum Param : int
    {
        gateThr = 0, gateHyst, gateAttack, gateHold, gateRelease, gateRange, detLowCut, detMode, detRelease,
        levelFirst   // level_amount .. level_law: LevelMatcher::Param offsets from here
    };

    // Meters::custom slots (the chain writes them in meter(); the editor reads them at 30 Hz).
    enum Meter : int { gateLampSlot = 0, gateReductionDb, matchGainDb, levelLufs, voiceLampSlot, keyDb, thresholdDb };

    const host::EffectInfo& effectInfo()
    {
        static const host::EffectInfo i = []
        {
            host::EffectInfo e;
            e.product = LABX11_PRODUCT;
            e.subtitle = "ВОКАЛ";
            e.slug = LABX11_SLUG;
            e.version = LABX11_VERSION;
            e.code = LABX11_CODE;
            e.faction = host::Faction::deepZone;
            e.params = {
                // GATE // ШЛЮЗ (docs/SPEC_LOOKAHEAD_GATE_AND_ALIGNMENT.md 4.4; defaults from the reference chains, handoff 5.1)
                { "gate_thr",     "Threshold",   Kind::real,   -80.0f, 0.0f,    -40.0f, 0.0f,   Smooth::linear, "dB", "" },
                { "gate_hyst",    "Hysteresis",  Kind::real,   0.0f,   12.0f,   4.0f,   0.0f,   Smooth::linear, "dB", "" },
                { "gate_attack",  "Attack",      Kind::real,   0.1f,   50.0f,   1.0f,   5.0f,   Smooth::log,    "ms", "" },
                { "gate_hold",    "Hold",        Kind::real,   0.0f,   500.0f,  40.0f,  50.0f,  Smooth::log,    "ms", "" },
                { "gate_release", "Release",     Kind::real,   5.0f,   2000.0f, 120.0f, 200.0f, Smooth::log,    "ms", "" },
                { "gate_range",   "Range",       Kind::real,   0.0f,   80.0f,   24.0f,  24.0f,  Smooth::linear, "dB", "" },
                // DETECTOR // ДЕТЕКТОР
                { "det_lowcut",   "Low Cut",     Kind::real,   20.0f,  500.0f,  80.0f,  100.0f, Smooth::log,    "Hz", "" },
                { "det_mode",     "Detector",    Kind::choice, 0.0f,   1.0f,    0.0f,   0.0f,   Smooth::none,   "",   "Peak|RMS" },
                { "det_release",  "Det Release", Kind::real,   1.0f,   200.0f,  20.0f,  20.0f,  Smooth::log,    "ms", "" },
                // LEVEL // УРОВЕНЬ (docs/SPEC_LEVEL_MATCHER.md L.9; LevelMatcher::defaults must match, checked by L31)
                { "level_amount", "Amount",      Kind::real,   0.0f,   1.0f,    0.75f,  0.0f,   Smooth::linear, "%",  "" },
                { "level_target", "Target",      Kind::real,   -40.0f, -6.0f,   -18.0f, 0.0f,   Smooth::linear, "dB", "" },
                { "level_time",   "Ride Time",   Kind::real,   0.5f,   10.0f,   1.0f,   1.0f,   Smooth::log,    "s",  "" },
                { "level_boost",  "Max Boost",   Kind::real,   0.0f,   18.0f,   6.0f,   0.0f,   Smooth::linear, "dB", "" },
                { "level_cut",    "Max Cut",     Kind::real,   0.0f,   18.0f,   9.0f,   0.0f,   Smooth::linear, "dB", "" },
                { "level_law",    "Law",         Kind::choice, 0.0f,   1.0f,    0.0f,   0.0f,   Smooth::none,   "",   "Ride|Step" },
            };
            e.stages = { "gate", "level" };
            // Handoff 5.4. Every preset writes TARGET -18 (PROVISIONAL, from the stem measurements later) and LAW Ride
            // (D7). Gate rows are PROVISIONAL too. OUTPUT trims keep the reference vocal's RMS within 1 dB (test P1);
            // Gate Only and Level Only stay at 0 dB (the X-16 rule for utility presets).
            e.presets = {
                { "Init", {} },
                { "Booth Floor", { { "gate_thr", -42.0f }, { "gate_hold", 80.0f }, { "gate_release", 150.0f },
                                   { "level_amount", 0.75f }, { "level_time", 2.0f }, { "level_boost", 6.0f }, { "level_cut", 9.0f } } },
                { "Room Tone", { { "gate_thr", -34.0f }, { "gate_hyst", 5.0f }, { "gate_hold", 60.0f }, { "gate_release", 250.0f },
                                 { "gate_range", 36.0f }, { "det_lowcut", 120.0f },
                                 { "level_amount", 0.6f }, { "level_time", 1.5f }, { "level_boost", 3.0f }, { "level_cut", 9.0f } } },
                { "Rap Lead", { { "gate_thr", -38.0f }, { "gate_attack", 0.5f }, { "gate_hold", 20.0f }, { "gate_release", 80.0f },
                                { "level_amount", 1.0f }, { "level_time", 0.5f }, { "level_boost", 4.0f }, { "level_cut", 6.0f } } },
                { "Soft Take", { { "gate_thr", -48.0f }, { "gate_hold", 60.0f }, { "gate_release", 160.0f }, { "gate_range", 12.0f },
                                 { "level_amount", 0.75f }, { "level_time", 1.0f }, { "level_boost", 12.0f }, { "level_cut", 6.0f } } },
                { "Ad-lib / Double", { { "gate_thr", -32.0f }, { "gate_hold", 30.0f }, { "gate_release", 100.0f }, { "gate_range", 40.0f },
                                       { "level_amount", 1.0f }, { "level_time", 0.5f }, { "level_boost", 6.0f }, { "level_cut", 9.0f } } },
                { "Gate Only", { { "level_amount", 0.0f } } },
                { "Level Only", { { "gate_range", 0.0f } } },
            };
            return e;
        }();
        return i;
    }

    class X11Chain final : public host::Chain
    {
    public:
        void prepare (double sampleRate, int) override
        {
            gate.prepare (sampleRate);
            align.prepare (sampleRate);
            level.prepare (sampleRate);
            jassert (dsp::VocalGate::lookaheadSamples (sampleRate) == align.samples());
            jassert (dsp::LevelMatcher::lookahead (sampleRate) == align.samples());
        }

        // Faults and prepareToPlay: everything to zero.
        void reset() override
        {
            gate.reset();
            align.reset();
            level.reset();
        }

        // A preset change: the base fades the effect out over 30 ms, calls flush() and fades back in over 5 ms, which
        // is exactly the lookahead. Emptying the alignment ring here would make the wet path silent for the whole
        // fade-in and then step back in at full level, so the ring (a pure delay of the sanitised input, like the base's
        // own dry line) and the gate's signal-derived state are kept; the new settings reach the gate through set(),
        // RANGE through its floor glide. The matcher keeps its measurement too and snaps to the new amount and limits
        // on the fade-in's first sample, so auditioning presets mid-song does not relearn from 0 dB.
        void flush() override { level.flush(); }

        // The lookahead is the plugin's only latency: fixed per sample rate, reported once at prepareToPlay (C2). The
        // matcher adds none.
        int latency (double sampleRate) const override { return dsp::TransientAlign::latency (sampleRate); }

        // Nothing rings on after the input stops: the gate only attenuates and the matcher holds its gain in gaps.
        double tailSeconds (const float*) const override { return dsp::TransientAlign::lookaheadSeconds; }

        void control (const float* v, const host::BlockContext&, int) override
        {
            dsp::VocalGate::Settings g;
            g.thresholdDb = v[gateThr];
            g.hysteresisDb = v[gateHyst];
            g.attackMs = v[gateAttack];
            g.holdMs = v[gateHold];
            g.releaseMs = v[gateRelease];
            g.rangeDb = v[gateRange];
            g.lowCutHz = v[detLowCut];
            g.mode = v[detMode] > 0.5f ? dsp::VocalGate::Mode::rms : dsp::VocalGate::Mode::peak;
            g.detReleaseMs = v[detRelease];
            gate.set (g);
            thresholdNow = g.thresholdDb;
            level.control (v + levelFirst);
        }

        void process (float* l, float* r, const float*, const float*, int n, host::Guard& guard) override
        {
            // The base hands at most one control block (16 samples); chunk anyway so the scratch arrays always fit.
            for (int pos = 0; pos < n; pos += scratchSize)
            {
                const int k = std::min (scratchSize, n - pos);
                float* bl = l + pos;
                float* br = r + pos;
                gate.computeGain (bl, br, gains.data(), states.data(), k);   // the decision, on the undelayed audio
                align.process (bl, br, k);                                     // the audio, LOOKAHEAD samples behind it
                for (int i = 0; i < k; ++i)
                {
                    bl[i] *= gains[(size_t) i];
                    br[i] *= gains[(size_t) i];
                }
                if (guard (0, bl, br, k))
                    return;
                level.process (bl, br, states.data(), k);                     // measures only where the gate is open
                if (guard (1, bl, br, k))
                    return;
            }
        }

        void meter (host::Meters& m) const override
        {
            m.custom[gateLampSlot].store (gate.lampValue());
            m.custom[gateReductionDb].store (gate.gainReductionDb());
            m.custom[matchGainDb].store (level.gainDb());
            m.custom[levelLufs].store (level.levelDb());
            m.custom[voiceLampSlot].store (level.voiceLamp());
            m.custom[keyDb].store (gate.keyDb());
            m.custom[thresholdDb].store (thresholdNow);
        }

    private:
        static constexpr int scratchSize = host::controlBlock;
        dsp::VocalGate gate;
        dsp::TransientAlign align;
        dsp::LevelMatcher level;
        std::array<float, scratchSize> gains {};
        std::array<uint8_t, scratchSize> states {};
        float thresholdNow = -40.0f;
    };

    class X11Processor final : public host::EffectProcessor
    {
    public:
        X11Processor() : EffectProcessor (effectInfo(), std::make_unique<X11Chain>())
        {
            // Before the first block the readouts show silence, no level and the default threshold, not zeros.
            meters.custom[keyDb].store (-120.0f);
            meters.custom[thresholdDb].store (-40.0f);
            meters.custom[gateReductionDb].store (0.0f);
            meters.custom[levelLufs].store (dsp::LevelMatcher::noLevel);
        }
        juce::AudioProcessorEditor* createEditor() override;
    };

    // A round lamp with three states (1 lit, 0.5 half-lit, 0 dark) and a word for each.
    class Lamp final : public juce::Component, public ui::Themed
    {
    public:
        Lamp (const char* lit, const char* half, const char* dark) : words { lit, half, dark } {}

        void set (float lampValue)
        {
            if (std::abs (lampValue - value) < 1.0e-3f)
                return;
            value = lampValue;
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            const auto& t = *theme;
            auto b = getLocalBounds().toFloat().reduced (4.0f);
            const auto text = b.removeFromBottom (14.0f);
            const float d = std::min (b.getWidth(), b.getHeight()) * 0.62f;
            const auto lens = b.withSizeKeepingCentre (d, d);
            g.setColour (t.faint);
            g.fillEllipse (lens.expanded (3.0f));
            if (value > 0.0f)
            {
                g.setColour (t.phosphor.withAlpha (0.18f * value));
                g.fillEllipse (lens.expanded (7.0f * value));
            }
            g.setColour (value >= 1.0f ? t.phosphor : value > 0.0f ? t.dim : t.background);
            g.fillEllipse (lens);
            g.setColour (t.border);
            g.drawEllipse (lens, 1.2f);
            g.setFont (ui::monoFont (10.0f, true));
            g.setColour (t.phosphor);
            g.drawText (value >= 1.0f ? words[0] : value > 0.0f ? words[1] : words[2], text, juce::Justification::centred);
        }

    private:
        std::array<const char*, 3> words;
        float value = 0.0f;
    };

    class X11Editor final : public ui::EffectEditor
    {
    public:
        explicit X11Editor (X11Processor& p) : EffectEditor (p)
        {
            gateLamp = own (std::make_unique<Lamp> ("OPEN", "HOLD", "CLOSED"));
            voiceLamp = own (std::make_unique<Lamp> ("VOICE", "HELD", "GAP"));
            keyReadout = own (std::make_unique<ui::BigReadout>());
            levelReadout = own (std::make_unique<ui::BigReadout>());
            reductionNeedle = own (std::make_unique<ui::Needle> ("GATE", 0.8f));
            matchNeedle = own (std::make_unique<ui::Needle> ("MATCH", 0.9f));

            addRow ({ panel ("GATE // ШЛЮЗ", { { knob ("gate_thr", "THRESHOLD", true), 1.0f }, { knob ("gate_hyst", "HYST"), 1.0f },
                                              { knob ("gate_attack", "ATTACK"), 1.0f }, { knob ("gate_hold", "HOLD"), 1.0f },
                                              { knob ("gate_release", "RELEASE"), 1.0f }, { knob ("gate_range", "RANGE"), 1.0f } }),
                      panel ("METERS // ПРИБОРЫ", { { gateLamp, 0.8f }, { keyReadout, 1.25f }, { reductionNeedle, 1.35f },
                                                   { voiceLamp, 0.8f }, { levelReadout, 1.25f }, { matchNeedle, 1.35f } }, true) });
            addRow ({ panel ("DETECTOR // ДЕТЕКТОР", { { knob ("det_lowcut", "LOW CUT"), 1.0f }, { knob ("det_release", "DET REL"), 1.0f },
                                                      { choices ({ { "det_mode", "DETECTOR" } }), 1.2f } }),
                      panel ("LEVEL // УРОВЕНЬ", { { knob ("level_amount", "AMOUNT", true), 1.0f }, { knob ("level_target", "TARGET"), 1.0f },
                                                  { knob ("level_time", "RIDE"), 1.0f }, { knob ("level_boost", "BOOST"), 1.0f },
                                                  { knob ("level_cut", "CUT"), 1.0f }, { choices ({ { "level_law", "LAW" } }), 1.2f } }),
                      outputPanel() });
            finish();
        }

    private:
        void refresh (float seconds) override
        {
            const auto& m = processor.meters;
            gateLamp->set (m.custom[gateLampSlot].load());
            voiceLamp->set (m.custom[voiceLampSlot].load());

            const float key = m.custom[keyDb].load(), thr = m.custom[thresholdDb].load();
            keyReadout->set ("KEY  THR " + juce::String (thr, 1), key <= -119.0f ? juce::String ("-INF dB") : juce::String (key, 1) + " dB",
                             key > thr);
            // LEVEL is the matcher's own measure (voiced K-weighted loudness, LUFS): set TARGET from it.
            const float lvl = m.custom[levelLufs].load();
            levelReadout->set ("LUFS  TGT " + juce::String (processor.effectRaw ((int) levelFirst + (int) dsp::LevelMatcher::target), 1),
                               lvl <= dsp::LevelMatcher::noLevel + 0.5f ? juce::String ("--.-") : juce::String (lvl, 1));

            const float range = std::max (1.0f, processor.effectRaw (Param::gateRange));
            const float reduction = std::max (0.0f, -m.custom[gateReductionDb].load());
            reductionNeedle->setReadout (reduction < 0.05f ? juce::String ("0.0 dB") : "-" + juce::String (reduction, 1) + " dB");
            reductionNeedle->advance (std::clamp (reduction / range, 0.0f, 1.0f), seconds);
            const float match = m.custom[matchGainDb].load();
            matchNeedle->setReadout (std::abs (match) < 0.05f ? juce::String ("0.0 dB") : (match > 0.0f ? "+" : "") + juce::String (match, 1) + " dB");
            matchNeedle->advance (std::clamp (0.5f + match / 24.0f, 0.0f, 1.0f), seconds);
        }

        Lamp* gateLamp = nullptr;
        Lamp* voiceLamp = nullptr;
        ui::BigReadout* keyReadout = nullptr;
        ui::BigReadout* levelReadout = nullptr;
        ui::Needle* reductionNeedle = nullptr;
        ui::Needle* matchNeedle = nullptr;
    };

    juce::AudioProcessorEditor* X11Processor::createEditor() { return new X11Editor (*this); }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new X11Processor();
}
