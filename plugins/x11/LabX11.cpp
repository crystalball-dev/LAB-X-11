// LAB X-11 // ВОКАЛ — the vocal insert of the LAB X line: a lookahead noise gate that takes the room tone out between
// phrases and keeps breaths and word tails, then a level matcher that holds each phrase at the level the song needs,
// the fader ride the reference projects do by hand. One effect for slot 1 of the vocal insert (handoff 5.1), on the
// LAB X-16 effect base: EffectInfo, the chain, the processor and the editor in one file, as every X-16 effect.
//
//   key (undelayed) -> LOW CUT -> DETECTOR -> gate state machine -> gain per sample
//   audio -> TransientAlign (5 ms) -> x gate gain -> guard GATE -> LevelMatcher -> guard LEVEL -> base (MIX, OUTPUT, BYPASS)

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
        lvlAmount
    };

    // Meters::custom slots (the chain writes them in meter(); the editor reads them at 30 Hz).
    enum Meter : int { lamp = 0, gateReductionDb, matchGainDb, keyDb, thresholdDb, matchLevelDb };

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
                // GATE // ШЛЮЗ (docs/SPEC_LOOKAHEAD_GATE_AND_ALIGNMENT.md 4.4; defaults from the reference chains, 5.1)
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
                // LEVEL // УРОВЕНЬ
                { "lvl_amount",   "Amount",      Kind::real,   0.0f,   1.0f,    1.0f,   0.0f,   Smooth::linear, "%",  "" },
            };
            e.stages = { "gate", "level" };
            e.presets = { { "Init", {} } };
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
        // RANGE through its floor glide. Only the matcher starts over.
        void flush() override { level.reset(); }

        // The lookahead is the plugin's only latency: fixed per sample rate, reported once at prepareToPlay (C2).
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

            dsp::LevelMatcher::Settings m;
            m.amount = v[lvlAmount];
            level.set (m);
        }

        void process (float* l, float* r, const float*, const float*, int n, host::Guard& guard) override
        {
            // The base hands at most one control block (16 samples); chunk anyway so the scratch arrays always fit.
            for (int pos = 0; pos < n; pos += scratchSize)
            {
                const int k = std::min (scratchSize, n - pos);
                float* bl = l + pos;
                float* br = r + pos;
                gate.computeGain (bl, br, gains.data(), voice.data(), k);   // the decision, on the undelayed audio
                align.process (bl, br, k);                                    // the audio, LOOKAHEAD samples behind it
                for (int i = 0; i < k; ++i)
                {
                    bl[i] *= gains[(size_t) i];
                    br[i] *= gains[(size_t) i];
                }
                if (guard (0, bl, br, k))
                    return;
                level.process (bl, br, voice.data(), k);                     // measures only where the gate is open
                if (guard (1, bl, br, k))
                    return;
            }
        }

        void meter (host::Meters& m) const override
        {
            m.custom[lamp].store (gate.lampValue());
            m.custom[gateReductionDb].store (gate.gainReductionDb());
            m.custom[matchGainDb].store (level.gainDb());
            m.custom[keyDb].store (gate.keyDb());
            m.custom[thresholdDb].store (thresholdNow);
        }

    private:
        static constexpr int scratchSize = host::controlBlock;
        dsp::VocalGate gate;
        dsp::TransientAlign align;
        dsp::LevelMatcher level;
        std::array<float, scratchSize> gains {}, voice {};
        float thresholdNow = -40.0f;
    };

    class X11Processor final : public host::EffectProcessor
    {
    public:
        X11Processor() : EffectProcessor (effectInfo(), std::make_unique<X11Chain>())
        {
            // Before the first block the readouts show silence and the default threshold, not zeros.
            meters.custom[keyDb].store (-120.0f);
            meters.custom[thresholdDb].store (-40.0f);
            meters.custom[gateReductionDb].store (0.0f);
        }
        juce::AudioProcessorEditor* createEditor() override;
    };

    // The gate-state lamp: lit while the gate is open or opening, half-lit while it holds or closes, dark when closed.
    class GateLamp final : public juce::Component, public ui::Themed
    {
    public:
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
            g.drawText (value >= 1.0f ? "OPEN" : value > 0.0f ? "HOLD" : "CLOSED", text, juce::Justification::centred);
        }

    private:
        float value = 0.0f;
    };

    class X11Editor final : public ui::EffectEditor
    {
    public:
        explicit X11Editor (X11Processor& p) : EffectEditor (p)
        {
            gateLamp = own (std::make_unique<GateLamp>());
            keyReadout = own (std::make_unique<ui::BigReadout>());
            reductionNeedle = own (std::make_unique<ui::Needle> ("GATE", 0.8f));
            matchNeedle = own (std::make_unique<ui::Needle> ("LEVEL", 0.9f));

            addRow ({ panel ("GATE // ШЛЮЗ", { { knob ("gate_thr", "THRESHOLD", true), 1.0f }, { knob ("gate_hyst", "HYST"), 1.0f },
                                              { knob ("gate_attack", "ATTACK"), 1.0f }, { knob ("gate_hold", "HOLD"), 1.0f },
                                              { knob ("gate_release", "RELEASE"), 1.0f }, { knob ("gate_range", "RANGE"), 1.0f } }),
                      panel ("METERS // ПРИБОРЫ", { { gateLamp, 1.0f }, { keyReadout, 1.4f }, { reductionNeedle, 1.6f },
                                                   { matchNeedle, 1.6f } }, true) });
            addRow ({ panel ("DETECTOR // ДЕТЕКТОР", { { knob ("det_lowcut", "LOW CUT"), 1.0f }, { knob ("det_release", "DET REL"), 1.0f },
                                                      { choices ({ { "det_mode", "DETECTOR" } }), 1.3f } }),
                      panel ("LEVEL // УРОВЕНЬ", { { knob ("lvl_amount", "AMOUNT", true), 1.0f } }),
                      outputPanel() });
            finish();
        }

    private:
        void refresh (float seconds) override
        {
            const auto& m = processor.meters;
            gateLamp->set (m.custom[lamp].load());
            const float key = m.custom[keyDb].load();
            keyReadout->set ("KEY / THR " + juce::String (m.custom[thresholdDb].load(), 1),
                             key <= -119.0f ? juce::String ("-INF dB") : juce::String (key, 1) + " dB",
                             key > m.custom[thresholdDb].load());
            const float range = std::max (1.0f, processor.effectRaw (Param::gateRange));
            const float reduction = std::max (0.0f, -m.custom[gateReductionDb].load());
            reductionNeedle->setReadout (reduction < 0.05f ? juce::String ("0.0 dB") : "-" + juce::String (reduction, 1) + " dB");
            reductionNeedle->advance (std::clamp (reduction / range, 0.0f, 1.0f), seconds);
            const float match = m.custom[matchGainDb].load();
            matchNeedle->setReadout (std::abs (match) < 0.05f ? juce::String ("0.0 dB") : (match > 0.0f ? "+" : "") + juce::String (match, 1) + " dB");
            matchNeedle->advance (std::clamp (0.5f + match / 24.0f, 0.0f, 1.0f), seconds);
        }

        GateLamp* gateLamp = nullptr;
        ui::BigReadout* keyReadout = nullptr;
        ui::Needle* reductionNeedle = nullptr;
        ui::Needle* matchNeedle = nullptr;
    };

    juce::AudioProcessorEditor* X11Processor::createEditor() { return new X11Editor (*this); }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new X11Processor();
}
