#pragma once

#include "Theme.h"

namespace lab::ui
{
    // One LookAndFeel for the whole suite (from LAB X-0's CrtLookAndFeel), reading a Theme instead of a static palette.
    class ThemedLookAndFeel final : public juce::LookAndFeel_V4
    {
    public:
        explicit ThemedLookAndFeel (const Theme& t) { setTheme (t); }

        const Theme& getTheme() const noexcept { return *theme; }

        void setTheme (const Theme& t)
        {
            theme = &t;
            setColour (juce::ResizableWindow::backgroundColourId, t.background);
            setColour (juce::Label::textColourId, t.dim);
            setColour (juce::Slider::textBoxTextColourId, t.phosphor);
            setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
            setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
            setColour (juce::Slider::textBoxHighlightColourId, t.dim.withAlpha (0.5f));
            setColour (juce::ComboBox::backgroundColourId, t.panel);
            setColour (juce::ComboBox::textColourId, t.phosphor);
            setColour (juce::ComboBox::outlineColourId, t.border);
            setColour (juce::ComboBox::arrowColourId, t.phosphor);
            setColour (juce::PopupMenu::backgroundColourId, t.background);
            setColour (juce::PopupMenu::textColourId, t.phosphor);
            setColour (juce::PopupMenu::headerTextColourId, t.accent);
            setColour (juce::PopupMenu::highlightedBackgroundColourId, t.dim);
            setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::black);
            setColour (juce::TextButton::buttonColourId, t.panel);
            setColour (juce::TextButton::buttonOnColourId, t.dim);
            setColour (juce::TextButton::textColourOffId, t.phosphor);
            setColour (juce::TextButton::textColourOnId, t.bright);
            setColour (juce::TextEditor::backgroundColourId, t.background);
            setColour (juce::TextEditor::textColourId, t.bright);
            setColour (juce::TextEditor::highlightColourId, t.dim.withAlpha (0.5f));
            setColour (juce::TextEditor::outlineColourId, t.border);
            setColour (juce::TextEditor::focusedOutlineColourId, t.dim);
            setColour (juce::CaretComponent::caretColourId, t.phosphor);
            setColour (juce::ListBox::backgroundColourId, t.background);
            setColour (juce::ListBox::outlineColourId, t.border);
            setColour (juce::ScrollBar::thumbColourId, t.dim.withAlpha (0.6f));
            setColour (juce::ScrollBar::trackColourId, t.faint);
            setColour (juce::TooltipWindow::backgroundColourId, t.background);
            setColour (juce::TooltipWindow::textColourId, t.phosphor);
            setColour (juce::TooltipWindow::outlineColourId, t.border);
        }

        void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float pos,
                               float startAngle, float endAngle, juce::Slider& slider) override
        {
            const auto& t = *theme;
            const bool accent = slider.getProperties().contains ("accent");
            const bool bipolar = slider.getProperties().contains ("bipolar");
            const auto colour = accent ? t.accent : t.phosphor;

            const auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height).reduced (3.0f);
            const float radius = std::min (bounds.getWidth(), bounds.getHeight()) * 0.5f;
            const auto centre = bounds.getCentre();
            const float angle = startAngle + pos * (endAngle - startAngle);
            const float lineW = std::max (2.0f, radius * 0.1f);
            const float arcR = radius - lineW;
            const juce::PathStrokeType stroke (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

            juce::Path track;
            track.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, startAngle, endAngle, true);
            g.setColour (t.faint);
            g.strokePath (track, stroke);

            // Oxidized knobs carry tick marks like a stamped scale; vacuum knobs glow.
            if (t.knob == Theme::oxidized)
            {
                g.setColour (t.border);
                for (int k = 0; k <= 10; ++k)
                {
                    const float a = startAngle + (endAngle - startAngle) * (float) k / 10.0f;
                    const auto p0 = centre.getPointOnCircumference (arcR + lineW * 0.9f, a);
                    const auto p1 = centre.getPointOnCircumference (arcR + lineW * 1.6f, a);
                    g.drawLine ({ p0, p1 }, 1.0f);
                }
            }

            const float from = bipolar ? (startAngle + endAngle) * 0.5f : startAngle;
            if (std::abs (angle - from) > 0.001f)
            {
                juce::Path value;
                value.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, std::min (from, angle), std::max (from, angle), true);
                g.setColour (colour.withAlpha (t.knob == Theme::vacuum ? 0.24f : 0.14f));
                g.strokePath (value, juce::PathStrokeType (lineW * (t.knob == Theme::vacuum ? 4.0f : 3.0f),
                                                           juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                g.setColour (colour);
                g.strokePath (value, stroke);
            }

            const float bodyR = arcR - lineW * 1.7f;
            if (bodyR > 2.0f)
            {
                juce::ColourGradient grad (t.knobTop, centre.x, centre.y - bodyR, t.knobBottom, centre.x, centre.y + bodyR, false);
                g.setGradientFill (grad);
                g.fillEllipse (centre.x - bodyR, centre.y - bodyR, bodyR * 2.0f, bodyR * 2.0f);
                g.setColour (t.border);
                g.drawEllipse (centre.x - bodyR, centre.y - bodyR, bodyR * 2.0f, bodyR * 2.0f, 1.0f);

                juce::Path pointer;
                pointer.addRoundedRectangle (-lineW * 0.4f, -bodyR + 1.0f, lineW * 0.8f, bodyR * 0.6f, lineW * 0.3f);
                pointer.applyTransform (juce::AffineTransform::rotation (angle).translated (centre.x, centre.y));
                g.setColour (colour);
                g.fillPath (pointer);
            }
        }

        void drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box) override
        {
            const auto& t = *theme;
            const auto r = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (0.5f);
            g.setColour (t.panel);
            g.fillRoundedRectangle (r, t.cornerRadius);
            g.setColour (box.hasKeyboardFocus (true) ? t.dim : t.border);
            g.drawRoundedRectangle (r, t.cornerRadius, t.borderWeight);
            juce::Path arrow;
            const float ax = (float) width - 9.0f, ay = (float) height * 0.5f;
            arrow.addTriangle (ax - 3.5f, ay - 2.0f, ax + 3.5f, ay - 2.0f, ax, ay + 3.0f);
            g.setColour (t.phosphor);
            g.fillPath (arrow);
        }

        void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
        {
            label.setBounds (3, 1, box.getWidth() - 14, box.getHeight() - 2);
            label.setFont (getComboBoxFont (box));
            label.setMinimumHorizontalScale (0.6f);
        }

        void drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour&, bool highlighted, bool down) override
        {
            const auto& t = *theme;
            const auto r = button.getLocalBounds().toFloat().reduced (0.5f);
            const bool latched = button.getToggleState();
            const bool accent = button.getProperties().contains ("accent");
            const auto on = accent ? t.accent : t.dim;
            auto fill = down ? on.withAlpha (0.45f) : (highlighted ? t.faint.brighter (0.25f) : t.panel);
            if (latched)
                fill = on.withAlpha (highlighted ? 0.5f : 0.36f);
            g.setColour (fill);
            g.fillRoundedRectangle (r, t.cornerRadius);
            g.setColour (latched ? (accent ? t.accent : t.phosphor).withAlpha (0.75f) : (highlighted ? t.dim : t.border));
            g.drawRoundedRectangle (r, t.cornerRadius, t.borderWeight);
        }

        void drawButtonText (juce::Graphics& g, juce::TextButton& button, bool, bool) override
        {
            g.setFont (getTextButtonFont (button, button.getHeight()));
            const bool latched = button.getToggleState();
            g.setColour (button.findColour (latched ? juce::TextButton::textColourOnId : juce::TextButton::textColourOffId)
                             .withMultipliedAlpha (button.isEnabled() ? 1.0f : 0.4f));
            g.drawFittedText (button.getButtonText(), button.getLocalBounds().reduced (2, 0), juce::Justification::centred, 1, 0.7f);
        }

        juce::Font getLabelFont (juce::Label&) override                   { return monoFont (11.0f); }
        juce::Font getComboBoxFont (juce::ComboBox&) override             { return monoFont (11.5f); }
        juce::Font getPopupMenuFont() override                            { return monoFont (13.0f); }
        juce::Font getTextButtonFont (juce::TextButton&, int h) override  { return monoFont (std::min (10.5f, (float) h * 0.62f), true); }

        juce::Label* createSliderTextBox (juce::Slider& slider) override
        {
            const auto& t = *theme;
            auto* label = juce::LookAndFeel_V4::createSliderTextBox (slider);
            label->setFont (monoFont (10.5f));
            label->setMinimumHorizontalScale (0.7f);
            label->setColour (juce::Label::textColourId, slider.getProperties().contains ("accent") ? t.accent : t.phosphor);
            label->setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
            label->setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
            label->setColour (juce::TextEditor::backgroundColourId, t.background);
            label->setColour (juce::TextEditor::textColourId, t.phosphor);
            return label;
        }

    private:
        const Theme* theme = nullptr;
    };
}
