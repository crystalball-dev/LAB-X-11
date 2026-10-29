// LabX11Render: the offline harness (ported unchanged from LAB X-16's Lx16<Name>Render, compiled with the plugin's
// sources and LABX11_HARNESS=1). It drives the effect through createPluginFilter() and prints key=value statistics
// for test/run_tests.py. --selftest also runs the C++ unit checks of the X-11 DSP headers (T0).
//
//   LabX11Render --out <file.wav> [--seconds 4] [--sr 48000] [--block 512] [--vary-blocks] [--bpm 120] [--stopped]
//               [--signal sine:440|impulse|impulses:2|noise|pink|sweep:20-20000|chord:C3,E3,G3|drums:172|silence]
//               [--in <file>] [--level -12] [--side <signal>] [--midi "60@0-2:100,64@1-2"]
//               [--set id=value ...] [--ramp id=a..b@t0-t1] [--step id=v@t] [--program N] [--program-at "N@s"]
//               [--text "<chain text>"] [--text k:"..."]   (the chain's own state as text: ANOMALY patterns, a key)
//               (--set, --step and --ramp take a k: prefix for one instance of several: --step 0:arm=1@2.0)
//               [--null-check] [--latency-check] [--roundtrip] [--list-params] [--selftest]
//               [--instances N] [--set k:id=value]          (bus effects: N processors in lockstep, one WAV each)
//               [--fuzz [--seed 1]] [--no-growth-check]     (random parameters, presets, jumps, blocks, signals)
//               [--inject stage@s] [--fault-log <file>] [--png <file.png> [--png-wait ms]] [--no-trim]

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <iostream>
#include "core/host/EffectProcessor.h"
#include "core/dsp/Gate.h"
#include "core/dsp/Leveler.h"
#include "ui/EffectEditor.h"

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#endif

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

namespace
{
    using lab::host::EffectProcessor;

    uint32_t instanceIdBase = 0x11000001u;   // --id <hex>

    struct TestPlayHead final : public juce::AudioPlayHead
    {
        double bpm = 120.0, sampleRate = 48000.0;
        juce::int64 position = 0;
        bool playing = true;
        double jumpAtSeconds = -1.0, jumpToQuarter = 0.0;

        double quarterAt (juce::int64 samples) const
        {
            const double seconds = (double) samples / sampleRate;
            if (jumpAtSeconds >= 0.0 && seconds >= jumpAtSeconds)
                return jumpToQuarter + (seconds - jumpAtSeconds) * bpm / 60.0;
            return seconds * bpm / 60.0;
        }

        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo info;
            info.setBpm (bpm);
            info.setTimeInSamples (position);
            info.setPpqPosition (quarterAt (position));
            info.setTimeSignature (TimeSignature { 4, 4 });
            info.setIsPlaying (playing);
            return info;
        }
    };

    juce::String option (const juce::StringArray& args, const juce::String& key, const juce::String& fallback = {})
    {
        const int i = args.indexOf (key);
        return (i >= 0 && i + 1 < args.size()) ? args[i + 1] : fallback;
    }

    juce::StringArray allOptions (const juce::StringArray& args, const juce::String& key)
    {
        juce::StringArray out;
        for (int i = 0; i + 1 < args.size(); ++i)
            if (args[i] == key)
                out.add (args[i + 1]);
        return out;
    }

    double threadCpuSeconds()
    {
       #if JUCE_WINDOWS
        FILETIME created, exited, kernel, user;
        if (GetThreadTimes (GetCurrentThread(), &created, &exited, &kernel, &user))
        {
            ULARGE_INTEGER k, u;
            k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
            u.LowPart = user.dwLowDateTime;   u.HighPart = user.dwHighDateTime;
            return (double) (k.QuadPart + u.QuadPart) * 1.0e-7;
        }
       #endif
        return juce::Time::getMillisecondCounterHiRes() * 0.001;
    }

    bool setParam (juce::AudioProcessor& proc, EffectProcessor* fx, const juce::String& assignment)
    {
        const auto id = assignment.upToFirstOccurrenceOf ("=", false, false).trim();
        const auto value = assignment.fromFirstOccurrenceOf ("=", false, false).trim().getFloatValue();
        if (fx != nullptr)
            if (auto* p = fx->apvts.getParameter (id))
            {
                p->setValueNotifyingHost (p->convertTo0to1 (value));
                return true;
            }
        juce::ignoreUnused (proc);
        std::cerr << "unknown parameter " << id << std::endl;
        return false;
    }

    // ------------------------------------------------------------------------------------------------------------
    // Test signals: everything is synthesized (no sample dependency).
    float noteToHz (const juce::String& name)
    {
        static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        const auto letters = name.retainCharacters ("ABCDEFG#b");
        const int octave = name.retainCharacters ("-0123456789").getIntValue();
        int pc = 0;
        for (int i = 0; i < 12; ++i)
            if (letters.equalsIgnoreCase (names[i]))
                pc = i;
        return lab::dsp::midiToHz ((float) (12 * (octave + 1) + pc));
    }

    juce::AudioBuffer<float> makeSignal (const juce::String& spec, double sr, int total, float levelDb, uint32_t seed)
    {
        juce::AudioBuffer<float> out (2, total);
        out.clear();
        const auto kind = spec.upToFirstOccurrenceOf (":", false, false).toLowerCase();
        const auto arg = spec.fromFirstOccurrenceOf (":", false, false);
        const float amp = juce::Decibels::decibelsToGain (levelDb);
        lab::dsp::Rng rng;
        rng.seed (seed);
        float* l = out.getWritePointer (0);
        float* r = out.getWritePointer (1);

        if (kind == "sine")
        {
            const double hz = arg.isNotEmpty() ? arg.getDoubleValue() : 440.0;
            for (int i = 0; i < total; ++i)
                l[i] = r[i] = amp * (float) std::sin (2.0 * juce::MathConstants<double>::pi * hz * (double) i / sr);
        }
        else if (kind == "impulse")
        {
            const int at = (int) (0.1 * sr);
            if (at < total)
                l[at] = r[at] = amp;
        }
        else if (kind == "impulses")
        {
            const double perSecond = arg.isNotEmpty() ? arg.getDoubleValue() : 2.0;
            for (double t = 0.1; t * sr < total; t += 1.0 / perSecond)
                l[(int) (t * sr)] = r[(int) (t * sr)] = amp;
        }
        else if (kind == "noise" || kind == "pink")
        {
            float b0 = 0, b1 = 0, b2 = 0;
            for (int i = 0; i < total; ++i)
            {
                float w = rng.bi();
                if (kind == "pink")
                {
                    b0 = 0.99765f * b0 + w * 0.0990460f;
                    b1 = 0.96300f * b1 + w * 0.2965164f;
                    b2 = 0.57000f * b2 + w * 1.0526913f;
                    w = (b0 + b1 + b2 + w * 0.1848f) * 0.25f;
                }
                l[i] = amp * w;
                r[i] = amp * (kind == "pink" ? w : rng.bi());
            }
        }
        else if (kind == "sweep")
        {
            const double f0 = arg.isNotEmpty() ? arg.upToFirstOccurrenceOf ("-", false, false).getDoubleValue() : 20.0;
            const double f1 = arg.contains ("-") ? arg.fromFirstOccurrenceOf ("-", false, false).getDoubleValue() : 20000.0;
            const double seconds = (double) total / sr, k = std::log (f1 / f0);
            for (int i = 0; i < total; ++i)
            {
                const double t = (double) i / sr;
                const double phase = 2.0 * juce::MathConstants<double>::pi * f0 * seconds / k * (std::exp (k * t / seconds) - 1.0);
                l[i] = r[i] = amp * (float) std::sin (phase);
            }
        }
        else if (kind == "chord")
        {
            const auto notes = juce::StringArray::fromTokens (arg.isNotEmpty() ? arg : "C3,E3,G3", ",", "");
            for (const auto& n : notes)
            {
                const double hz = noteToHz (n);
                double phase = 0.0;
                for (int i = 0; i < total; ++i)
                {
                    const float saw = (float) (2.0 * phase - 1.0) - lab::dsp::polyBlep ((float) phase, (float) (hz / sr));
                    phase += hz / sr;
                    if (phase >= 1.0) phase -= 1.0;
                    l[i] += amp * 0.4f * saw;
                    r[i] += amp * 0.4f * saw;
                }
            }
        }
        else if (kind == "drums")
        {
            // Synthesized kick (sine sweep), snare (noise + 180 Hz) and hats (filtered noise) at the given BPM.
            const double bpm = arg.isNotEmpty() ? arg.getDoubleValue() : 172.0;
            const double step = 60.0 / bpm / 4.0 * sr;
            const char* kick  = "x.........x.....";
            const char* snare = "....x.......x...";
            float hp = 0.0f, prev = 0.0f;
            for (int s = 0; (double) s * step < total; ++s)
            {
                const int at = (int) ((double) s * step);
                const int slot = s % 16;
                for (int i = 0; i < (int) (0.3 * sr) && at + i < total; ++i)
                {
                    const double t = (double) i / sr;
                    float v = 0.0f;
                    if (kick[slot] == 'x')
                        v += (float) (std::sin (2.0 * juce::MathConstants<double>::pi * (45.0 * t + 90.0 * (1.0 - std::exp (-t * 30.0)) / 30.0))
                                      * std::exp (-t * 9.0));
                    if (snare[slot] == 'x')
                        v += (float) ((rng.bi() * 0.6 + 0.4 * std::sin (2.0 * juce::MathConstants<double>::pi * 180.0 * t)) * std::exp (-t * 18.0));
                    if ((slot % 2) == 0 && i < (int) (0.05 * sr))
                    {
                        const float w = rng.bi();
                        hp = 0.6f * (hp + w - prev);
                        prev = w;
                        v += 0.35f * hp * (float) std::exp (-t * 80.0);
                    }
                    l[at + i] += amp * v;
                    r[at + i] += amp * v;
                }
            }
        }
        return out;
    }

    juce::AudioBuffer<float> loadInput (const juce::File& file, double sr, int total, float levelDb)
    {
        juce::AudioBuffer<float> out (2, total);
        out.clear();
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        if (reader == nullptr)
        {
            std::cerr << "cannot read " << file.getFullPathName() << std::endl;
            return out;
        }
        const int srcLen = (int) reader->lengthInSamples;
        juce::AudioBuffer<float> src (2, srcLen);
        reader->read (&src, 0, srcLen, 0, true, true);
        const double ratio = reader->sampleRate / sr;
        const float gain = juce::Decibels::decibelsToGain (levelDb);
        for (int ch = 0; ch < 2; ++ch)
        {
            const float* s = src.getReadPointer (std::min (ch, (int) reader->numChannels - 1));
            float* d = out.getWritePointer (ch);
            for (int i = 0; i < total; ++i)
            {
                const double pos = (double) i * ratio;
                const int k = (int) pos;
                if (k + 1 >= srcLen)
                    break;
                const float f = (float) (pos - (double) k);
                d[i] = gain * (s[k] + f * (s[k + 1] - s[k]));
            }
        }
        return out;
    }

    bool writeWav (const juce::File& file, const juce::AudioBuffer<float>& buffer, double sr, int bits = 32)
    {
        file.deleteFile();
        std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
        if (stream == nullptr)
            return false;
        juce::WavAudioFormat wav;
        auto options = juce::AudioFormatWriterOptions().withSampleRate (sr).withNumChannels (buffer.getNumChannels()).withBitsPerSample (bits);
        if (bits == 32)
            options = options.withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
        if (auto writer = wav.createWriterFor (stream, options))
            return writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
        return false;
    }

    struct MidiEvent { int note; double start, end; int velocity; };
    std::vector<MidiEvent> parseMidi (const juce::String& spec)
    {
        std::vector<MidiEvent> events;
        for (const auto& token : juce::StringArray::fromTokens (spec, ",", ""))
        {
            MidiEvent e { 60, 0.0, 1.0, 100 };
            e.note = token.upToFirstOccurrenceOf ("@", false, false).getIntValue();
            auto times = token.fromFirstOccurrenceOf ("@", false, false);
            if (times.contains (":"))
            {
                e.velocity = juce::jlimit (1, 127, times.fromFirstOccurrenceOf (":", false, false).getIntValue());
                times = times.upToFirstOccurrenceOf (":", false, false);
            }
            e.start = times.upToFirstOccurrenceOf ("-", false, false).getDoubleValue();
            e.end = times.contains ("-") ? times.fromFirstOccurrenceOf ("-", false, false).getDoubleValue() : e.start + 0.5;
            events.push_back (e);
        }
        return events;
    }

    struct Automation { juce::String id; float a, b; double t0, t1; int instance = -1; };

    // "k:rest" -> (k, rest) when the text starts with an instance number and a colon; (-1, text) otherwise.
    std::pair<int, juce::String> splitInstance (const juce::String& text)
    {
        const auto head = text.upToFirstOccurrenceOf (":", false, false);
        if (text.contains (":") && head.isNotEmpty() && head.containsOnly ("0123456789"))
            return { head.getIntValue(), text.fromFirstOccurrenceOf (":", false, false) };
        return { -1, text };
    }

    struct Stats
    {
        int nonFinite = 0;
        float peak = 0.0f, maxJump = 0.0f;
        double sumSquares = 0.0;
        juce::int64 count = 0;

        void add (const juce::AudioBuffer<float>& b, int start, int n)
        {
            for (int ch = 0; ch < b.getNumChannels(); ++ch)
            {
                const float* d = b.getReadPointer (ch);
                for (int i = start; i < start + n; ++i)
                {
                    if (! std::isfinite (d[i])) { ++nonFinite; continue; }
                    peak = std::max (peak, std::abs (d[i]));
                    sumSquares += (double) d[i] * d[i];
                    if (i > 0 && std::isfinite (d[i - 1]))
                        maxJump = std::max (maxJump, std::abs (d[i] - d[i - 1]));
                }
                count += n;
            }
        }
        float rmsDb() const { return juce::Decibels::gainToDecibels ((float) std::sqrt (sumSquares / (double) std::max<juce::int64> (1, count)), -240.0f); }
    };

    // ------------------------------------------------------------------------------------------------------------
    // The play head lives on the heap: an Instance is moved (returned, pushed into vectors), and the processor keeps a
    // pointer to its play head, which must not move with it.
    struct Instance
    {
        std::unique_ptr<juce::AudioProcessor> proc;
        EffectProcessor* fx = nullptr;
        std::unique_ptr<TestPlayHead> head = std::make_unique<TestPlayHead>();
    };

    Instance makeInstance (double sr, int, double bpm, bool playing)
    {
        Instance in;
        in.proc.reset (createPluginFilter());
        in.fx = dynamic_cast<EffectProcessor*> (in.proc.get());
        // A fixed id per instance (overridable with --id) so every render of a test repeats exactly.
        static int made = 0;
        if (in.fx != nullptr)
            in.fx->setInstanceIdForTest (instanceIdBase + 2u * (uint32_t) made++);
        in.head->bpm = bpm;
        in.head->sampleRate = sr;
        in.head->playing = playing;
        in.proc->setPlayHead (in.head.get());
        return in;
    }

    void prepare (Instance& in, double sr, int block, bool sidechain)
    {
        // Main stereo in/out, the sidechain bus enabled when the effect has one and a side signal is given.
        if (in.proc->getBusCount (true) > 1)
        {
            auto layout = in.proc->getBusesLayout();
            layout.inputBuses.getReference (1) = sidechain ? juce::AudioChannelSet::stereo() : juce::AudioChannelSet::disabled();
            in.proc->setBusesLayout (layout);
        }
        in.proc->setRateAndBufferSizeDetails (sr, block);
        in.proc->prepareToPlay (sr, block);
    }

    int runSelfTest()
    {
        const float mono = lab::dsp::DelayLine::selfTest();
        const float stereo = lab::dsp::StereoDelayLine::selfTest();
        std::cout << "selftest_delay_mono=" << mono << " selftest_delay_stereo=" << stereo << std::endl;
        // LAB X-11's own DSP headers (T0): every case prints selftest_<case>=<value> and selftest_<case>_ok=<0|1>.
        lab::dsp::SelfTestReport report;
        {
            juce::ScopedNoDenormals noDenormals;   // as on the audio thread
            report.merge (lab::dsp::TransientAlign::selfTest());
            report.merge (lab::dsp::VocalGate::selfTest());
            report.merge (lab::dsp::LevelMatcher::selfTest());
        }
        // L31: LevelMatcher::defaults (used before the first control()) equal the level_* rows of the parameter table.
        {
            auto in = makeInstance (48000.0, 512, 120.0, true);
            const auto& specs = in.fx->getSpecs();
            int first = -1;
            for (int i = 0; i < (int) specs.size(); ++i)
                if (juce::String (specs[(size_t) i].id) == "level_amount")
                    first = i;
            bool same = first >= 0 && first + lab::dsp::LevelMatcher::paramCount <= (int) specs.size();
            for (int k = 0; same && k < lab::dsp::LevelMatcher::paramCount; ++k)
                same = std::abs (specs[(size_t) (first + k)].defaultValue - lab::dsp::LevelMatcher::defaults[k]) <= 0.0f;
            report.add ("level_defaults", same ? 1.0 : 0.0, same);
        }
        std::cout << report.text << "selftest_cases=" << report.cases << " selftest_failures=" << report.failures << std::endl;
        return (mono < 1.0e-4f && stereo < 1.0e-4f && report.failures == 0) ? 0 : 1;
    }

    int runRoundtrip (double sr)
    {
        auto a = makeInstance (sr, 512, 120.0, true);
        prepare (a, sr, 512, false);
        juce::Random rng (77);
        for (auto* p : a.proc->getParameters())
            p->setValueNotifyingHost (rng.nextFloat());
        if (a.fx != nullptr)
            a.fx->setCallsign ("RT-01");
        // Run a little audio so learned state (if any) is not trivial.
        juce::AudioBuffer<float> buf (2, 512);
        juce::MidiBuffer midi;
        for (int k = 0; k < 40; ++k)
        {
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 512; ++i)
                    buf.setSample (ch, i, 0.3f * std::sin ((float) (k * 512 + i) * 0.031f));
            a.head->position = k * 512;
            a.proc->processBlock (buf, midi);
        }
        juce::MemoryBlock state;
        a.proc->getStateInformation (state);

        auto b = makeInstance (sr, 512, 120.0, true);
        prepare (b, sr, 512, false);
        b.proc->setStateInformation (state.getData(), (int) state.getSize());
        juce::MemoryBlock again;
        b.proc->getStateInformation (again);

        int mismatches = 0;
        const auto& pa = a.proc->getParameters();
        const auto& pb = b.proc->getParameters();
        for (int i = 0; i < pa.size(); ++i)
            if (std::abs (pa[i]->getValue() - pb[i]->getValue()) > 1.0e-6f)
                ++mismatches;
        const bool same = state == again;
        const bool callsignOk = a.fx == nullptr || b.fx->getCallsign() == a.fx->getCallsign();
        std::cout << "roundtrip=" << (mismatches == 0 && same && callsignOk ? "PASS" : "FAIL") << " param_mismatches=" << mismatches
                  << " state_identical=" << (same ? 1 : 0) << " callsign_ok=" << (callsignOk ? 1 : 0) << " state_bytes=" << (int) state.getSize()
                  << std::endl;
        return mismatches == 0 && same && callsignOk ? 0 : 1;
    }

    int listParams()
    {
        auto in = makeInstance (48000.0, 512, 120.0, true);
        if (in.fx == nullptr)
            return 1;
        for (const auto& s : in.fx->getSpecs())
            std::cout << "param " << s.id << " kind=" << (int) s.kind << " min=" << s.minValue << " max=" << s.maxValue
                      << " default=" << s.defaultValue << " choices=\"" << s.choices << "\"" << std::endl;
        for (int i = 0; i < in.proc->getNumPrograms(); ++i)
            std::cout << "preset " << i << " \"" << in.proc->getProgramName (i) << "\"" << std::endl;
        std::cout << "product=\"" << in.proc->getName() << "\" latency=" << in.proc->getLatencySamples()
                  << " stages=" << (int) in.fx->info.stages.size() << std::endl;
        return 0;
    }

    // Random parameters, presets, transport jumps, block sizes and signals; silence stretches check that nothing
    // grows without input.
    // Growth in silence, after 4 s of it: `min` flags a 100 ms window 6 dB above the quietest so far (tails must die);
    // `peak` flags one 3 dB above the loudest of those first 4 s (for effects that sound on in silence by design: a
    // delay's long echoes, DOSIMETER's clicks).
    int runFuzz (double sr, double seconds, int seed, bool varyBlocks, int fixedBlock, bool growthCheck, bool peakGrowth)
    {
        auto in = makeInstance (sr, 4096, 120.0, true);
        prepare (in, sr, 4096, in.proc->getBusCount (true) > 1);
        juce::Random rng (seed);
        const int total = (int) (seconds * sr);
        const auto drums = makeSignal ("drums:150", sr, (int) (8.0 * sr), -6.0f, 1);
        const auto noise = makeSignal ("pink", sr, (int) (8.0 * sr), -12.0f, 2);
        juce::AudioBuffer<float> block (std::max (2, in.proc->getTotalNumInputChannels()), 4096);
        juce::MidiBuffer midi;
        Stats stats;
        int presetChanges = 0, paramChanges = 0, jumps = 0, growth = 0, latencyChanges = 0;
        const int latency = in.proc->getLatencySamples();
        float worstGrowth = -1000.0f;
        const double cycle = 30.0, activeSpan = 20.0;
        double nextEvent = 0.0;
        double windowSum = 0.0, minSoFar = 1000.0, earlyMax = -1000.0;
        int windowCount = 0, windowIndex = 0;
        bool inSilence = false;
        const int window = (int) (0.1 * sr);
        const auto cpu0 = threadCpuSeconds();

        for (int pos = 0; pos < total;)
        {
            const int n = varyBlocks ? std::min (total - pos, 1 + rng.nextInt (3000)) : std::min (total - pos, fixedBlock);
            const double t0 = (double) pos / sr;
            const bool silent = std::fmod (t0, cycle) >= activeSpan;
            if (silent && ! inSilence)
            {
                inSilence = true;
                windowSum = 0.0;
                windowCount = windowIndex = 0;
                minSoFar = 1000.0;
                earlyMax = -1000.0;
            }
            else if (! silent)
                inSilence = false;

            while (! silent && nextEvent < t0 + (double) n / sr)
            {
                const float r = rng.nextFloat();
                const auto& params = in.proc->getParameters();
                if (r < 0.6f && params.size() > 0)
                {
                    auto* p = params[rng.nextInt (params.size())];
                    if (p != in.proc->getBypassParameter())
                        p->setValueNotifyingHost (rng.nextFloat());
                    ++paramChanges;
                }
                else if (r < 0.7f)
                {
                    in.proc->setCurrentProgram (rng.nextInt (std::max (1, in.proc->getNumPrograms())));
                    ++presetChanges;
                }
                else if (r < 0.75f)
                {
                    in.head->jumpAtSeconds = t0;
                    in.head->jumpToQuarter = rng.nextFloat() * 64.0f;
                    ++jumps;
                }
                nextEvent += 0.05 + rng.nextDouble() * 0.5;
            }

            block.setSize (block.getNumChannels(), n, false, false, true);
            block.clear();
            if (! silent)
            {
                const auto& src = (int) (t0 / 4.0) % 2 == 0 ? drums : noise;
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < n; ++i)
                        block.setSample (ch, i, src.getSample (ch, (pos + i) % src.getNumSamples()));
                for (int ch = 2; ch < block.getNumChannels(); ++ch)
                    for (int i = 0; i < n; ++i)
                        block.setSample (ch, i, noise.getSample (ch % 2, (pos + i) % noise.getNumSamples()));
            }
            in.head->position = pos;
            in.proc->processBlock (block, midi);
            stats.add (block, 0, n);
            if (in.proc->getLatencySamples() != latency)
                ++latencyChanges;

            if (inSilence && growthCheck)
            {
                for (int i = 0; i < n; ++i)
                {
                    const float l = block.getSample (0, i), r = block.getSample (1, i);
                    windowSum += (double) (l * l + r * r) * 0.5;
                    if (++windowCount >= window)
                    {
                        const float db = juce::Decibels::gainToDecibels ((float) std::sqrt (windowSum / windowCount), -240.0f);
                        if (windowIndex < 40)
                            earlyMax = std::max (earlyMax, (double) db);
                        else if (db > -80.0f)   // after 4 s: tails have died (min) or only fall (peak)
                        {
                            const double reference = peakGrowth ? earlyMax + 3.0 : minSoFar + 6.0;
                            worstGrowth = std::max (worstGrowth, (float) (db - (peakGrowth ? earlyMax : minSoFar)));
                            if (db > reference)
                                ++growth;
                        }
                        minSoFar = std::min (minSoFar, (double) db);
                        windowSum = 0.0;
                        windowCount = 0;
                        ++windowIndex;
                    }
                }
            }
            pos += n;
        }
        const double cpu = threadCpuSeconds() - cpu0;
        std::cout << "fuzz seed=" << seed << " seconds=" << seconds << " param_changes=" << paramChanges << " preset_changes=" << presetChanges
                  << " jumps=" << jumps << " non_finite=" << stats.nonFinite << " peak_db=" << juce::Decibels::gainToDecibels (stats.peak, -240.0f)
                  << " growth_silence=" << growth << " worst_growth_db=" << worstGrowth << " latency_changes=" << latencyChanges
                  << " faults=" << (in.fx != nullptr ? in.fx->meters.faults.load() : 0)
                  << " faults_logged=" << (in.fx != nullptr ? in.fx->flushFaultLog() : 0)
                  << " realtime_x=" << (cpu > 0.0 ? seconds / cpu : 0.0) << std::endl;
        in.proc->releaseResources();
        return stats.nonFinite == 0 && growth == 0 && latencyChanges == 0 ? 0 : 1;
    }
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI gui;
    juce::StringArray args;
    for (int i = 1; i < argc; ++i)
        args.add (juce::String::fromUTF8 (argv[i]));

    if (args.contains ("--id"))
        instanceIdBase = (uint32_t) option (args, "--id").getHexValue64() | 1u;
    const double sr = option (args, "--sr", "48000").getDoubleValue();
    const int blockSize = option (args, "--block", "512").getIntValue();
    const double seconds = option (args, "--seconds", "4").getDoubleValue();
    const double bpm = option (args, "--bpm", "120").getDoubleValue();

    if (args.contains ("--selftest"))
        return runSelfTest();
    if (args.contains ("--list-params"))
        return listParams();
    if (args.contains ("--roundtrip"))
        return runRoundtrip (sr);
    if (args.contains ("--fuzz"))
        return runFuzz (sr, seconds, option (args, "--seed", "1").getIntValue(), args.contains ("--vary-blocks"), blockSize,
                        ! args.contains ("--no-growth-check"), option (args, "--growth", "min") == "peak");

    const int instances = std::max (1, option (args, "--instances", "1").getIntValue());
    const bool nullCheck = args.contains ("--null-check");
    const bool latencyCheck = args.contains ("--latency-check");
    const float level = option (args, "--level", "-12").getFloatValue();
    const juce::String sideSpec = option (args, "--side");

    std::vector<Instance> fx;
    for (int k = 0; k < instances; ++k)
        fx.push_back (makeInstance (sr, blockSize, bpm, ! args.contains ("--stopped")));
    auto* first = fx[0].fx;
    if (first == nullptr)
    {
        std::cerr << "not a LAB X effect" << std::endl;
        return 2;
    }

    // Presets first, then explicit settings (so --set overrides a preset).
    if (args.contains ("--program"))
        for (auto& in : fx)
            in.proc->setCurrentProgram (option (args, "--program").getIntValue());
    for (const auto& a : allOptions (args, "--set"))
    {
        const auto head = a.upToFirstOccurrenceOf ("=", false, false);
        if (head.contains (":"))   // k:id=value, for one instance
        {
            const int k = head.upToFirstOccurrenceOf (":", false, false).getIntValue();
            if (juce::isPositiveAndBelow (k, instances))
                if (! setParam (*fx[(size_t) k].proc, fx[(size_t) k].fx, a.fromFirstOccurrenceOf (":", false, false)))
                    return 2;
        }
        else
            for (auto& in : fx)
                if (! setParam (*in.proc, in.fx, a))
                    return 2;
    }
    for (const auto& a : allOptions (args, "--text"))
    {
        const auto [k, text] = splitInstance (a);
        for (int j = 0; j < instances; ++j)
            if ((k < 0 || k == j) && ! fx[(size_t) j].fx->getChain().loadText (text))
            {
                std::cerr << "the chain did not take --text " << text << std::endl;
                return 2;
            }
    }
    if (nullCheck)
        for (auto& in : fx)
            setParam (*in.proc, in.fx, "bypass=1");
    if (args.contains ("--fault-log"))
        first->setFaultLogFile (juce::File (option (args, "--fault-log")));
   #if LABX11_HARNESS
    if (args.contains ("--inject"))
    {
        const auto spec = option (args, "--inject");
        first->injectStage = spec.upToFirstOccurrenceOf ("@", false, false).getIntValue();
        first->injectAtSample = (juce::int64) (spec.fromFirstOccurrenceOf ("@", false, false).getDoubleValue() * sr);
    }
   #endif

    for (auto& in : fx)
        prepare (in, sr, blockSize, sideSpec.isNotEmpty());
    const int latency = first->getLatencySamples();

    // --emission "fireBar,repeatBars,leadBars": an EMISSION schedule on the Zone Bus, as a siren would post it (ARTIFACT's
    // reseed test; the harness hosts one effect only). Bars count from 0.
    lab::host::bus::Client emissionPoster;
    if (args.contains ("--emission"))
    {
        const auto parts = juce::StringArray::fromTokens (option (args, "--emission"), ",", "");
        if (emissionPoster.connect (0x5E1E0001u, "L6Em", "SIREN"))
        {
            lab::host::bus::EmissionSchedule s;
            s.fireBar = parts[0].getLargeIntValue();
            s.repeatBars = parts.size() > 1 ? parts[1].getIntValue() : 0;
            s.leadBars = parts.size() > 2 ? parts[2].getIntValue() : 2;
            s.severity = 1.0f;
            s.pressureSeconds = 2.5f;
            s.aftermathSeconds = 4.0f;
            s.sirenId = 0x5E1E0001u;
            s.seq = 1;
            emissionPoster.postSchedule (s);
        }
        else
            std::cerr << "no Zone Bus for --emission" << std::endl;
    }

    if (args.contains ("--png"))
    {
        std::unique_ptr<juce::AudioProcessorEditor> editor (first->createEditorAndMakeActive());
        editor->setVisible (true);
        if (auto* e = dynamic_cast<lab::ui::EffectEditor*> (editor.get()))
            e->refreshFromProcessor();
        if (args.contains ("--png-wait"))
            juce::MessageManager::getInstance()->runDispatchLoopUntil (option (args, "--png-wait").getIntValue());
        const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);
        const juce::File pngFile (option (args, "--png"));
        pngFile.deleteFile();
        juce::FileOutputStream stream (pngFile);
        juce::PNGImageFormat png;
        const bool ok = stream.openedOk() && png.writeImageToStream (image, stream);
        std::cout << "png=" << pngFile.getFullPathName() << " size=" << image.getWidth() << "x" << image.getHeight()
                  << " ok=" << (ok ? 1 : 0) << std::endl;
        if (auto* e = dynamic_cast<lab::ui::EffectEditor*> (editor.get()))
            std::cout << "panels=\"" << e->panelTitles().joinIntoString ("|") << "\"" << std::endl;
        editor.reset();
        if (! args.contains ("--out"))
            return ok ? 0 : 1;
    }

    // Input and sidechain
    const int total = (int) (seconds * sr);
    const int rendered = total + (args.contains ("--no-trim") ? 0 : latency);
    juce::AudioBuffer<float> input = args.contains ("--in") ? loadInput (juce::File (option (args, "--in")), sr, rendered, level)
                                                            : makeSignal (latencyCheck ? "impulse" : option (args, "--signal", "drums:120"), sr, rendered, level, 7);
    const juce::AudioBuffer<float> side = sideSpec.isNotEmpty() ? makeSignal (sideSpec, sr, rendered, level, 9) : juce::AudioBuffer<float> (2, rendered);
    const auto midiEvents = parseMidi (option (args, "--midi"));

    std::vector<Automation> automation;
    for (const auto& raw : allOptions (args, "--ramp"))   // [k:]id=a..b@t0-t1
    {
        const auto [k, a] = splitInstance (raw);
        const auto id = a.upToFirstOccurrenceOf ("=", false, false);
        const auto values = a.fromFirstOccurrenceOf ("=", false, false).upToFirstOccurrenceOf ("@", false, false);
        const auto times = a.fromFirstOccurrenceOf ("@", false, false);
        automation.push_back ({ id, values.upToFirstOccurrenceOf ("..", false, false).getFloatValue(),
                                values.fromFirstOccurrenceOf ("..", false, false).getFloatValue(),
                                times.upToFirstOccurrenceOf ("-", false, false).getDoubleValue(),
                                times.fromFirstOccurrenceOf ("-", false, false).getDoubleValue(), k });
    }
    for (const auto& raw : allOptions (args, "--step"))   // [k:]id=v@t
    {
        const auto [k, a] = splitInstance (raw);
        const auto id = a.upToFirstOccurrenceOf ("=", false, false);
        const float v = a.fromFirstOccurrenceOf ("=", false, false).upToFirstOccurrenceOf ("@", false, false).getFloatValue();
        const double t = a.fromFirstOccurrenceOf ("@", false, false).getDoubleValue();
        automation.push_back ({ id, v, v, t, t, k });
    }
    struct ProgramChange { int index; double time; };
    std::vector<ProgramChange> programChanges;
    for (const auto& token : juce::StringArray::fromTokens (option (args, "--program-at"), ",", ""))
        programChanges.push_back ({ token.upToFirstOccurrenceOf ("@", false, false).getIntValue(),
                                    token.fromFirstOccurrenceOf ("@", false, false).getDoubleValue() });

    std::vector<juce::AudioBuffer<float>> outputs ((size_t) instances, juce::AudioBuffer<float> (2, rendered));
    for (auto& o : outputs)
        o.clear();
    const int channels = std::max (2, fx[0].proc->getTotalNumInputChannels());
    juce::AudioBuffer<float> block (channels, blockSize);
    juce::Random blockRng (5);
    const bool vary = args.contains ("--vary-blocks");
    const auto cpu0 = threadCpuSeconds();

    for (int pos = 0; pos < rendered;)
    {
        const int n = vary ? std::min (rendered - pos, 1 + blockRng.nextInt (std::max (1, blockSize))) : std::min (blockSize, rendered - pos);
        const double t = (double) pos / sr;
        for (const auto& pc : programChanges)
        {
            const int at = (int) (pc.time * sr);
            if (at >= pos && at < pos + n)
                for (auto& in : fx)
                    in.proc->setCurrentProgram (pc.index);
        }
        for (const auto& a : automation)
            if (t >= a.t0 - (double) n / sr)
            {
                const double f = a.t1 > a.t0 ? std::clamp ((t - a.t0) / (a.t1 - a.t0), 0.0, 1.0) : 1.0;
                if (a.t1 > a.t0 || (a.t0 >= t && a.t0 < t + (double) n / sr))
                    for (size_t j = 0; j < fx.size(); ++j)
                        if (a.instance < 0 || a.instance == (int) j)
                            setParam (*fx[j].proc, fx[j].fx, a.id + "=" + juce::String (a.a + (a.b - a.a) * (float) f));
            }
        juce::MidiBuffer midi;
        for (const auto& e : midiEvents)
        {
            const int on = (int) std::llround (e.start * sr), off = (int) std::llround (e.end * sr);
            if (on >= pos && on < pos + n)
                midi.addEvent (juce::MidiMessage::noteOn (1, e.note, (juce::uint8) e.velocity), on - pos);
            if (off >= pos && off < pos + n)
                midi.addEvent (juce::MidiMessage::noteOff (1, e.note), off - pos);
        }
        for (size_t k = 0; k < fx.size(); ++k)
        {
            auto& in = fx[k];
            block.setSize (channels, n, false, false, true);
            block.clear();
            for (int ch = 0; ch < 2; ++ch)
                block.copyFrom (ch, 0, input, ch, pos, n);
            if (sideSpec.isNotEmpty())
                for (int ch = 2; ch < channels; ++ch)
                    block.copyFrom (ch, 0, side, (ch - 2) % 2, pos, n);
            in.head->position = pos;
            juce::MidiBuffer m (midi);
            in.proc->processBlock (block, m);
            for (int ch = 0; ch < 2; ++ch)
                outputs[k].copyFrom (ch, pos, block, ch, 0, n);
        }
        pos += n;
    }
    const double cpu = threadCpuSeconds() - cpu0;

    // Trim the latency so the file lines up with the input (unless --no-trim).
    const int trim = args.contains ("--no-trim") ? 0 : latency;
    Stats stats;
    for (size_t k = 0; k < fx.size(); ++k)
    {
        juce::AudioBuffer<float> out (2, total);
        for (int ch = 0; ch < 2; ++ch)
            out.copyFrom (ch, 0, outputs[k], ch, trim, total);
        if (k == 0)
            stats.add (out, 0, total);
        juce::File file (option (args, "--out"));
        if (instances > 1)
            file = file.getSiblingFile (file.getFileNameWithoutExtension() + "_" + juce::String ((int) k) + file.getFileExtension());
        if (args.contains ("--out") && ! writeWav (file, out, sr))
        {
            std::cerr << "cannot write " << file.getFullPathName() << std::endl;
            return 1;
        }
        if (instances > 1)
        {
            std::cout << "instance_" << k << "_peers=" << fx[k].fx->meters.peers.load() << " instance_" << k << "_callsign="
                      << fx[k].fx->getCallsign() << " instance_" << k << "_faults=" << fx[k].fx->meters.faults.load();
            for (int c = 0; c < (int) fx[k].fx->meters.custom.size(); ++c)
                std::cout << " instance_" << k << "_custom" << c << "=" << fx[k].fx->meters.custom[(size_t) c].load();
            std::cout << std::endl;
        }
    }

    if (nullCheck)
    {
        // Bypass On: the output is the input delayed by the reported latency (compare after the 20 ms crossfade).
        float worst = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = (int) (0.05 * sr); i < total; ++i)
                worst = std::max (worst, std::abs (outputs[0].getSample (ch, i + trim) - input.getSample (ch, i + trim - latency)));
        std::cout << "null_max_diff=" << worst << std::endl;
    }
    if (latencyCheck)
    {
        // Impulse at 0.1 s; the output's largest sample is where it came out (untrimmed timeline).
        int at = -1;
        float best = 0.0f;
        for (int i = 0; i < rendered; ++i)
        {
            const float v = std::abs (outputs[0].getSample (0, i)) + std::abs (outputs[0].getSample (1, i));
            if (v > best) { best = v; at = i; }
        }
        std::cout << "latency_reported=" << latency << " latency_measured=" << (at >= 0 ? at - (int) (0.1 * sr) : -1) << std::endl;
    }

    std::cout << "product=\"" << fx[0].proc->getName() << "\" latency=" << latency << " tail_s=" << fx[0].proc->getTailLengthSeconds()
              << " seconds=" << seconds
              << " peak_db=" << juce::Decibels::gainToDecibels (stats.peak, -240.0f) << " rms_db=" << stats.rmsDb()
              << " max_jump=" << stats.maxJump << " non_finite=" << stats.nonFinite << " faults=" << first->meters.faults.load()
              << " faults_logged=" << first->flushFaultLog() << " realtime_x=" << (cpu > 0.0 ? seconds * instances / cpu : 0.0);
   #if LABX11_HARNESS
    for (size_t s = 0; s < first->info.stages.size() && s < first->stagePeak.size(); ++s)
        std::cout << " stage_" << first->info.stages[s] << "_peak_db=" << juce::Decibels::gainToDecibels (first->stagePeak[s], -240.0f);
   #endif
    for (int i = 0; i < (int) first->meters.custom.size(); ++i)
        std::cout << " custom" << i << "=" << first->meters.custom[(size_t) i].load();
    std::cout << " peers=" << first->meters.peers.load() << std::endl;
    for (auto& in : fx)
        in.proc->releaseResources();
    return stats.nonFinite == 0 ? 0 : 1;
}
