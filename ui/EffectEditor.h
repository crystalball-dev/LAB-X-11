#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "../core/host/EffectProcessor.h"
#include "LookAndFeel.h"
#include "Widgets.h"

namespace lab::ui
{
    inline Faction toUi (host::Faction f) { return f == host::Faction::deepZone ? Faction::deepZone : Faction::etherealSignal; }

    inline juce::String productForCode (uint32_t code)
    {
        static const std::pair<const char*, const char*> names[] = {
            { "L6Ka", "KAYMANOV" }, { "L6Ps", "PSY-FIELD" }, { "L6Do", "DOSIMETER" }, { "L6Si", "SILO" },
            { "L6An", "ANOMALY" }, { "L6Co", "CONTAGION" }, { "L6Em", "EMISSION" }, { "L6Ap", "APPARITION" },
            { "L6Ze", "ZEALOTS" }, { "L6Rd", "100 RADS" }, { "L6Sa", "SARCOPHAGUS" }, { "L6Po", "POLTERGEIST" },
            { "L6Sn", "SNORK" }, { "L6Ga", "GAUSS" }, { "L6Pd", "PSY-DOG" }, { "L6Rb", "RAINBOW" },
            { "L6Wg", "WISH GRANTER" }, { "L6Cr", "CARRIER" }, { "L6Cc", "C-CONSCIOUSNESS" }, { "L6Ar", "ARTIFACT" },
            { "L6Ns", "NUMBERS STATION" }, { "L6Oa", "OASIS" }, { "L6Cs", "CLEAR SKY" }, { "L6St", "STUB" }, { "L6Sz", "STUB ZERO" }, { "Lb11", "LAB X-11" },
        };
        for (const auto& [c, n] : names)
            if (host::bus::packCode (c) == code)
                return n;
        return "?";
    }

    // PEERS // СВЯЗЬ: the other LAB X instances on the Zone Bus, and this one's callsign (click to edit). Unused in
    // LAB X-11 (busClient = false, decision D2); kept so base fixes port from X-16 line for line.
    class PeersView final : public juce::Component, public Themed
    {
    public:
        explicit PeersView (host::EffectProcessor& p) : processor (p)
        {
            callsignEditor.setFont (monoFont (11.0f, true));
            callsignEditor.setInputRestrictions (8, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-");
            callsignEditor.setText (processor.getCallsign(), false);
            callsignEditor.setTooltip ("This instance's callsign on the Zone Bus (saved with the project)");
            callsignEditor.onReturnKey = callsignEditor.onFocusLost = [this]
            {
                processor.setCallsign (callsignEditor.getText());
                callsignEditor.setText (processor.getCallsign(), false);
            };
            addAndMakeVisible (callsignEditor);
        }

        void refresh()
        {
            count = processor.getBusClient().peers (views.data(), (int) views.size());
            if (! callsignEditor.hasKeyboardFocus (true) && callsignEditor.getText() != processor.getCallsign())
                callsignEditor.setText (processor.getCallsign(), false);
            repaint();
        }

        void setTheme (const Theme& t) override
        {
            Themed::setTheme (t);
            callsignEditor.setColour (juce::TextEditor::backgroundColourId, t.background);
            callsignEditor.setColour (juce::TextEditor::textColourId, t.bright);
            callsignEditor.setColour (juce::TextEditor::outlineColourId, t.border);
            repaint();
        }

        void resized() override { callsignEditor.setBounds (getLocalBounds().removeFromTop (20).removeFromRight (90)); }

        void paint (juce::Graphics& g) override
        {
            const auto& t = *theme;
            auto b = getLocalBounds();
            g.setFont (monoFont (10.0f, true));
            g.setColour (t.dim);
            g.drawText ("CALLSIGN", b.removeFromTop (20), juce::Justification::centredLeft);
            b.removeFromTop (4);
            g.setFont (monoFont (10.5f));
            const bool connected = processor.getBusClient().isConnected();
            const bool busOff = processor.info.busClient && processor.rawValue (host::Common::bus) < 0.5f;
            if (! connected)
            {
                g.setColour (t.accent);
                g.drawText ("PEERS: NO BUS (BRIDGED OR FULL)", b.removeFromTop (16), juce::Justification::centredLeft);
                return;
            }
            g.setColour (busOff ? t.dim : t.phosphor);
            g.drawText (count == 0 ? juce::String ("PEERS: ALONE") : "PEERS: " + juce::String (count) + (busOff ? "  (BUS OFF)" : ""),
                        b.removeFromTop (16), juce::Justification::centredLeft);
            for (int i = 0; i < std::min (count, (int) views.size()) && b.getHeight() >= 14; ++i)
            {
                const auto& v = views[(size_t) i];
                g.setColour (t.bright.withAlpha (0.85f));
                g.drawText (host::bus::unpackCallsign (v.callsign).paddedRight (' ', 9) + productForCode (v.code),
                            b.removeFromTop (14), juce::Justification::centredLeft);
            }
        }

    private:
        host::EffectProcessor& processor;
        juce::TextEditor callsignEditor;
        std::array<host::bus::PeerView, 8> views {};
        int count = 0;
    };

    // The suite's editor: a header (product, subtitle, faction mark, presets, SYSTEM readout), rows of titled panels
    // laid out by units (LAB X-19's engine), and a footer. 960 x 560, fixed. An effect's editor is its rows table plus
    // its custom views.
    class EffectEditor : public juce::AudioProcessorEditor, private juce::Timer
    {
    public:
        static constexpr int editorWidth = 960, editorHeight = 560;

        explicit EffectEditor (host::EffectProcessor& p)
            : AudioProcessorEditor (p), processor (p), theme (&themeFor (toUi (p.info.faction))), laf (*theme)
        {
            setLookAndFeel (&laf);
            for (int i = 0; i < processor.getNumPrograms(); ++i)
                presetBox.addItem (juce::String (i + 1).paddedLeft ('0', 2) + "  " + processor.getProgramName (i), i + 1);
            presetBox.setSelectedId (processor.getCurrentProgram() + 1, juce::dontSendNotification);
            presetBox.onChange = [this]
            {
                const int index = presetBox.getSelectedId() - 1;
                if (index >= 0 && index != processor.getCurrentProgram())
                    processor.setCurrentProgram (index);
            };
            presetBox.setTooltip ("Factory presets (loading one writes every parameter except BYPASS and BUS)");
            styleButton (prevButton, "Previous preset");
            styleButton (nextButton, "Next preset");
            prevButton.onClick = [this] { stepPreset (-1); };
            nextButton.onClick = [this] { stepPreset (1); };
            addAndMakeVisible (presetBox);
            addAndMakeVisible (prevButton);
            addAndMakeVisible (nextButton);
            systemLabel.setFont (monoFont (10.0f));
            systemLabel.setJustificationType (juce::Justification::topRight);
            addAndMakeVisible (systemLabel);
            setSize (editorWidth, editorHeight);
            setResizable (false, false);
        }

        ~EffectEditor() override
        {
            stopTimer();
            setLookAndFeel (nullptr);
        }

        void setTheme (const Theme& t)
        {
            theme = &t;
            laf.setTheme (t);
            for (auto* c : themed)
                c->setTheme (t);
            overlay.setTheme (t);
            systemLabel.setColour (juce::Label::textColourId, t.dim);
            sendLookAndFeelChange();
            repaint();
        }

        const Theme& getTheme() const noexcept { return *theme; }

        // Every panel title, row by row then the side column (test C9 checks them against the spec).
        juce::StringArray panelTitles() const
        {
            juce::StringArray titles;
            for (const auto& row : rows)
                for (const auto& p : row)
                    titles.add (p.title);
            for (const auto& p : side)
                titles.add (p.title);
            return titles;
        }

        // Call once at the end of the derived constructor: adds the overlay and starts the frame timer.
        void finish()
        {
            addAndMakeVisible (overlay);
            setTheme (*theme);
            resized();
            startTimerHz (30);
        }

        // Public so the render harness can draw a frame without a running message loop.
        void refreshFromProcessor()
        {
            const double now = juce::Time::getMillisecondCounterHiRes();
            const float seconds = lastFrame > 0.0 ? (float) std::clamp ((now - lastFrame) * 0.001, 0.0, 0.25) : 1.0f / 30.0f;
            lastFrame = now;

            const auto& m = processor.meters;
            juce::String sys;
            sys << "LATENCY " << m.latency.load() << "  FAULTS " << m.faults.load();
            if (processor.info.busClient)
                sys << "  PEERS " << m.peers.load();
            sys << "\n" << juce::String (m.bpm.load(), 1) << " BPM " << (m.playing.load() ? "PLAYING" : "STOPPED")
                << "  OUT " << juce::String (m.outDb.load(), 1) << " dB";
            if (systemLabel.getText() != sys)
                systemLabel.setText (sys, juce::dontSendNotification);
            if (presetBox.getSelectedId() != processor.getCurrentProgram() + 1)
                presetBox.setSelectedId (processor.getCurrentProgram() + 1, juce::dontSendNotification);
            if (theme->flicker > 0.0f)
                overlay.setFlicker (theme->flicker * (0.5f + 0.5f * std::sin ((float) now * 0.0137f) * std::sin ((float) now * 0.0031f)));
            for (auto* p : peers)
                p->refresh();
            refresh (seconds);
        }

        void paint (juce::Graphics& g) override
        {
            const auto& t = *theme;
            const auto bounds = getLocalBounds();
            g.fillAll (t.background);

            // Header: the maker and the faction, the plugin's name, its subtitle and the faction mark. (X-16 printed
            // "LAB X-16" here above each effect's name; LAB X-11's name is the plugin itself.)
            auto header = bounds.withHeight (headerHeight).reduced (margin + 4, 6);
            const auto glyphArea = header.removeFromLeft (34).toFloat().withSizeKeepingCentre (28.0f, 28.0f).translated (0.0f, 2.0f);
            g.setColour (t.isDeepZone() ? t.phosphor : t.phosphor.withAlpha (0.9f));
            if (t.isDeepZone())
                g.fillPath (factionGlyph (t.faction, glyphArea));
            else
                g.strokePath (factionGlyph (t.faction, glyphArea), juce::PathStrokeType (1.6f));
            header.removeFromLeft (8);
            g.setFont (monoFont (11.0f));
            g.setColour (t.dim);
            g.drawText ("OPERATION FAIRWAY  //  " + juce::String (t.isDeepZone() ? "THE DEEP ZONE" : "THE ETHEREAL SIGNAL"),
                        header.withHeight (14), juce::Justification::topLeft);
            g.setFont (monoFont (24.0f, true));
            g.setColour (t.bright);
            g.drawText (processor.info.product, header.withTrimmedTop (13).withHeight (28), juce::Justification::topLeft);
            const int nameWidth = (int) std::ceil (juce::GlyphArrangement::getStringWidth (monoFont (24.0f, true), processor.info.product)) + 14;
            // The subtitle stops short of the preset selector (long pairs such as WISH GRANTER squeeze to fit).
            g.setFont (monoFont (12.0f));
            g.setColour (t.phosphor);
            const auto subtitleArea = header.withTrimmedTop (22).withTrimmedLeft (nameWidth).withHeight (16)
                                            .withRight (std::min (header.getRight(), prevButton.getX() - 8));
            g.drawFittedText ("//  " + utf8 (processor.info.subtitle), subtitleArea, juce::Justification::topLeft, 1, 0.75f);

            for (const auto& row : rows)
                for (const auto& panel : row)
                    drawPanel (g, panel);
            for (const auto& panel : side)
                drawPanel (g, panel);

            auto footer = bounds.withTop (bounds.getHeight() - footerHeight).reduced (margin + 4, 3);
            g.setFont (monoFont (9.5f));
            g.setColour (t.dim);
            g.drawText ("OPERATION FAIRWAY  //  AGPL-3.0  //  github.com/crystalball-dev/lab-x-11", footer, juce::Justification::centredLeft);
            juce::String right ("v");
            right << processor.info.version;
            if (processor.info.busClient)
                right << "  //  CALLSIGN " << processor.getCallsign();
            right << "  //  NO GAME AUDIO INCLUDED";
            g.drawText (right, footer, juce::Justification::centredRight);
        }

        void resized() override
        {
            const auto bounds = getLocalBounds();
            overlay.setBounds (bounds);
            auto header = bounds.withHeight (headerHeight).reduced (margin, 12);
            auto presetArea = header.withSizeKeepingCentre (300, 26).translated (84, -2);
            prevButton.setBounds (presetArea.removeFromLeft (26));
            nextButton.setBounds (presetArea.removeFromRight (26));
            presetBox.setBounds (presetArea.reduced (4, 0));
            systemLabel.setBounds (header.removeFromRight (230).translated (0, -4).withHeight (32));

            auto body = bounds.withTrimmedTop (headerHeight).withTrimmedBottom (footerHeight).reduced (margin, 0);
            if (! side.empty())
            {
                auto column = body.removeFromRight (sideWidth);
                body.removeFromRight (gap);
                const int h = (column.getHeight() - gap * ((int) side.size() - 1)) / (int) side.size();
                for (auto& panel : side)
                {
                    panel.bounds = column.removeFromTop (h);
                    column.removeFromTop (gap);
                    auto content = panel.bounds.withTrimmedTop (titleHeight + 4).reduced (panelPad, 4);
                    for (const auto& item : panel.items)
                        item.component->setBounds (content);
                }
            }

            if (rows.empty())
                return;
            const int rowCount = (int) rows.size();
            const int rowHeight = (body.getHeight() - gap * (rowCount - 1)) / rowCount;
            float unitWidth = 78.0f;
            for (const auto& row : rows)
            {
                float units = 0.0f;
                for (const auto& panel : row)
                    for (const auto& item : panel.items)
                        units += item.units;
                const float overhead = (float) (row.size() * 2 * panelPad + (row.size() - 1) * gap);
                if (units > 0.0f)
                    unitWidth = std::min (unitWidth, ((float) body.getWidth() - overhead) / units);
            }
            for (int r = 0; r < rowCount; ++r)
                layoutRow (rows[(size_t) r], body.withTrimmedTop (r * (rowHeight + gap)).withHeight (rowHeight), unitWidth);
        }

    protected:
        struct Item
        {
            juce::Component* component;
            float units;
        };

        struct Panel
        {
            juce::String title;
            std::vector<Item> items;
            bool accent = false;
            juce::Rectangle<int> bounds {};
        };

        static Panel panel (const char* title, std::vector<Item> items, bool accent = false)
        {
            return Panel { utf8 (title), std::move (items), accent, {} };
        }

        LabeledKnob* knob (const char* id, const char* caption, bool accent = false)
        {
            return own (std::make_unique<LabeledKnob> (processor.apvts, id, utf8 (caption), accent));
        }

        ChoiceStack* choices (std::initializer_list<std::pair<const char*, const char*>> idsAndCaptions)
        {
            auto stack = std::make_unique<ChoiceStack>();
            for (const auto& [id, caption] : idsAndCaptions)
                stack->add (std::make_unique<LabeledChoice> (processor.apvts, id, utf8 (caption)));
            return own (std::move (stack));
        }

        PeersView* peersView()
        {
            auto* p = own (std::make_unique<PeersView> (processor));
            peers.push_back (p);
            return p;
        }

        // The common controls: MIX, OUTPUT, BYPASS (and BUS for bus clients).
        Panel outputPanel()
        {
            std::vector<Item> items { { knob ("mix", "MIX"), 1.0f }, { knob ("output", "OUTPUT"), 1.0f } };
            if (processor.info.busClient)
                items.push_back ({ choices ({ { "bypass", "BYPASS" }, { "bus", "ZONE BUS" } }), 1.3f });
            else
                items.push_back ({ choices ({ { "bypass", "BYPASS" } }), 1.3f });
            return panel ("OUTPUT // ВЫХОД", std::move (items));
        }

        template <typename C>
        C* own (std::unique_ptr<C> c)
        {
            auto* raw = c.get();
            addAndMakeVisible (*raw);
            if constexpr (std::is_base_of_v<Themed, C>)
                themed.push_back (raw);
            owned.push_back (std::move (c));
            return raw;
        }

        void addRow (std::vector<Panel> row) { rows.push_back (std::move (row)); }
        void addSide (Panel p) { side.push_back (std::move (p)); }

        // Per frame, after the common readouts. `seconds` since the last frame.
        virtual void refresh (float seconds) { juce::ignoreUnused (seconds); }

        host::EffectProcessor& processor;
        const Theme* theme;

    private:
        static constexpr int headerHeight = 58, footerHeight = 20, margin = 10, gap = 6, panelPad = 6, titleHeight = 18, sideWidth = 196;

        void timerCallback() override { refreshFromProcessor(); }

        void stepPreset (int delta)
        {
            const int count = processor.getNumPrograms();
            processor.setCurrentProgram ((processor.getCurrentProgram() + delta + count) % count);
            presetBox.setSelectedId (processor.getCurrentProgram() + 1, juce::dontSendNotification);
        }

        void drawPanel (juce::Graphics& g, const Panel& p) const
        {
            const auto& t = *theme;
            const auto r = p.bounds.toFloat();
            g.setColour (t.panel.withAlpha (0.94f));
            g.fillRoundedRectangle (r, t.cornerRadius + 1.0f);
            g.setColour (p.accent ? t.accent.withAlpha (0.5f) : t.border);
            g.drawRoundedRectangle (r.reduced (0.5f), t.cornerRadius + 1.0f, t.borderWeight);
            auto title = p.bounds.withHeight (titleHeight).reduced (panelPad, 0);
            g.setFont (monoFont (10.5f, true));
            g.setColour (p.accent ? t.accent : t.phosphor.withAlpha (0.88f));
            g.drawText (p.title, title.withTrimmedTop (3), juce::Justification::centredLeft);
            g.setColour (p.accent ? t.accent.withAlpha (0.3f) : t.faint);
            g.drawHorizontalLine (p.bounds.getY() + titleHeight, r.getX() + 4.0f, r.getRight() - 4.0f);
        }

        void layoutRow (std::vector<Panel>& row, juce::Rectangle<int> area, float unitWidth)
        {
            float naturalTotal = 0.0f;
            for (const auto& p : row)
            {
                float units = 0.0f;
                for (const auto& item : p.items)
                    units += item.units;
                naturalTotal += units * unitWidth + (float) (2 * panelPad);
            }
            const float spare = std::max (0.0f, (float) area.getWidth() - naturalTotal - (float) (gap * ((int) row.size() - 1)));
            const float extra = spare / (float) std::max<size_t> (1, row.size());
            float x = (float) area.getX();
            for (auto& p : row)
            {
                float units = 0.0f;
                for (const auto& item : p.items)
                    units += item.units;
                const float w = units * unitWidth + (float) (2 * panelPad) + extra;
                p.bounds = juce::Rectangle<int> ((int) std::round (x), area.getY(), (int) std::round (w), area.getHeight());
                auto content = p.bounds.withTrimmedTop (titleHeight + 4).reduced (panelPad, 0).withTrimmedBottom (4);
                float cx = (float) content.getX() + ((float) content.getWidth() - units * unitWidth) * 0.5f;
                for (const auto& item : p.items)
                {
                    const float iw = item.units * unitWidth;
                    item.component->setBounds (juce::Rectangle<int> ((int) std::round (cx), content.getY(), (int) std::round (iw), content.getHeight()));
                    cx += iw;
                }
                x += w + (float) gap;
            }
        }

        ThemedLookAndFeel laf;
        juce::ComboBox presetBox;
        juce::TextButton prevButton { "<" }, nextButton { ">" };
        juce::Label systemLabel;
        Overlay overlay;
        std::vector<std::vector<Panel>> rows;
        std::vector<Panel> side;
        std::vector<std::unique_ptr<juce::Component>> owned;
        std::vector<Themed*> themed;
        std::vector<PeersView*> peers;
        juce::TooltipWindow tooltips { this, 600 };
        double lastFrame = 0.0;
    };
}
