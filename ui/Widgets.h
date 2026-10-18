#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Theme.h"

namespace lab::ui
{
    using APVTS = juce::AudioProcessorValueTreeState;

    // Every widget reads the editor's Theme through a pointer, so the stub can switch faces at run time.
    struct Themed
    {
        virtual ~Themed() = default;
        const Theme* theme = &oxideTheme();
        virtual void setTheme (const Theme& t) { theme = &t; }
    };

    // Rotary control with a caption above and its value below (LAB X-19 / X-0).
    class LabeledKnob final : public juce::Component, public Themed
    {
    public:
        LabeledKnob (APVTS& s, const juce::String& paramID, const juce::String& caption, bool accent = false)
            : state (s), isAccent (accent)
        {
            slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 64, 14);
            slider.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
            if (accent)
                slider.getProperties().set ("accent", true);
            label.setText (caption, juce::dontSendNotification);
            label.setJustificationType (juce::Justification::centred);
            label.setInterceptsMouseClicks (false, false);
            label.setMinimumHorizontalScale (0.6f);
            addAndMakeVisible (label);
            addAndMakeVisible (slider);
            if (auto* param = state.getParameter (paramID))
            {
                const auto range = param->getNormalisableRange();
                if (range.start < 0.0f && range.end > 0.0f)
                    slider.getProperties().set ("bipolar", true);
                slider.setTooltip (param->getName (64));
                attachment = std::make_unique<APVTS::SliderAttachment> (state, paramID, slider);
                slider.setDoubleClickReturnValue (true, param->convertFrom0to1 (param->getDefaultValue()));
            }
            setTheme (*theme);
        }

        void setTheme (const Theme& t) override
        {
            Themed::setTheme (t);
            label.setColour (juce::Label::textColourId, isAccent ? t.accent : t.dim);
            slider.repaint();
        }

        void resized() override
        {
            auto b = getLocalBounds();
            label.setBounds (b.removeFromTop (13));
            slider.setBounds (b);
        }

        juce::Slider slider;
        juce::Label label;

    private:
        APVTS& state;
        bool isAccent = false;
        std::unique_ptr<APVTS::SliderAttachment> attachment;
    };

    // Choice parameter as a combo box with a caption above.
    class LabeledChoice final : public juce::Component, public Themed
    {
    public:
        LabeledChoice (APVTS& s, const juce::String& paramID, const juce::String& caption)
        {
            label.setText (caption, juce::dontSendNotification);
            label.setJustificationType (juce::Justification::centredLeft);
            label.setInterceptsMouseClicks (false, false);
            addAndMakeVisible (label);
            addAndMakeVisible (box);
            if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (s.getParameter (paramID)))
            {
                box.addItemList (choice->choices, 1);
                box.setTooltip (choice->getName (64));
                attachment = std::make_unique<APVTS::ComboBoxAttachment> (s, paramID, box);
            }
            setTheme (*theme);
        }

        void setTheme (const Theme& t) override
        {
            Themed::setTheme (t);
            label.setColour (juce::Label::textColourId, t.dim);
            box.repaint();
        }

        void resized() override
        {
            auto b = getLocalBounds();
            label.setBounds (b.removeFromTop (13));
            b.removeFromTop (2);
            box.setBounds (b.removeFromTop (22));
        }

        static constexpr int naturalHeight = 37;
        juce::ComboBox box;
        juce::Label label;

    private:
        std::unique_ptr<APVTS::ComboBoxAttachment> attachment;
    };

    // Up to three choices stacked in one column.
    class ChoiceStack final : public juce::Component, public Themed
    {
    public:
        void add (std::unique_ptr<LabeledChoice> c)
        {
            addAndMakeVisible (*c);
            items.push_back (std::move (c));
        }

        void setTheme (const Theme& t) override
        {
            Themed::setTheme (t);
            for (auto& c : items)
                c->setTheme (t);
        }

        void resized() override
        {
            auto b = getLocalBounds().reduced (3, 0);
            const int h = LabeledChoice::naturalHeight;
            const int count = (int) items.size();
            if (count == 0)
                return;
            const int step = std::max (h, b.getHeight() / count);
            const int y0 = b.getY() + std::max (0, (b.getHeight() - (count - 1) * step - h) / 2);
            for (int i = 0; i < count; ++i)
                items[(size_t) i]->setBounds (b.getX(), y0 + i * step, b.getWidth(), h);
        }

    private:
        std::vector<std::unique_ptr<LabeledChoice>> items;
    };

    // Phosphor oscilloscope with a rising-edge trigger and a short persistence trail (LAB X-19 / X-0).
    class ScopeView final : public juce::Component, public Themed
    {
    public:
        static constexpr int points = 384;

        void setSamples (const float* source, int numSource)
        {
            int start = 0;
            const int searchEnd = std::max (0, numSource - points * 2);
            for (int i = 1; i < searchEnd; ++i)
                if (source[i - 1] <= 0.0f && source[i] > 0.0f)
                {
                    start = i;
                    break;
                }
            history[2] = history[1];
            history[1] = history[0];
            for (int i = 0; i < points; ++i)
            {
                const int idx = start + i * 2;
                history[0][(size_t) i] = idx < numSource ? source[idx] : 0.0f;
            }
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            const auto& t = *theme;
            const auto b = getLocalBounds().toFloat();
            g.setColour (t.background);
            g.fillRoundedRectangle (b, t.cornerRadius);
            g.setColour (t.faint);
            g.drawHorizontalLine ((int) b.getCentreY(), b.getX() + 2.0f, b.getRight() - 2.0f);
            for (int k = 2; k >= 0; --k)
            {
                juce::Path p;
                for (int i = 0; i < points; ++i)
                {
                    const float x = b.getX() + b.getWidth() * (float) i / (float) (points - 1);
                    const float y = b.getCentreY() - std::clamp (history[(size_t) k][(size_t) i], -1.0f, 1.0f) * b.getHeight() * 0.45f;
                    if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
                }
                g.setColour (t.phosphor.withAlpha (k == 0 ? 0.9f : 0.22f / (float) k));
                g.strokePath (p, juce::PathStrokeType (k == 0 ? 1.4f : 1.0f));
            }
        }

    private:
        std::array<std::array<float, points>, 3> history {};
    };

    // An analogue needle on an arc (LAB X-3 RadiationMeter). The theme sets the ballistics: Faction A jitters and
    // overshoots, Faction B glides. Values 0..1; above `redFrom` the arc is red.
    class Needle final : public juce::Component, public Themed
    {
    public:
        explicit Needle (juce::String captionText = {}, float redFromValue = 0.8f)
            : caption (std::move (captionText)), redFrom (redFromValue) {}

        // Called at the editor's frame rate with the elapsed seconds since the last call.
        void advance (float target, float seconds, float extraJitter = 0.0f)
        {
            const auto& n = theme->needle;
            const float coef = 1.0f - std::exp (-seconds / std::max (0.01f, n.dampingSeconds));
            value += (std::clamp (target, 0.0f, 1.2f) - value) * coef;
            phase += seconds * (n.jitterHz > 0.0f ? n.jitterHz : 0.0f);
            const float jitter = (n.jitterDepth + extraJitter) * 0.04f
                                 * (std::sin (phase * 6.2832f) + 0.5f * std::sin (phase * 17.3f + 1.3f));
            shown = std::clamp (value + (n.jitterHz > 0.0f ? jitter : 0.0f), 0.0f, 1.1f);
            repaint();
        }

        void setReadout (const juce::String& text) { readout = text; }

        void paint (juce::Graphics& g) override
        {
            const auto& t = *theme;
            auto b = getLocalBounds().toFloat().reduced (4.0f);
            const auto textArea = b.removeFromBottom (14.0f);
            // A 224-degree face whose lower ends stay above the readout line.
            const float span = juce::MathConstants<float>::pi * 0.62f;
            const float below = -std::cos (span);   // how far the arc ends dip under the centre, in radii
            const float radius = std::max (8.0f, std::min (b.getWidth() * 0.5f - 2.0f, (b.getHeight() - 4.0f) / (1.0f + below)));
            const auto centre = juce::Point<float> (b.getCentreX(), b.getBottom() - 2.0f - radius * below);
            const float from = -span, to = span;

            juce::Path arc, red;
            arc.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, from, from + (to - from) * redFrom, true);
            red.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, from + (to - from) * redFrom, to, true);
            g.setColour (t.dim.withAlpha (0.7f));
            g.strokePath (arc, juce::PathStrokeType (2.0f));
            g.setColour (t.accent.withAlpha (0.85f));
            g.strokePath (red, juce::PathStrokeType (3.0f));
            for (int k = 0; k <= 10; ++k)
            {
                const float a = from + (to - from) * (float) k / 10.0f;
                g.setColour (a > from + (to - from) * redFrom ? t.accent : t.dim);
                g.drawLine ({ centre.getPointOnCircumference (radius - 6.0f, a), centre.getPointOnCircumference (radius, a) }, 1.0f);
            }
            const float a = from + (to - from) * std::clamp (shown, 0.0f, 1.05f);
            if (t.overlay == Theme::vacuumGlow)
            {
                g.setColour (t.phosphor.withAlpha (0.18f));
                g.drawLine ({ centre, centre.getPointOnCircumference (radius - 3.0f, a) }, 5.0f);
            }
            g.setColour (shown > redFrom ? t.accent : t.bright);
            g.drawLine ({ centre, centre.getPointOnCircumference (radius - 3.0f, a) }, 1.6f);
            g.fillEllipse (centre.x - 3.0f, centre.y - 3.0f, 6.0f, 6.0f);

            g.setFont (monoFont (10.0f, true));
            g.setColour (t.phosphor);
            g.drawText (caption, textArea, juce::Justification::centredLeft);
            g.drawText (readout, textArea, juce::Justification::centredRight);
        }

    private:
        juce::String caption, readout;
        float redFrom = 0.8f, value = 0.0f, shown = 0.0f, phase = 0.0f;
    };

    // Horizontal level bar in dB (-60..0).
    class LevelBar final : public juce::Component, public Themed
    {
    public:
        void setLevel (float db) { level = db; repaint(); }

        void paint (juce::Graphics& g) override
        {
            const auto& t = *theme;
            const auto b = getLocalBounds().toFloat();
            g.setColour (t.faint);
            g.fillRoundedRectangle (b, 2.0f);
            const float frac = std::clamp ((level + 60.0f) / 60.0f, 0.0f, 1.0f);
            g.setColour (level > -1.0f ? t.accent : t.meterTint.withAlpha (0.85f));
            g.fillRoundedRectangle (b.withWidth (b.getWidth() * frac), 2.0f);
        }

    private:
        float level = -100.0f;
    };

    // Faction overlay over the whole editor: scanlines, vignette, rust speckle and a faint flicker for A; a fine grid
    // and a soft glow for B. Never takes clicks.
    class Overlay final : public juce::Component, public Themed
    {
    public:
        Overlay() { setInterceptsMouseClicks (false, false); }

        void setTheme (const Theme& t) override
        {
            Themed::setTheme (t);
            speckle = {};
            repaint();
        }

        void setFlicker (float f) { flicker = f; repaint(); }

        void paint (juce::Graphics& g) override
        {
            const auto& t = *theme;
            const auto b = getLocalBounds().toFloat();
            if (t.overlay == Theme::scanlines)
            {
                if (speckle.isNull() || speckle.getWidth() != getWidth() || speckle.getHeight() != getHeight())
                    buildSpeckle();
                g.drawImageAt (speckle, 0, 0);
                juce::Image lines (juce::Image::ARGB, 4, 3, true);
                for (int x = 0; x < 4; ++x)
                    lines.setPixelAt (x, 2, juce::Colour (0x26000000));
                g.setFillType (juce::FillType (lines, juce::AffineTransform()));
                g.fillRect (b);
                juce::ColourGradient vignette (juce::Colours::transparentBlack, b.getCentreX(), b.getCentreY(),
                                               juce::Colour (0x66000000), b.getX(), b.getY(), true);
                g.setGradientFill (vignette);
                g.fillRect (b);
                if (flicker > 0.0f)
                {
                    g.setColour (juce::Colours::black.withAlpha (flicker));
                    g.fillRect (b);
                }
            }
            else
            {
                g.setColour (t.phosphor.withAlpha (0.025f));
                for (float x = 0.0f; x < b.getWidth(); x += 12.0f)
                    g.drawVerticalLine ((int) x, 0.0f, b.getHeight());
                for (float y = 0.0f; y < b.getHeight(); y += 12.0f)
                    g.drawHorizontalLine ((int) y, 0.0f, b.getWidth());
                juce::ColourGradient glow (t.phosphor.withAlpha (0.045f), b.getCentreX(), b.getY() + b.getHeight() * 0.3f,
                                           juce::Colours::transparentBlack, b.getX(), b.getBottom(), true);
                g.setGradientFill (glow);
                g.fillRect (b);
            }
        }

    private:
        // Procedural rust: seeded noise, drawn once into an image.
        void buildSpeckle()
        {
            speckle = juce::Image (juce::Image::ARGB, std::max (1, getWidth()), std::max (1, getHeight()), true);
            juce::Graphics g (speckle);
            juce::Random rng (0x16);
            const auto& t = *theme;
            const int count = (getWidth() * getHeight()) / 90;
            for (int i = 0; i < count; ++i)
            {
                const float x = rng.nextFloat() * (float) getWidth(), y = rng.nextFloat() * (float) getHeight();
                const float s = 0.6f + rng.nextFloat() * 1.6f;
                g.setColour ((rng.nextBool() ? t.dim : juce::Colour (0xff5a2a12)).withAlpha (0.03f + 0.05f * rng.nextFloat() * t.grain));
                g.fillEllipse (x, y, s, s);
            }
        }

        juce::Image speckle;
        float flicker = 0.0f;
    };

    inline void styleButton (juce::TextButton& b, const juce::String& tooltip, bool toggle = false)
    {
        b.setTooltip (tooltip);
        b.setClickingTogglesState (toggle);
        b.setWantsKeyboardFocus (false);
    }
}
