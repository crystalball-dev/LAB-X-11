#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// lab::ui: one UI engine, two faces. A Theme value drives one LookAndFeel, the widgets and the overlays; no effect
// carries palette code of its own.
namespace lab::ui
{
    enum class Faction { deepZone, etherealSignal };

    struct Theme
    {
        const char* name;
        Faction faction;
        juce::Colour background, panel, border, faint, dim, phosphor, bright, accent, knobTop, knobBottom, meterTint;
        float cornerRadius, borderWeight;
        enum KnobStyle { oxidized, vacuum } knob;
        struct Needle { float dampingSeconds, jitterHz, jitterDepth; } needle;   // erratic for A, damped for B
        enum Overlay { scanlines, vacuumGlow } overlay;
        float flicker, grain;

        bool isDeepZone() const noexcept { return faction == Faction::deepZone; }
    };

    // Faction A, THE DEEP ZONE: oxidized hazard instrumentation, hazard yellow phosphor, red warnings, erratic needles.
    inline const Theme& oxideTheme()
    {
        static const Theme t {
            "Oxide", Faction::deepZone,
            juce::Colour (0xff0b0907), juce::Colour (0xff16110c), juce::Colour (0xff4a3620), juce::Colour (0xff23190f),
            juce::Colour (0xffa06a2a), juce::Colour (0xffe0b23a), juce::Colour (0xfff6e7b8), juce::Colour (0xffff4a2a),
            juce::Colour (0xff2a1d10), juce::Colour (0xff0b0907), juce::Colour (0xffe0b23a),
            2.0f, 1.0f,
            Theme::oxidized,
            { 0.08f, 7.0f, 0.15f },
            Theme::scanlines, 0.02f, 0.35f
        };
        return t;
    }

    // Faction B, THE ETHEREAL SIGNAL: sterile laboratory, vacuum-display ice, indicator lamps, damped needles.
    inline const Theme& vacuumTheme()
    {
        static const Theme t {
            "Vacuum", Faction::etherealSignal,
            juce::Colour (0xff040709), juce::Colour (0xff0a1116), juce::Colour (0xff1b3540), juce::Colour (0xff0f1e26),
            juce::Colour (0xff4d97a6), juce::Colour (0xffb9f6ff), juce::Colour (0xfff4fdff), juce::Colour (0xffffd36b),
            juce::Colour (0xff102028), juce::Colour (0xff040709), juce::Colour (0xffb9f6ff),
            5.0f, 1.0f,
            Theme::vacuum,
            { 0.35f, 0.0f, 0.0f },
            Theme::vacuumGlow, 0.0f, 0.0f
        };
        return t;
    }

    inline const Theme& themeFor (Faction f) { return f == Faction::deepZone ? oxideTheme() : vacuumTheme(); }

    inline juce::Font monoFont (float height, bool bold = false)
    {
        return juce::Font (juce::FontOptions ("Consolas", height, bold ? juce::Font::bold : juce::Font::plain));
    }

    inline juce::String utf8 (const char* text) { return juce::String::fromUTF8 (text); }

    // Faction marks, drawn as paths (no image files): the radiation trefoil for A, an antenna over a sine for B.
    inline juce::Path factionGlyph (Faction f, juce::Rectangle<float> r)
    {
        juce::Path p;
        const auto c = r.getCentre();
        const float s = std::min (r.getWidth(), r.getHeight()) * 0.5f;
        if (f == Faction::deepZone)
        {
            p.addEllipse (c.x - s * 0.16f, c.y - s * 0.16f, s * 0.32f, s * 0.32f);
            for (int k = 0; k < 3; ++k)
            {
                const float a0 = juce::MathConstants<float>::twoPi * (float) k / 3.0f - juce::MathConstants<float>::pi / 6.0f;
                juce::Path blade;
                blade.addPieSegment (c.x - s, c.y - s, s * 2.0f, s * 2.0f, a0, a0 + juce::MathConstants<float>::pi / 3.0f, 0.3f);
                p.addPath (blade);
            }
        }
        else
        {
            p.startNewSubPath (c.x, c.y + s);
            p.lineTo (c.x, c.y - s * 0.55f);
            p.addEllipse (c.x - s * 0.12f, c.y - s * 0.75f, s * 0.24f, s * 0.24f);
            juce::Path wave;
            for (int i = 0; i <= 24; ++i)
            {
                const float t = (float) i / 24.0f;
                const float x = c.x - s + 2.0f * s * t;
                const float y = c.y + s * 0.45f + std::sin (t * juce::MathConstants<float>::twoPi * 2.0f) * s * 0.18f;
                if (i == 0) wave.startNewSubPath (x, y); else wave.lineTo (x, y);
            }
            p.addPath (wave);
        }
        return p;
    }
}
