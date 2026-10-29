#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <vector>

namespace lab::host
{
    enum class Kind { real, integer, choice };
    enum class Smooth { none, linear, log };

    // One parameter, as in the siblings (LAB X-19 Params.h). Every parameter is a float or a choice: a bool
    // parameter failed pluginval's state restore in LAB X-3. Ids are append-only once an effect is installed.
    struct ParamSpec
    {
        const char* id;
        const char* name;
        Kind kind;
        float minValue, maxValue, defaultValue, skewCentre;   // skewCentre 0 = linear range
        Smooth smooth;
        const char* unit;      // "%", "Hz", "s", "ms", "dB", "ct", "st", "x", "/s", "oct", ""
        const char* choices;   // '|' separated
    };

    // The common parameters, always first and in this order (handoff 3.4). Bus clients add `bus` fourth.
    namespace Common
    {
        enum Index : int { mix = 0, output, bypass, bus };
    }

    inline std::vector<ParamSpec> commonParams (bool busClient)
    {
        std::vector<ParamSpec> p {
            { "mix",    "Mix",    Kind::real,   0.0f,   1.0f,  1.0f, 0.0f, Smooth::linear, "%",  "" },
            { "output", "Output", Kind::real,   -24.0f, 12.0f, 0.0f, 0.0f, Smooth::linear, "dB", "" },
            { "bypass", "Bypass", Kind::choice, 0.0f,   1.0f,  0.0f, 0.0f, Smooth::none,   "",   "Off|On" },
        };
        if (busClient)
            p.push_back ({ "bus", "Zone Bus", Kind::choice, 0.0f, 1.0f, 1.0f, 0.0f, Smooth::none, "", "Off|On" });
        return p;
    }

    inline juce::StringArray choicesFor (const ParamSpec& s)
    {
        return juce::StringArray::fromTokens (s.choices, "|", "");
    }

    inline juce::NormalisableRange<float> rangeFor (const ParamSpec& s)
    {
        juce::NormalisableRange<float> range (s.minValue, s.maxValue);
        if (s.kind != Kind::real)
            range.interval = 1.0f;
        else if (s.skewCentre > 0.0f)
            range.setSkewForCentre (s.skewCentre);
        return range;
    }

    inline juce::String formatValue (const ParamSpec& s, float v)
    {
        const juce::String unit (s.unit);
        if (unit == "%")   return juce::String (juce::roundToInt (v * 100.0f)) + "%";
        if (unit == "Hz")  return v >= 1000.0f ? juce::String (v / 1000.0f, 2) + " kHz"
                                               : juce::String (v, v < 10.0f ? 2 : (v < 100.0f ? 1 : 0)) + " Hz";
        if (unit == "s")   return v < 1.0f ? juce::String (juce::roundToInt (v * 1000.0f)) + " ms" : juce::String (v, 2) + " s";
        if (unit == "ms")  return juce::String (v, v < 10.0f ? 1 : 0) + " ms";
        if (unit == "dB")  return juce::String (v, 1) + " dB";
        if (unit == "ct")  return juce::String (juce::roundToInt (v)) + " ct";
        if (unit == "st")  return s.kind == Kind::real ? juce::String (v, 1) + " st"
                                                       : (v > 0.0f ? "+" : "") + juce::String (juce::roundToInt (v)) + " st";
        if (unit == "x")   return juce::String (v, v < 10.0f ? 2 : 1) + "x";
        if (unit == "bit") return juce::String (v, 1) + " bit";
        if (unit == "/s")  return juce::String (v, v < 10.0f ? 1 : 0) + "/s";
        if (unit == "oct") return (v > 0.0f ? "+" : "") + juce::String (juce::roundToInt (v));
        if (unit == "min") return juce::String (v, v < 10.0f ? 1 : 0) + " min";
        if (unit == "km")  return juce::String (v, 1) + " km";
        if (unit == "m/s2") return juce::String (v, 1) + " m/s2";
        if (unit == "h")
        {
            const int minutes = juce::roundToInt (v * 60.0f) % (24 * 60);
            return juce::String (minutes / 60).paddedLeft ('0', 2) + ":" + juce::String (minutes % 60).paddedLeft ('0', 2);
        }
        if (s.kind == Kind::real)
            return juce::String (v, std::abs (v) < 10.0f ? 2 : 1);
        return juce::String (juce::roundToInt (v));
    }

    inline float parseValue (const ParamSpec& s, const juce::String& text)
    {
        const auto t = text.trim().toLowerCase();
        float v = t.retainCharacters ("0123456789.-+").getFloatValue();
        const juce::String unit (s.unit);
        if (unit == "%")                           v *= 0.01f;
        else if (unit == "Hz" && t.contains ("k")) v *= 1000.0f;
        else if (unit == "s" && t.contains ("ms")) v *= 0.001f;
        return juce::jlimit (s.minValue, s.maxValue, v);
    }

    // The APVTS layout for a full parameter list. The specs must outlive the processor (they are stored in it).
    inline juce::AudioProcessorValueTreeState::ParameterLayout createLayout (const std::vector<ParamSpec>& specs)
    {
        juce::AudioProcessorValueTreeState::ParameterLayout layout;
        for (const auto& s : specs)
        {
            jassert (s.id != nullptr);
            const juce::ParameterID pid { s.id, 1 };
            const auto* spec = &s;
            switch (s.kind)
            {
                case Kind::choice:
                    layout.add (std::make_unique<juce::AudioParameterChoice> (pid, s.name, choicesFor (s), (int) s.defaultValue));
                    break;
                case Kind::integer:
                    layout.add (std::make_unique<juce::AudioParameterInt> (
                        pid, s.name, (int) s.minValue, (int) s.maxValue, (int) s.defaultValue,
                        juce::AudioParameterIntAttributes()
                            .withStringFromValueFunction ([spec] (int v, int) { return formatValue (*spec, (float) v); })));
                    break;
                case Kind::real:
                    layout.add (std::make_unique<juce::AudioParameterFloat> (
                        pid, s.name, rangeFor (s), s.defaultValue,
                        juce::AudioParameterFloatAttributes()
                            .withStringFromValueFunction ([spec] (float v, int) { return formatValue (*spec, v); })
                            .withValueFromStringFunction ([spec] (const juce::String& text) { return parseValue (*spec, text); })));
                    break;
            }
        }
        return layout;
    }

    // Smoother as in the siblings: linear or log-domain one-pole, 15 ms.
    struct Smoother
    {
        bool enabled = false, logDomain = false;
        float current = 0.0f;

        void reset (float v) noexcept { current = logDomain ? std::log2 (std::max (v, 1.0e-4f)) : v; }

        float advance (float target, float coef) noexcept
        {
            const float t = logDomain ? std::log2 (std::max (target, 1.0e-4f)) : target;
            current += (t - current) * coef;
            return logDomain ? std::exp2 (current) : current;
        }
    };
}
