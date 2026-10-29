#pragma once

#include <vector>
#include "Widgets.h"

// Generic displays for the effects' custom panels. Each reads values the editor copies from the chain's atomics once
// a frame; none touches the audio thread.
namespace lab::ui
{
    // N bars, 0..1 (or -1..1 when bipolar), an optional highlighted bar and labels under them (100 RADS' wealth, SNORK's
    // fitness and genome, ARTIFACT's lattice, C-CONSCIOUSNESS's pitch classes).
    class BarsView final : public juce::Component, public Themed
    {
    public:
        void setValues (const float* v, int n)
        {
            values.assign (v, v + n);
            repaint();
        }
        void setHighlight (int index) { highlight = index; }
        void setSecondary (int index) { secondary = index; }
        void setLabels (juce::StringArray l) { labels = std::move (l); }
        void setBipolar (bool b) { bipolar = b; }
        void setCaption (const juce::String& c) { caption = c; }

        void paint (juce::Graphics& g) override
        {
            const auto& t = *theme;
            auto b = getLocalBounds().toFloat();
            if (caption.isNotEmpty())
            {
                g.setFont (monoFont (10.0f, true));
                g.setColour (t.phosphor);
                g.drawText (caption, b.removeFromTop (14.0f), juce::Justification::centredLeft);
            }
            const bool labelled = labels.size() > 0;
            auto labelRow = labelled ? b.removeFromBottom (12.0f) : juce::Rectangle<float>();
            g.setColour (t.background);
            g.fillRoundedRectangle (b, t.cornerRadius);
            const int n = (int) values.size();
            if (n == 0)
                return;
            const float w = b.getWidth() / (float) n;
            const float base = bipolar ? b.getCentreY() : b.getBottom() - 1.0f;
            for (int i = 0; i < n; ++i)
            {
                const float v = std::clamp (values[(size_t) i], bipolar ? -1.0f : 0.0f, 1.0f);
                const float h = (bipolar ? 0.5f : 1.0f) * (b.getHeight() - 2.0f) * std::abs (v);
                auto bar = juce::Rectangle<float> (b.getX() + w * (float) i + 1.0f, v >= 0.0f ? base - h : base, std::max (1.0f, w - 2.0f), h);
                const auto colour = i == highlight ? t.accent : (i == secondary ? t.bright : t.meterTint.withAlpha (0.75f));
                if (t.overlay == Theme::vacuumGlow)
                {
                    g.setColour (colour.withAlpha (0.18f));
                    g.fillRect (bar.expanded (1.5f, 0.0f));
                }
                g.setColour (colour);
                g.fillRect (bar);
                if (labelled && i < labels.size())
                {
                    g.setFont (monoFont (9.0f));
                    g.setColour (i == highlight ? t.accent : t.dim);
                    g.drawText (labels[i], juce::Rectangle<float> (b.getX() + w * (float) i, labelRow.getY(), w, labelRow.getHeight()),
                                juce::Justification::centred);
                }
            }
        }

    private:
        std::vector<float> values;
        juce::StringArray labels;
        juce::String caption;
        int highlight = -1, secondary = -1;
        bool bipolar = false;
    };

    // An N x N grid of glowing cells, -1..1 (C-CONSCIOUSNESS's mixing matrix).
    class MatrixView final : public juce::Component, public Themed
    {
    public:
        void setValues (const float* v, int size)
        {
            n = size;
            values.assign (v, v + size * size);
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            const auto& t = *theme;
            const auto b = getLocalBounds().toFloat();
            const float side = std::min (b.getWidth(), b.getHeight());
            const auto grid = b.withSizeKeepingCentre (side, side);
            g.setColour (t.background);
            g.fillRect (grid);
            if (n == 0)
                return;
            const float cell = side / (float) n;
            for (int r = 0; r < n; ++r)
                for (int c = 0; c < n; ++c)
                {
                    const float v = std::clamp (values[(size_t) (r * n + c)], -1.0f, 1.0f);
                    const auto colour = v >= 0.0f ? t.phosphor : t.accent;
                    g.setColour (colour.withAlpha (0.08f + 0.85f * std::abs (v)));
                    g.fillRect (grid.getX() + cell * (float) c + 1.0f, grid.getY() + cell * (float) r + 1.0f, cell - 2.0f, cell - 2.0f);
                }
        }

    private:
        std::vector<float> values;
        int n = 0;
    };

    // Points in a box, x and y in -1..1 (y up), each with a size 0..1 and a kind: 0 dim, 1 phosphor, 2 accent, 3 bright
    // (ZEALOTS' flock, POLTERGEIST's objects, CONTAGION's map). Optional axis captions.
    class DotsView final : public juce::Component, public Themed
    {
    public:
        struct Dot { float x = 0.0f, y = 0.0f, size = 0.5f; int kind = 1; juce::String label; };

        void setDots (std::vector<Dot> d) { dots = std::move (d); repaint(); }
        void setAxes (const juce::String& xCaption, const juce::String& yCaption) { xAxis = xCaption; yAxis = yCaption; }
        void setFloorLine (bool b) { floorLine = b; }

        void paint (juce::Graphics& g) override
        {
            const auto& t = *theme;
            auto b = getLocalBounds().toFloat();
            g.setColour (t.background);
            g.fillRoundedRectangle (b, t.cornerRadius);
            g.setColour (t.faint);
            g.drawRoundedRectangle (b.reduced (0.5f), t.cornerRadius, 1.0f);
            if (floorLine)
                g.drawHorizontalLine ((int) (b.getBottom() - 4.0f), b.getX() + 2.0f, b.getRight() - 2.0f);
            else
            {
                g.drawHorizontalLine ((int) b.getCentreY(), b.getX() + 2.0f, b.getRight() - 2.0f);
                g.drawVerticalLine ((int) b.getCentreX(), b.getY() + 2.0f, b.getBottom() - 2.0f);
            }
            g.setFont (monoFont (9.0f));
            g.setColour (t.dim);
            if (xAxis.isNotEmpty())
                g.drawText (xAxis, b.reduced (4.0f), juce::Justification::bottomRight);
            if (yAxis.isNotEmpty())
                g.drawText (yAxis, b.reduced (4.0f), juce::Justification::topLeft);
            const auto inner = b.reduced (6.0f);
            for (const auto& d : dots)
            {
                const float x = inner.getX() + (std::clamp (d.x, -1.0f, 1.0f) * 0.5f + 0.5f) * inner.getWidth();
                const float y = inner.getBottom() - (std::clamp (d.y, -1.0f, 1.0f) * 0.5f + 0.5f) * inner.getHeight();
                const float r = 2.0f + 4.0f * std::clamp (d.size, 0.0f, 1.0f);
                const auto colour = d.kind == 0 ? t.dim : d.kind == 2 ? t.accent : d.kind == 3 ? t.bright : t.phosphor;
                if (t.overlay == Theme::vacuumGlow)
                {
                    g.setColour (colour.withAlpha (0.2f));
                    g.fillEllipse (x - r * 1.8f, y - r * 1.8f, r * 3.6f, r * 3.6f);
                }
                g.setColour (colour);
                g.fillEllipse (x - r, y - r, 2.0f * r, 2.0f * r);
                if (d.label.isNotEmpty())
                    g.drawText (d.label, juce::Rectangle<float> (x + r + 2.0f, y - 6.0f, 80.0f, 12.0f), juce::Justification::centredLeft);
            }
        }

    private:
        std::vector<Dot> dots;
        juce::String xAxis, yAxis;
        bool floorLine = false;
    };

    // Two lines of large text: a caption and a value (EMISSION's countdown, CLEAR SKY's weather, NUMBERS STATION's
    // digits, CONTAGION's stage).
    class BigReadout final : public juce::Component, public Themed
    {
    public:
        void set (const juce::String& captionText, const juce::String& valueText, bool alert = false)
        {
            if (captionText == caption && valueText == value && alert == alarm)
                return;
            caption = captionText;
            value = valueText;
            alarm = alert;
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            const auto& t = *theme;
            auto b = getLocalBounds().toFloat().reduced (2.0f);
            g.setColour (t.background);
            g.fillRoundedRectangle (b, t.cornerRadius);
            b.reduce (6.0f, 4.0f);
            g.setFont (monoFont (10.0f, true));
            g.setColour (t.dim);
            g.drawText (caption, b.removeFromTop (14.0f), juce::Justification::centredLeft);
            const float h = std::clamp (b.getHeight() * 0.7f, 12.0f, 30.0f);
            g.setFont (monoFont (h, true));
            if (t.overlay == Theme::vacuumGlow)
            {
                g.setColour ((alarm ? t.accent : t.phosphor).withAlpha (0.25f));
                g.drawFittedText (value, b.translated (0.0f, 1.0f).toNearestInt(), juce::Justification::centredLeft, 2, 0.6f);
            }
            g.setColour (alarm ? t.accent : t.bright);
            g.drawFittedText (value, b.toNearestInt(), juce::Justification::centredLeft, 2, 0.6f);
        }

    private:
        juce::String caption, value;
        bool alarm = false;
    };
}
