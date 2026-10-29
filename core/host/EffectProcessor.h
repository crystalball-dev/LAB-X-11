#pragma once

#include <array>
#include <atomic>
#include <limits>
#include <memory>
#include <vector>
#include <juce_audio_processors/juce_audio_processors.h>
#include "../dsp/Common.h"
#include "../dsp/DelayLine.h"
#include "../dsp/Transport.h"
#include "Params.h"
#include "ZoneBus.h"

#ifndef LABX11_HARNESS
 #define LABX11_HARNESS 0
#endif

// lab::host: the EffectProcessor base, ported from LAB X-16 (where every effect is an EffectProcessor plus a Chain).
// LAB X-11 is one effect on this base: the X11Chain in plugins/x11/LabX11.cpp. The base owns what
// the siblings re-implemented per plugin: the parameter table and smoothers, the 16-sample control loop, the dry path
// delayed by the latency, mix, output, bypass, the preset fade, the runaway guard and its fault log, meters, the
// scope, state (with an optional binary blob) and the Zone Bus client.
namespace lab::host
{
    inline constexpr const char* vendorName = "OPERATION FAIRWAY, LLC";
    inline constexpr int controlBlock = 16;

    enum class Faction { deepZone, etherealSignal };
    enum class MixLaw { linear, equalPower };   // equal power: the Noosphere law, for reverbs and delays

    struct Preset
    {
        const char* name;
        std::vector<std::pair<const char*, float>> values;   // ids not listed take their defaults
    };

    // The static description of one effect.
    struct EffectInfo
    {
        const char* product = "";     // "KAYMANOV"
        const char* subtitle = "";    // UTF-8, e.g. "ИЗЛУЧАТЕЛЬ"
        const char* slug = "";
        const char* version = "0.1.0";
        const char* code = "L6xx";
        Faction faction = Faction::deepZone;
        bool sidechain = false, midi = false, busClient = false;
        MixLaw mixLaw = MixLaw::linear;
        std::vector<ParamSpec> params;     // the effect's own, after the common ones
        std::vector<const char*> stages;   // guard stage names, in chain order
        std::vector<Preset> presets;       // preset 0 is "Init"
    };

    // Editor-facing readouts, written on the audio thread.
    struct Meters
    {
        std::atomic<float> inDb { -100.0f }, outDb { -100.0f }, inPeakDb { -100.0f }, outPeakDb { -100.0f };
        std::atomic<float> sideDb { -100.0f }, bpm { 120.0f };
        std::atomic<int> latency { 0 }, faults { 0 }, lastFaultStage { -1 }, peers { 0 }, midiEvents { 0 };
        std::atomic<bool> playing { false }, sidechainActive { false }, busConnected { false };
        std::array<std::atomic<float>, 8> custom {};   // the chain's own readouts (HEAT, dose, ...)
    };

    class EffectProcessor;

    // Passed to Chain::process. Call it after every stage the effect names; it returns true when the block is a fault
    // (the base silences it and resets the chain).
    class Guard
    {
    public:
        bool operator() (int stage, float* l, float* r, int n) noexcept;
        bool tripped = false;

    private:
        friend class EffectProcessor;
        EffectProcessor* owner = nullptr;
    };

    // What a chain sees of the world for one control block.
    struct BlockContext
    {
        const dsp::Transport* transport = nullptr;
        uint32_t callsign = 0;          // the instance's id: the salt of every hash01 decision
        bus::Client* bus = nullptr;     // null unless a bus client with BUS On and connected
        bool sidechainActive = false;   // the host enabled the sidechain bus
        int64_t sampleClock = 0;        // samples processed since prepareToPlay
        // Set true to change something that must not be heard switching (oversampling, a mode with its own
        // state): the base fades the effect to the dry signal over 30 ms, calls flush(), and fades back in.
        bool* requestSwitch = nullptr;
        // A macro that also raises MIX (WISH GRANTER's GRANT) writes how much here; the base adds it to the mix.
        float* mixOffset = nullptr;
    };

    // Implemented per effect. `v` points at the effect's first own parameter (smoothed values for this block).
    class Chain
    {
    public:
        virtual ~Chain() = default;
        virtual void prepare (double sampleRate, int maxBlock) = 0;
        virtual void reset() = 0;                                // state to zero, settings kept
        virtual void flush() { reset(); }                        // preset change: zero and snap
        virtual int latency (double) const { return 0; }         // fixed per sample rate, read at prepareToPlay only
        virtual double tailSeconds (const float*) const { return 0.0; }
        virtual void control (const float* v, const BlockContext&, int n) { juce::ignoreUnused (v, n); }
        virtual void midi (const juce::MidiMessage&, const BlockContext&) {}
        // io: the wet path, in place; side: the sidechain block (zeros when inactive)
        virtual void process (float* l, float* r, const float* sideL, const float* sideR, int n, Guard& guard) = 0;
        virtual void meter (Meters&) const {}
        virtual int blobVersion() const { return 0; }            // 0: no blob
        virtual void saveBlob (juce::MemoryOutputStream&) const {}
        virtual bool loadBlob (juce::MemoryInputStream&, int) { return false; }
        // A factory preset was written (message thread): state that is not a parameter can follow it (NUMBERS STATION
        // drops a typed key, so a preset's key is its KEY number alone). Use atomics for anything the audio thread reads.
        virtual void presetLoaded (int) {}
        // A text form of the chain's own state (ANOMALY's patterns, NUMBERS STATION's key): the harness's --text, tests.
        virtual bool loadText (const juce::String&) { return false; }
    };

    class EffectProcessor : public juce::AudioProcessor, private juce::Timer
    {
    private:
        // Declared first: the parameter layout below points into these specs, so they must exist before it.
        std::vector<ParamSpec> specs;
        int commonCount = 3;

    public:
        EffectProcessor (const EffectInfo& effectInfo, std::unique_ptr<Chain> effectChain)
            : AudioProcessor (makeBuses (effectInfo.sidechain)),
              specs (makeSpecs (effectInfo)),
              commonCount ((int) commonParams (effectInfo.busClient).size()),
              info (effectInfo),
              apvts (*this, nullptr, "LABX11", createLayout (specs)),
              chain (std::move (effectChain))
        {
            const int count = (int) specs.size();
            raw.resize ((size_t) count);
            smoothers.resize ((size_t) count);
            values.assign ((size_t) count, 0.0f);
            for (int i = 0; i < count; ++i)
            {
                raw[(size_t) i] = apvts.getRawParameterValue (specs[(size_t) i].id);
                jassert (raw[(size_t) i] != nullptr);
                smoothers[(size_t) i].enabled = specs[(size_t) i].smooth != Smooth::none;
                smoothers[(size_t) i].logDomain = specs[(size_t) i].smooth == Smooth::log;
                values[(size_t) i] = rawValue (i);
            }
            instanceId = (uint32_t) juce::Random::getSystemRandom().nextInt() | 1u;
            callsign = bus::defaultCallsign (instanceId);
            if (! info.presets.empty())
                applyPreset (0, false);
            startTimer (500);
        }

        ~EffectProcessor() override
        {
            stopTimer();
            flushFaultLog();
        }

        //==============================================================================
        const EffectInfo& info;

        const std::vector<ParamSpec>& getSpecs() const noexcept { return specs; }
        int getCommonCount() const noexcept { return commonCount; }
        int effectIndex (int i) const noexcept { return commonCount + i; }
        float rawValue (int index) const noexcept { return raw[(size_t) index]->load (std::memory_order_relaxed); }
        float effectRaw (int i) const noexcept { return rawValue (commonCount + i); }
        Chain& getChain() noexcept { return *chain; }

        uint32_t getInstanceId() const noexcept { return instanceId; }
        juce::String getCallsign() const { return callsign; }
        void setCallsign (const juce::String& text)   // message thread
        {
            callsign = bus::unpackCallsign (bus::packCallsign (text.isNotEmpty() ? text : bus::defaultCallsign (instanceId)));
            busClient.setIdentity (instanceId, callsign);
        }
        const bus::Client& getBusClient() const noexcept { return busClient; }

        Meters meters;
        static constexpr int scopeSize = 2048;
        void copyScope (float* dest, int numSamples) const noexcept
        {
            const int end = scopeWrite.load (std::memory_order_acquire);
            for (int i = 0; i < numSamples; ++i)
                dest[i] = scope[(size_t) ((end - numSamples + i) & (scopeSize - 1))];
        }

        juce::AudioProcessorValueTreeState apvts;

        //==============================================================================
        void prepareToPlay (double sampleRate, int samplesPerBlock) override
        {
            preparedBlockSize = samplesPerBlock;
            currentSampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
            const auto sr = (float) currentSampleRate;
            for (int n = 0; n <= controlBlock; ++n)
                smoothCoef[(size_t) n] = 1.0f - std::exp (-(float) std::max (n, 1) / (0.015f * sr));
            for (size_t i = 0; i < specs.size(); ++i)
            {
                smoothers[i].reset (rawValue ((int) i));
                values[i] = rawValue ((int) i);
            }

            chain->prepare (currentSampleRate, controlBlock);
            chain->reset();
            latencySamples = std::max (0, chain->latency (currentSampleRate));
            setLatencySamples (latencySamples);
            meters.latency.store (latencySamples);
            dryLine.prepare (std::max (1, latencySamples));

            bypassMix = rawValue (Common::bypass) > 0.5f ? 1.0f : 0.0f;
            bypassStep = 1.0f / (0.02f * sr);
            mixNow = values[Common::mix];
            outputGainNow = dsp::dbToGain (values[Common::output]);

            programChangePending.store (false);
            switchState = SwitchState::idle;
            switchGain = 1.0f;
            switchFadeLength = std::max (1, (int) (0.03 * currentSampleRate));
            switchFadeInStep = 1.0f / (0.005f * sr);

            transport = {};
            transport.sampleRate = currentSampleRate;
            sampleClock = 0;
            meterSamples = 0;

            if (info.busClient)
                meters.busConnected.store (busClient.connect (instanceId, info.code, callsign));
        }

        void releaseResources() override {}

        bool isBusesLayoutSupported (const BusesLayout& layouts) const override
        {
            const auto in = layouts.getMainInputChannelSet();
            const auto out = layouts.getMainOutputChannelSet();
            if (out != juce::AudioChannelSet::stereo() && out != juce::AudioChannelSet::mono())
                return false;
            if (in != out || layouts.outputBuses.size() != 1)
                return false;
            if (layouts.inputBuses.size() > (info.sidechain ? 2 : 1))
                return false;
            if (layouts.inputBuses.size() == 2)
            {
                const auto sc = layouts.inputBuses[1];
                if (! sc.isDisabled() && sc != juce::AudioChannelSet::stereo() && sc != juce::AudioChannelSet::mono())
                    return false;
            }
            return true;
        }

        void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
        {
            juce::ScopedNoDenormals noDenormals;
            const int numSamples = buffer.getNumSamples();
            auto mainIn = getBusBuffer (buffer, true, 0);
            auto mainOut = getBusBuffer (buffer, false, 0);
            const int inChannels = mainIn.getNumChannels();
            const int outChannels = mainOut.getNumChannels();

            const float* sideChannels[2] = { nullptr, nullptr };
            bool sideActive = false;
            if (info.sidechain && getBusCount (true) > 1)
                if (auto* sideBus = getBus (true, 1); sideBus != nullptr && sideBus->isEnabled() && sideBus->getNumberOfChannels() > 0)
                {
                    auto side = getBusBuffer (buffer, true, 1);
                    if (side.getNumChannels() > 0)
                    {
                        sideChannels[0] = side.getReadPointer (0);
                        sideChannels[1] = side.getReadPointer (std::min (1, side.getNumChannels() - 1));
                        sideActive = true;
                    }
                }

            if (programChangePending.exchange (false) && switchState != SwitchState::fadingOut)
            {
                switchState = SwitchState::fadingOut;
                switchFadeRemaining = (int) std::ceil (switchGain * (float) switchFadeLength);
            }

            readTransport();
            const bool busOn = info.busClient && busClient.isConnected() && rawValue (Common::bus) > 0.5f;

            auto midiIt = midi.cbegin();
            const auto midiEnd = midi.cend();
            double inSquares = 0.0, outSquares = 0.0, sideSquares = 0.0;
            float inPeak = 0.0f, outPeak = 0.0f;
            int scopePos = scopeWrite.load (std::memory_order_relaxed);
            int midiCount = 0;

            int pos = 0;
            while (pos < numSamples)
            {
                BlockContext ctx;
                ctx.transport = &transport;
                ctx.callsign = instanceId;
                ctx.bus = busOn ? &busClient : nullptr;
                ctx.sidechainActive = sideActive;
                ctx.sampleClock = sampleClock;
                bool switchWanted = false;
                ctx.requestSwitch = &switchWanted;
                float mixOffset = 0.0f;
                ctx.mixOffset = &mixOffset;

                while (midiIt != midiEnd && (*midiIt).samplePosition <= pos)
                {
                    if (info.midi)
                        chain->midi ((*midiIt).getMessage(), ctx);
                    ++midiCount;
                    ++midiIt;
                }

                int next = numSamples;
                if (midiIt != midiEnd)
                    next = std::min (next, (*midiIt).samplePosition);
                const int n = std::min ({ controlBlock, std::max (1, next - pos), numSamples - pos });

                transport.ppq = transport.playing ? blockPpq + (double) pos * transport.ppqPerSample : 0.0;
                updateControl (n);
                chain->control (values.data() + commonCount, ctx, n);
                if (switchWanted && switchState != SwitchState::fadingOut)
                {
                    switchState = SwitchState::fadingOut;
                    switchFadeRemaining = (int) std::ceil (switchGain * (float) switchFadeLength);
                }

                // Input (non-finite host samples become silence: the input itself is never faulted).
                std::array<float, controlBlock> dryL {}, dryR {}, wetL {}, wetR {}, sideL {}, sideR {};
                for (int i = 0; i < n; ++i)
                {
                    float l = inChannels > 0 ? mainIn.getSample (0, pos + i) : 0.0f;
                    float r = inChannels > 1 ? mainIn.getSample (1, pos + i) : l;
                    if (! std::isfinite (l)) l = 0.0f;
                    if (! std::isfinite (r)) r = 0.0f;
                    dryL[(size_t) i] = wetL[(size_t) i] = l;
                    dryR[(size_t) i] = wetR[(size_t) i] = r;
                    inSquares += (double) (l * l + r * r) * 0.5;
                    inPeak = std::max (inPeak, std::max (std::abs (l), std::abs (r)));
                    if (sideActive)
                    {
                        sideL[(size_t) i] = sideChannels[0][pos + i];
                        sideR[(size_t) i] = sideChannels[1][pos + i];
                        sideSquares += (double) (sideL[(size_t) i] * sideL[(size_t) i] + sideR[(size_t) i] * sideR[(size_t) i]) * 0.5;
                    }
                }

                Guard guard;
                guard.owner = this;
                chain->process (wetL.data(), wetR.data(), sideL.data(), sideR.data(), n, guard);
                if (guard.tripped)
                {
                    wetL.fill (0.0f);
                    wetR.fill (0.0f);
                    chain->reset();
                }
               #if LABX11_HARNESS
                // The OUTPUT stage (after the chain's own): a non-finite sample, which the output check must catch.
                if (injectStage == (int) info.stages.size() && injectAtSample >= sampleClock && injectAtSample < sampleClock + n)
                    wetL[0] = std::numeric_limits<float>::quiet_NaN();
               #endif

                const float mixTarget = std::clamp (values[Common::mix] + mixOffset, 0.0f, 1.0f);
                const float gainTarget = dsp::dbToGain (values[Common::output]);
                const float bypassTarget = rawValue (Common::bypass) > 0.5f ? 1.0f : 0.0f;
                const float mixStep = (mixTarget - mixNow) / (float) n;
                const float gainStep = (gainTarget - outputGainNow) / (float) n;
                bool nonFinite = false;

                for (int i = 0; i < n; ++i)
                {
                    // The dry signal, delayed by the reported latency so it lines up with the wet path.
                    float dl = dryL[(size_t) i], dr = dryR[(size_t) i];
                    if (latencySamples > 0)
                    {
                        dryLine.push (dl, dr);
                        dl = dryLine.readInt (0, latencySamples);
                        dr = dryLine.readInt (1, latencySamples);
                    }

                    float wl = dsp::wetSafety (wetL[(size_t) i]);
                    float wr = dsp::wetSafety (wetR[(size_t) i]);
                    if (! std::isfinite (wl) || ! std::isfinite (wr))
                    {
                        nonFinite = true;
                        wl = wr = 0.0f;
                    }

                    mixNow += mixStep;
                    outputGainNow += gainStep;
                    float dryGain, wetGain;
                    mixGains (mixNow, dryGain, wetGain);
                    float l = (dl * dryGain + wl * wetGain) * outputGainNow;
                    float r = (dr * dryGain + wr * wetGain) * outputGainNow;

                    // Bypass: a 20 ms crossfade to the aligned dry signal; fully bypassed means exactly the dry signal.
                    if (bypassMix != bypassTarget)
                        bypassMix = bypassTarget > bypassMix ? std::min (bypassTarget, bypassMix + bypassStep)
                                                             : std::max (bypassTarget, bypassMix - bypassStep);
                    if (bypassMix >= 1.0f)
                    {
                        l = dl;
                        r = dr;
                    }
                    else if (bypassMix > 0.0f)
                    {
                        l += (dl - l) * bypassMix;
                        r += (dr - r) * bypassMix;
                    }

                    // Preset change: the effect fades to the dry signal and back; the track itself never drops out.
                    if (switchState == SwitchState::fadingOut)
                    {
                        switchGain = (float) switchFadeRemaining / (float) switchFadeLength;
                        if (switchFadeRemaining > 0)
                            --switchFadeRemaining;
                    }
                    else if (switchState == SwitchState::fadingIn)
                    {
                        switchGain = std::min (1.0f, switchGain + switchFadeInStep);
                        if (switchGain >= 1.0f)
                            switchState = SwitchState::idle;
                    }
                    if (switchGain < 1.0f)
                    {
                        l = dl + (l - dl) * switchGain;
                        r = dr + (r - dr) * switchGain;
                    }

                    if (outChannels >= 2)
                    {
                        mainOut.setSample (0, pos + i, l);
                        mainOut.setSample (1, pos + i, r);
                    }
                    else if (outChannels == 1)
                        mainOut.setSample (0, pos + i, 0.5f * (l + r));

                    outSquares += (double) (l * l + r * r) * 0.5;
                    outPeak = std::max (outPeak, std::max (std::abs (l), std::abs (r)));
                    scope[(size_t) scopePos] = 0.5f * (l + r);
                    scopePos = (scopePos + 1) & (scopeSize - 1);
                }

                if (nonFinite)
                {
                    noteFault ((int) info.stages.size(), std::numeric_limits<float>::quiet_NaN(), true);
                    chain->reset();
                }

                sampleClock += n;
               #if LABX11_HARNESS
                samplesRendered += n;
               #endif

                if (switchState == SwitchState::fadingOut && switchFadeRemaining == 0)
                {
                    chain->flush();
                    for (size_t k = 0; k < specs.size(); ++k)
                        smoothers[k].reset (rawValue ((int) k));
                    switchState = SwitchState::fadingIn;
                    switchGain = 0.0f;
                }
                pos += n;
            }
            for (; midiIt != midiEnd; ++midiIt)
            {
                BlockContext ctx;
                ctx.transport = &transport;
                ctx.callsign = instanceId;
                if (info.midi)
                    chain->midi ((*midiIt).getMessage(), ctx);
                ++midiCount;
            }
            scopeWrite.store (scopePos, std::memory_order_release);

            // Output channels beyond the main pair (none in the suite) stay silent.
            for (int ch = outChannels; ch < getTotalNumOutputChannels() && ch < buffer.getNumChannels(); ++ch)
                buffer.clear (ch, 0, numSamples);

            if (info.busClient)
                busClient.heartbeat();
            if (midiCount > 0)
                meters.midiEvents.fetch_add (midiCount);
            if (numSamples > 0)
            {
                const auto inv = 1.0 / (double) numSamples;
                meters.inDb.store (juce::Decibels::gainToDecibels ((float) std::sqrt (inSquares * inv), -100.0f));
                meters.outDb.store (juce::Decibels::gainToDecibels ((float) std::sqrt (outSquares * inv), -100.0f));
                meters.inPeakDb.store (juce::Decibels::gainToDecibels (inPeak, -100.0f));
                meters.outPeakDb.store (juce::Decibels::gainToDecibels (outPeak, -100.0f));
                meters.sideDb.store (sideActive ? juce::Decibels::gainToDecibels ((float) std::sqrt (sideSquares * inv), -100.0f) : -100.0f);
            }
            meters.sidechainActive.store (sideActive);
            meters.bpm.store ((float) transport.bpm);
            meters.playing.store (transport.playing);
            chain->meter (meters);
            meterSamples += numSamples;
            if (meterSamples >= (int) (currentSampleRate * 0.25))
            {
                meterSamples = 0;
                if (info.busClient)
                    meters.peers.store (busClient.peers (nullptr, 0));
            }
        }

        using AudioProcessor::processBlock;

        //==============================================================================
        // LAB X-11 is a single plugin, so its product name is the plugin name ("LAB X-11"); X-16 prefixed "LAB X-16 ".
        const juce::String getName() const override { return juce::String::fromUTF8 (info.product); }
        bool acceptsMidi() const override { return info.midi; }
        bool producesMidi() const override { return false; }
        bool isMidiEffect() const override { return false; }
        double getTailLengthSeconds() const override
        {
            std::vector<float> v ((size_t) std::max (1, (int) specs.size() - commonCount));
            for (int i = commonCount; i < (int) specs.size(); ++i)
                v[(size_t) (i - commonCount)] = rawValue (i);
            return chain->tailSeconds (v.data());
        }
        juce::AudioProcessorParameter* getBypassParameter() const override { return apvts.getParameter ("bypass"); }
        bool hasEditor() const override { return true; }

        int getNumPrograms() override { return std::max (1, (int) info.presets.size()); }
        int getCurrentProgram() override { return currentProgram; }
        void setCurrentProgram (int index) override
        {
            if (juce::isPositiveAndBelow (index, (int) info.presets.size()))
                applyPreset (index, true);
        }
        const juce::String getProgramName (int index) override
        {
            return juce::isPositiveAndBelow (index, (int) info.presets.size()) ? juce::String::fromUTF8 (info.presets[(size_t) index].name)
                                                                               : juce::String();
        }
        void changeProgramName (int, const juce::String&) override {}

        // Presets write every parameter once, straight to its final value; bypass and bus stay as the user set them.
        void applyPreset (int index, bool fade)
        {
            if (! juce::isPositiveAndBelow (index, (int) info.presets.size()))
                return;
            const auto& preset = info.presets[(size_t) index];
            for (const auto& spec : specs)
            {
                const juce::String id (spec.id);
                if (id == "bypass" || id == "bus")
                    continue;
                auto* param = apvts.getParameter (spec.id);
                if (param == nullptr)
                    continue;
                float target = spec.defaultValue;
                for (const auto& [pid, value] : preset.values)
                    if (id == pid)
                    {
                        target = value;
                        break;
                    }
                param->setValueNotifyingHost (param->convertTo0to1 (target));
            }
            currentProgram = index;
            chain->presetLoaded (index);
            if (fade)
                programChangePending.store (true);
        }

        //==============================================================================
        void getStateInformation (juce::MemoryBlock& destData) override
        {
            auto state = apvts.copyState();
            state.setProperty ("version", info.version, nullptr);
            state.setProperty ("program", currentProgram, nullptr);
            state.setProperty ("instanceId", juce::String::toHexString ((juce::int64) instanceId), nullptr);
            state.setProperty ("callsign", callsign, nullptr);
            auto xml = state.createXml();
            if (xml == nullptr)
                return;
            if (const int v = chain->blobVersion(); v > 0)
            {
                juce::MemoryOutputStream rawBlob;
                chain->saveBlob (rawBlob);
                juce::MemoryOutputStream packed;
                {
                    juce::GZIPCompressorOutputStream gz (packed, 9);
                    gz.write (rawBlob.getData(), rawBlob.getDataSize());
                }
                auto* blob = xml->createNewChildElement ("LX11");
                blob->setAttribute ("version", v);
                blob->setAttribute ("code", info.code);
                blob->setAttribute ("data", juce::Base64::toBase64 (packed.getData(), packed.getDataSize()));
            }
            copyXmlToBinary (*xml, destData);
        }

        void setStateInformation (const void* data, int sizeInBytes) override
        {
            auto xml = getXmlFromBinary (data, sizeInBytes);
            if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
                return;

            std::unique_ptr<juce::XmlElement> blob;
            if (auto* b = xml->getChildByName ("LX11"))
            {
                blob = std::make_unique<juce::XmlElement> (*b);
                xml->removeChildElement (b, true);
            }

            auto tree = juce::ValueTree::fromXml (*xml);
            currentProgram = juce::jlimit (0, getNumPrograms() - 1, (int) tree.getProperty ("program", 0));
            const auto idText = tree.getProperty ("instanceId").toString();
            if (idText.isNotEmpty())
                instanceId = (uint32_t) idText.getHexValue64() | 1u;
            const auto sign = tree.getProperty ("callsign").toString();
            callsign = sign.isNotEmpty() ? bus::unpackCallsign (bus::packCallsign (sign)) : bus::defaultCallsign (instanceId);
            busClient.setIdentity (instanceId, callsign);
            apvts.replaceState (tree);

            if (blob != nullptr && chain->blobVersion() > 0)
            {
                juce::MemoryOutputStream packed;
                if (juce::Base64::convertFromBase64 (packed, blob->getStringAttribute ("data")))
                {
                    juce::MemoryInputStream packedIn (packed.getData(), packed.getDataSize(), false);
                    juce::GZIPDecompressorInputStream gz (packedIn);
                    juce::MemoryBlock rawBlob;
                    gz.readIntoMemoryBlock (rawBlob);
                    juce::MemoryInputStream in (rawBlob, false);
                    suspendProcessing (true);
                    chain->loadBlob (in, blob->getIntAttribute ("version"));
                    suspendProcessing (false);
                }
            }
        }

        //==============================================================================
        // Fault log: <user app data>\OPERATION FAIRWAY, LLC\LAB X-11 faults.log, written on the message thread.
        juce::File getFaultLogFile() const
        {
            if (faultLogOverride != juce::File())
                return faultLogOverride;
            return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                       .getChildFile (vendorName).getChildFile (getName() + " faults.log");
        }
        void setFaultLogFile (const juce::File& file) { faultLogOverride = file; }

        juce::String stageName (int stage) const
        {
            if (juce::isPositiveAndBelow (stage, (int) info.stages.size()))
                return juce::String (info.stages[(size_t) stage]).toUpperCase();
            return "OUTPUT";
        }

        int flushFaultLog()
        {
            const int written = faultWrite.load (std::memory_order_acquire);
            if (written == faultRead)
                return 0;
            const int size = (int) faultRing.size();
            const int from = std::max (faultRead, written - size);
            const int lost = from - faultRead;
            faultRead = written;

            auto file = getFaultLogFile();
            file.getParentDirectory().createDirectory();
            juce::FileOutputStream out (file);
            if (! out.openedOk())
                return 0;
            out.setPosition (out.getFile().getSize());
            const auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H:%M:%S");
            if (! faultLogHeaderWritten)
            {
                faultLogHeaderWritten = true;
                out << "\n" << stamp << "  " << getName() << " " << info.version << "  sample rate " << (int) currentSampleRate
                    << "  block " << preparedBlockSize << "  latency " << latencySamples << "  callsign " << callsign << "\n";
            }
            if (lost > 0)
                out << stamp << "  (" << lost << " entries lost: too many in half a second)\n";
            int count = 0;
            bool wantParams = false;
            for (int i = from; i < written; ++i)
            {
                const auto f = faultRing[(size_t) (i % size)];
                out << stamp << "  " << (f.fault ? "FAULT" : "hot  ") << "  " << stageName (f.stage).paddedRight (' ', 10)
                    << " peak " << (f.peakDb > 900.0f ? juce::String ("non-finite") : juce::String (f.peakDb, 1) + " dBFS")
                    << "  at " << juce::String (f.seconds, 2) << " s  bpm " << juce::String (f.bpm, 1)
                    << (f.playing ? " playing" : " stopped") << "  preset " << getProgramName (currentProgram) << "\n";
                wantParams = wantParams || f.fault;
                ++count;
            }
            if (wantParams)
            {
                juce::String line ("    params:");
                for (const auto& spec : specs)
                    line << " " << spec.id << "=" << juce::String (apvts.getRawParameterValue (spec.id)->load(), 3);
                out << line << "\n";
            }
            out.flush();
            return count;
        }

        //==============================================================================
       #if LABX11_HARNESS
        // Tests pin the instance id so hash01 decisions and seeded noise repeat from render to render.
        void setInstanceIdForTest (uint32_t id)
        {
            instanceId = id | 1u;
            callsign = bus::defaultCallsign (instanceId);
            busClient.setIdentity (instanceId, callsign);
        }
        std::array<float, 16> stagePeak {};
        int64_t samplesRendered = 0;
        int injectStage = -1;          // test hook: a 1e9 sample appears after this stage...
        int64_t injectAtSample = -1;   // ...in the control block containing this sample
       #endif

    protected:
        double currentSampleRate = 48000.0;

    private:
        friend class Guard;

        static BusesProperties makeBuses (bool sidechain)
        {
            auto buses = BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true);
            // Active by default (FL Studio 20.6 crashed reloading a project that routed a sidechain into a stub whose
            // sidechain bus started inactive; see docs/DESIGN.md). Unrouted, the host feeds it silence.
            if (sidechain)
                buses = buses.withInput ("Sidechain", juce::AudioChannelSet::stereo(), true);
            return buses;
        }

        static std::vector<ParamSpec> makeSpecs (const EffectInfo& i)
        {
            auto all = commonParams (i.busClient);
            all.insert (all.end(), i.params.begin(), i.params.end());
            return all;
        }

        void timerCallback() override { flushFaultLog(); }

        void readTransport() noexcept
        {
            bool synced = false;
            double ppq = 0.0;
            if (auto* host = getPlayHead())
                if (auto position = host->getPosition())
                {
                    if (auto bpm = position->getBpm())
                        if (*bpm > 1.0)
                            transport.bpm = *bpm;
                    if (auto sig = position->getTimeSignature())
                        if (sig->numerator > 0 && sig->denominator > 0)
                        {
                            transport.numerator = sig->numerator;
                            transport.denominator = sig->denominator;
                        }
                    if (position->getIsPlaying())
                        if (auto p = position->getPpqPosition())
                        {
                            synced = true;
                            ppq = *p;
                        }
                }
            transport.playing = synced;
            transport.sampleRate = currentSampleRate;
            transport.ppqPerSample = transport.bpm / 60.0 / currentSampleRate;
            blockPpq = ppq;
        }

        void updateControl (int n) noexcept
        {
            const float coef = smoothCoef[(size_t) std::clamp (n, 0, controlBlock)];
            for (size_t i = 0; i < specs.size(); ++i)
            {
                const float target = rawValue ((int) i);
                values[i] = smoothers[i].enabled ? smoothers[i].advance (target, coef) : target;
            }
        }

        void mixGains (float mix, float& dry, float& wet) const noexcept
        {
            mix = std::clamp (mix, 0.0f, 1.0f);
            if (mix <= 0.0f) { dry = 1.0f; wet = 0.0f; return; }
            if (mix >= 1.0f) { dry = 0.0f; wet = 1.0f; return; }
            if (info.mixLaw == MixLaw::equalPower)
            {
                dry = dsp::sin2pi (0.25f + 0.25f * mix);   // cos (mix * pi / 2)
                wet = dsp::sin2pi (0.25f * mix);           // sin (mix * pi / 2)
            }
            else
            {
                dry = 1.0f - mix;
                wet = mix;
            }
        }

        void noteFault (int stage, float peak, bool fault) noexcept
        {
            const int64_t now = sampleClock;
            if (! fault)
            {
                auto& last = lastHotAt[(size_t) std::clamp (stage, 0, (int) lastHotAt.size() - 1)];
                if (last >= 0 && now - last < (int64_t) (2.0 * currentSampleRate))
                    return;
                last = now;
            }
            const int index = faultWrite.load (std::memory_order_relaxed);
            auto& f = faultRing[(size_t) (index % (int) faultRing.size())];
            f.stage = stage;
            f.peakDb = std::isfinite (peak) ? juce::Decibels::gainToDecibels (peak, -120.0f) : 999.0f;
            f.fault = fault;
            f.seconds = (double) now / currentSampleRate;
            f.bpm = transport.bpm;
            f.playing = transport.playing;
            faultWrite.store (index + 1, std::memory_order_release);
            if (fault)
            {
                meters.faults.fetch_add (1);
                meters.lastFaultStage.store (stage);
            }
        }

        std::unique_ptr<Chain> chain;
        std::vector<std::atomic<float>*> raw;
        std::vector<Smoother> smoothers;
        std::vector<float> values;
        std::array<float, controlBlock + 1> smoothCoef {};

        dsp::Transport transport;
        double blockPpq = 0.0;
        int64_t sampleClock = 0;
        int preparedBlockSize = 0, latencySamples = 0, meterSamples = 0;
        dsp::StereoDelayLine dryLine;
        float mixNow = 1.0f, outputGainNow = 1.0f, bypassMix = 0.0f, bypassStep = 0.001f;

        enum class SwitchState { idle, fadingOut, fadingIn };
        std::atomic<bool> programChangePending { false };
        SwitchState switchState = SwitchState::idle;
        int switchFadeRemaining = 0, switchFadeLength = 1440;
        float switchGain = 1.0f, switchFadeInStep = 0.005f;
        int currentProgram = 0;

        uint32_t instanceId = 1;
        juce::String callsign;
        bus::Client busClient;

        std::array<float, scopeSize> scope {};
        std::atomic<int> scopeWrite { 0 };

        static constexpr float stageLimit = 128.0f;   // +42 dBFS anywhere in a chain
        static constexpr float hotLevel = 8.0f;       // +18 dBFS: logged, not reset
        struct FaultEntry
        {
            int stage = 0;
            float peakDb = 0.0f;
            bool fault = false, playing = false;
            double seconds = 0.0, bpm = 120.0;
        };
        std::array<FaultEntry, 64> faultRing {};
        std::atomic<int> faultWrite { 0 };
        int faultRead = 0;
        std::array<int64_t, 17> lastHotAt { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
        juce::File faultLogOverride;
        bool faultLogHeaderWritten = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EffectProcessor)
    };

    // The guard after each stage: above +42 dBFS or non-finite is a fault (the block is silenced, the chain reset and
    // the stage logged); above +18 dBFS is logged as hot at most every 2 s per stage.
    inline bool Guard::operator() (int stage, float* l, float* r, int n) noexcept
    {
        if (owner == nullptr || n <= 0)
            return false;
       #if LABX11_HARNESS
        if (owner->injectStage == stage && owner->injectAtSample >= owner->sampleClock && owner->injectAtSample < owner->sampleClock + n)
            l[0] = 1.0e9f;
       #endif
        const float peak = dsp::blockPeak (l, r, n);
       #if LABX11_HARNESS
        if (juce::isPositiveAndBelow (stage, (int) owner->stagePeak.size()))
            owner->stagePeak[(size_t) stage] = std::max (owner->stagePeak[(size_t) stage], std::isfinite (peak) ? peak : 1.0e30f);
       #endif
        if (! (peak <= EffectProcessor::stageLimit))
        {
            owner->noteFault (stage, peak, true);
            tripped = true;
            return true;
        }
        if (peak > EffectProcessor::hotLevel)
            owner->noteFault (stage, peak, false);
        return tripped;
    }
}
