#include "ui/StripDetailPanel.h"
#include "ui/Controls.h"
#include "ui/EqEditor.h"
#include "ui/MeterComponent.h"
#include "dsp/DspUtils.h"

namespace livedaw::ui
{

namespace
{
    /** A titled box that lays its children out in a simple grid. */
    class Section : public juce::Component
    {
    public:
        Section (const juce::String& t, int preferredWidth) : title (t), width (preferredWidth) {}

        int getPreferredWidth() const noexcept { return width; }

        void paint (juce::Graphics& g) override
        {
            auto b = getLocalBounds().toFloat().reduced (2.0f);
            g.setColour (Colours::panel);
            g.fillRoundedRectangle (b, 5.0f);
            g.setColour (Colours::textDim);
            g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
            g.drawText (title.toUpperCase(), b.reduced (8.0f, 4.0f).removeFromTop (16.0f), juce::Justification::centredLeft);
        }

    protected:
        juce::Rectangle<int> contentArea() const { return getLocalBounds().reduced (8).withTrimmedTop (20); }

        template <typename T, typename... Args>
        T& make (Args&&... args)
        {
            auto* c = new T (std::forward<Args> (args)...);
            owned.add (c);
            addAndMakeVisible (c);
            return *c;
        }

    private:
        juce::String title;
        int width;
        juce::OwnedArray<juce::Component> owned;
    };

    void addOutputItems (ParamCombo& combo, const juce::StringArray& names)
    {
        combo.addValue ("-", -1);
        for (int i = 0; i < names.size(); ++i)
            combo.addValue (names[i], i);
        combo.refresh();
    }

    //==============================================================================
    class InputSection final : public Section
    {
    public:
        InputSection (Strip& s, MixerEngine& engine)
            : Section (s.getKind() == StripKind::Input ? "Input" : "Output", 230),
              trim (make<Knob> ("Trim", s.params.trimDb, s.getKind() == StripKind::Input ? -20.0 : -20.0,
                                s.getKind() == StripKind::Input ? 40.0 : 20.0, 0.1, " dB")),
              hpfFreq (make<Knob> ("HPF", s.params.hpfFrequency, 20.0, 400.0, 1.0, " Hz")),
              polarity (make<ParamButton> (s.params.polarityInvert, juce::String (juce::CharPointer_UTF8 ("\xc3\x98")))),
              hpfOn (make<ParamButton> (s.params.hpfEnabled, "HPF")),
              mainAssign (make<ParamButton> (s.params.mainAssign, "To Master", Colours::active)),
              outLeft (make<ParamCombo> (s.params.outputLeft)),
              outRight (make<ParamCombo> (s.params.outputRight)),
              outLabel (make<juce::Label>())
        {
            hpfFreq.slider.setSkewAround (80.0);
            addOutputItems (outLeft, engine.getHardwareOutputNames());
            addOutputItems (outRight, engine.getHardwareOutputNames());
            outLabel.setText (s.getKind() == StripKind::Input ? "Direct out L / R" : "Hardware out L / R",
                              juce::dontSendNotification);
            outLabel.setFont (juce::FontOptions (11.0f));
            outLabel.setColour (juce::Label::textColourId, Colours::textDim);
            mainAssign.setVisible (s.getKind() != StripKind::Master);
        }

        void resized() override
        {
            auto b = contentArea();
            auto knobs = b.removeFromTop (78);
            trim.setBounds (knobs.removeFromLeft (70));
            hpfFreq.setBounds (knobs.removeFromLeft (70));
            auto toggles = knobs.reduced (4, 8);
            polarity.setBounds (toggles.removeFromTop (24));
            toggles.removeFromTop (6);
            hpfOn.setBounds (toggles.removeFromTop (24));

            b.removeFromTop (6);
            mainAssign.setBounds (b.removeFromTop (24));
            b.removeFromTop (6);
            outLabel.setBounds (b.removeFromTop (16));
            auto outs = b.removeFromTop (24);
            outLeft.setBounds (outs.removeFromLeft (outs.getWidth() / 2).reduced (1, 0));
            outRight.setBounds (outs.reduced (1, 0));
        }

    private:
        Knob &trim, &hpfFreq;
        ParamButton &polarity, &hpfOn, &mainAssign;
        ParamCombo &outLeft, &outRight;
        juce::Label& outLabel;
    };

    //==============================================================================
    class GateSection final : public Section
    {
    public:
        explicit GateSection (Strip& s)
            : Section ("Gate", 250),
              on (make<ParamButton> (s.params.gate.enabled, "ON", Colours::active)),
              threshold (make<Knob> ("Thresh", s.params.gate.thresholdDb, -80.0, 0.0, 0.5, " dB")),
              range (make<Knob> ("Range", s.params.gate.rangeDb, -80.0, 0.0, 0.5, " dB")),
              attack (make<Knob> ("Attack", s.params.gate.attackMs, 0.05, 50.0, 0.05, " ms")),
              hold (make<Knob> ("Hold", s.params.gate.holdMs, 0.0, 500.0, 1.0, " ms")),
              release (make<Knob> ("Release", s.params.gate.releaseMs, 5.0, 2000.0, 1.0, " ms")),
              meter (make<GainReductionMeter> ([&s] { return -dsp::gainToDb (s.gateGain.load()); }, 60.0f))
        {
            attack.slider.setSkewAround (2.0);
            release.slider.setSkewAround (200.0);
            hold.slider.setSkewAround (60.0);
        }

        void resized() override
        {
            auto b = contentArea();
            auto top = b.removeFromTop (24);
            on.setBounds (top.removeFromLeft (50));
            top.removeFromLeft (6);
            meter.setBounds (top.reduced (0, 4));
            b.removeFromTop (6);
            auto row1 = b.removeFromTop (78);
            const int w = row1.getWidth() / 3;
            threshold.setBounds (row1.removeFromLeft (w));
            range.setBounds (row1.removeFromLeft (w));
            attack.setBounds (row1);
            auto row2 = b.removeFromTop (78);
            hold.setBounds (row2.removeFromLeft (w));
            release.setBounds (row2.removeFromLeft (w));
        }

    private:
        ParamButton& on;
        Knob &threshold, &range, &attack, &hold, &release;
        GainReductionMeter& meter;
    };

    //==============================================================================
    class EqSection final : public Section
    {
    public:
        EqSection (Strip& s, MixerEngine& engine)
            : Section ("Equalizer", 680),
              on (make<ParamButton> (s.params.eq.enabled, "ON", Colours::active)),
              editor (make<EqEditor> (s.params.eq, [&engine] { return engine.getSampleRate(); }))
        {
            for (int b = 0; b < dsp::ParametricEq::kNumBands; ++b)
            {
                auto& band = s.params.eq.bands[(size_t) b];
                auto& ctl = bands[b];
                ctl.on = &make<ParamButton> (band.enabled, juce::String (b + 1), Colours::active);
                ctl.type = &make<ParamCombo> (band.type);
                using T = dsp::ParametricEq::BandType;
                ctl.type->addValue ("Bell", static_cast<int> (T::Bell));
                if (b == 0)
                {
                    ctl.type->addValue ("Shelf", static_cast<int> (T::LowShelf));
                    ctl.type->addValue ("Cut", static_cast<int> (T::LowCut));
                }
                if (b == dsp::ParametricEq::kNumBands - 1)
                {
                    ctl.type->addValue ("Shelf", static_cast<int> (T::HighShelf));
                    ctl.type->addValue ("Cut", static_cast<int> (T::HighCut));
                }
                ctl.type->refresh();
                ctl.type->setEnabled (b == 0 || b == dsp::ParametricEq::kNumBands - 1);

                ctl.freq = &make<Knob> ("Freq", band.frequency, 20.0, 20000.0, 1.0, " Hz");
                ctl.freq->slider.setSkewAround (1000.0);
                ctl.gain = &make<Knob> ("Gain", band.gainDb, -18.0, 18.0, 0.1, " dB");
                ctl.q = &make<Knob> ("Q", band.q, 0.1, 16.0, 0.01, "");
                ctl.q->slider.setSkewAround (1.0);
            }
        }

        void resized() override
        {
            auto b = contentArea();
            auto left = b.removeFromLeft (230);
            on.setBounds (left.removeFromTop (24).removeFromLeft (50));
            left.removeFromTop (6);
            editor.setBounds (left);
            b.removeFromLeft (8);

            const int colW = b.getWidth() / dsp::ParametricEq::kNumBands;
            for (auto& ctl : bands)
            {
                auto col = b.removeFromLeft (colW).reduced (2, 0);
                auto header = col.removeFromTop (24);
                ctl.on->setBounds (header.removeFromLeft (24));
                header.removeFromLeft (2);
                ctl.type->setBounds (header);
                col.removeFromTop (2);
                const int h = col.getHeight() / 3;
                ctl.freq->setBounds (col.removeFromTop (h));
                ctl.gain->setBounds (col.removeFromTop (h));
                ctl.q->setBounds (col);
            }
        }

    private:
        struct BandControls
        {
            ParamButton* on = nullptr;
            ParamCombo* type = nullptr;
            Knob *freq = nullptr, *gain = nullptr, *q = nullptr;
        };

        ParamButton& on;
        EqEditor& editor;
        BandControls bands[dsp::ParametricEq::kNumBands];
    };

    //==============================================================================
    class CompSection final : public Section
    {
    public:
        explicit CompSection (Strip& s)
            : Section ("Compressor", 250),
              on (make<ParamButton> (s.params.comp.enabled, "ON", Colours::active)),
              threshold (make<Knob> ("Thresh", s.params.comp.thresholdDb, -60.0, 0.0, 0.5, " dB")),
              ratio (make<Knob> ("Ratio", s.params.comp.ratio, 1.0, 20.0, 0.1, ":1")),
              knee (make<Knob> ("Knee", s.params.comp.kneeDb, 0.0, 24.0, 0.5, " dB")),
              attack (make<Knob> ("Attack", s.params.comp.attackMs, 0.1, 200.0, 0.1, " ms")),
              release (make<Knob> ("Release", s.params.comp.releaseMs, 10.0, 2000.0, 1.0, " ms")),
              makeup (make<Knob> ("Makeup", s.params.comp.makeupDb, 0.0, 24.0, 0.1, " dB")),
              meter (make<GainReductionMeter> ([&s] { return s.compReductionDb.load(); }))
        {
            ratio.slider.setSkewAround (4.0);
            attack.slider.setSkewAround (10.0);
            release.slider.setSkewAround (150.0);
        }

        void resized() override
        {
            auto b = contentArea();
            auto top = b.removeFromTop (24);
            on.setBounds (top.removeFromLeft (50));
            top.removeFromLeft (6);
            meter.setBounds (top.reduced (0, 4));
            b.removeFromTop (6);
            auto row1 = b.removeFromTop (78);
            const int w = row1.getWidth() / 3;
            threshold.setBounds (row1.removeFromLeft (w));
            ratio.setBounds (row1.removeFromLeft (w));
            knee.setBounds (row1);
            auto row2 = b.removeFromTop (78);
            attack.setBounds (row2.removeFromLeft (w));
            release.setBounds (row2.removeFromLeft (w));
            makeup.setBounds (row2);
        }

    private:
        ParamButton& on;
        Knob &threshold, &ratio, &knee, &attack, &release, &makeup;
        GainReductionMeter& meter;
    };

    //==============================================================================
    class InsertsSection final : public Section, private juce::Timer
    {
    public:
        InsertsSection (Strip& s, MixerEngine& e, PluginHost& h, StripDetailPanel& p)
            : Section ("Inserts", 210), strip (s), engine (e), host (h), panel (p)
        {
            for (int i = 0; i < kNumInsertSlots; ++i)
            {
                auto& row = rows[i];
                row.name = &make<juce::TextButton>();
                row.name->onClick = [this, i] { showSlotMenu (i); };
                row.bypass = &make<juce::TextButton> ("BYP");
                row.bypass->setClickingTogglesState (true);
                row.bypass->setColour (juce::TextButton::buttonOnColourId, Colours::solo);
                row.bypass->onClick = [this, i] { strip.inserts.setBypassed (i, rows[i].bypass->getToggleState()); };
            }

            latencyLabel = &make<juce::Label>();
            latencyLabel->setFont (juce::FontOptions (11.0f));
            latencyLabel->setColour (juce::Label::textColourId, Colours::textDim);

            timerCallback();
            startTimerHz (4);
        }

        void resized() override
        {
            auto b = contentArea();
            latencyLabel->setBounds (b.removeFromBottom (16));
            const int h = juce::jmin (24, b.getHeight() / kNumInsertSlots);
            for (auto& row : rows)
            {
                auto r = b.removeFromTop (h).reduced (0, 1);
                row.bypass->setBounds (r.removeFromRight (40));
                r.removeFromRight (2);
                row.name->setBounds (r);
            }
        }

    private:
        void timerCallback() override
        {
            for (int i = 0; i < kNumInsertSlots; ++i)
            {
                auto* plugin = strip.inserts.getPlugin (i);
                rows[i].name->setButtonText (plugin != nullptr ? plugin->getName() : juce::String (i + 1) + ": -");
                rows[i].bypass->setEnabled (plugin != nullptr);
                rows[i].bypass->setToggleState (strip.inserts.isBypassed (i), juce::dontSendNotification);
            }

            const int latency = strip.inserts.getTotalLatencySamples();
            latencyLabel->setText (latency > 0 ? "Plugin latency: " + juce::String (latency) + " smp ("
                                                     + juce::String (1000.0 * latency / engine.getSampleRate(), 1) + " ms)"
                                               : juce::String(),
                                   juce::dontSendNotification);
        }

        void showSlotMenu (int slot)
        {
            juce::PopupMenu menu;
            auto* existing = strip.inserts.getPlugin (slot);

            if (existing != nullptr)
            {
                menu.addItem ("Open editor", [this, slot] { if (panel.onOpenPluginEditor) panel.onOpenPluginEditor (strip, slot); });
                menu.addItem ("Remove", [this, slot] { setPlugin (slot, nullptr); });
                menu.addSeparator();
            }

            juce::PopupMenu plugins;
            const auto types = host.getSortedTypes();
            juce::String currentMaker;
            juce::PopupMenu makerMenu;

            auto flushMaker = [&]
            {
                if (currentMaker.isNotEmpty())
                    plugins.addSubMenu (currentMaker, makerMenu);
                makerMenu = {};
            };

            for (const auto& desc : types)
            {
                if (desc.isInstrument)
                    continue;

                const auto maker = desc.manufacturerName.isEmpty() ? juce::String ("Unknown") : desc.manufacturerName;
                if (maker != currentMaker)
                {
                    flushMaker();
                    currentMaker = maker;
                }

                makerMenu.addItem (desc.name + " (" + desc.pluginFormatName + ")", [this, slot, desc]
                {
                    juce::String error;
                    auto instance = host.createInstance (desc, engine.getSampleRate(), engine.getBlockSize(), error);
                    if (instance == nullptr)
                    {
                        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                                "Could not load plugin", desc.name + "\n" + error);
                        return;
                    }
                    setPlugin (slot, std::move (instance));
                    if (panel.onOpenPluginEditor)
                        panel.onOpenPluginEditor (strip, slot);
                });
            }
            flushMaker();

            menu.addSubMenu (existing != nullptr ? "Replace with" : "Insert plugin", plugins, ! types.isEmpty());
            menu.addItem ("Scan for plugins...", [this] { if (panel.onShowPluginScanner) panel.onShowPluginScanner(); });

            menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (rows[slot].name));
        }

        void setPlugin (int slot, std::unique_ptr<juce::AudioPluginInstance> plugin)
        {
            if (auto* old = strip.inserts.getPlugin (slot))
                if (panel.onPluginRemoved)
                    panel.onPluginRemoved (old);

            strip.inserts.setPlugin (slot, std::move (plugin));
            timerCallback();
        }

        struct Row
        {
            juce::TextButton* name = nullptr;
            juce::TextButton* bypass = nullptr;
        };

        Strip& strip;
        MixerEngine& engine;
        PluginHost& host;
        StripDetailPanel& panel;
        Row rows[kNumInsertSlots];
        juce::Label* latencyLabel = nullptr;
    };

    //==============================================================================
    class SendsSection final : public Section
    {
    public:
        SendsSection (Strip& s, MixerEngine& engine) : Section ("Sends", 330)
        {
            for (auto& bus : engine.getBuses())
            {
                auto& send = s.params.sends[(size_t) bus->getId()];
                Row row;
                row.label = &make<juce::Label>();
                row.label->setText (bus->name, juce::dontSendNotification);
                row.label->setFont (juce::FontOptions (12.0f));
                row.label->setColour (juce::Label::textColourId, bus->colour.brighter (0.4f));
                row.on = &make<ParamButton> (send.enabled, "ON", Colours::active);
                row.pre = &make<ParamButton> (send.preFader, "PRE", Colours::solo);
                row.level = &make<ParamSlider> (send.levelDb, -100.0, 10.0, 0.1, " dB", juce::Slider::LinearHorizontal);
                row.level->setSkewFactorFromMidPoint (-12.0);
                row.level->setTextBoxStyle (juce::Slider::TextBoxRight, false, 56, 18);
                row.level->setDoubleClickReturnValue (true, 0.0);
                rows.push_back (row);
            }

            if (rows.empty())
            {
                auto& hint = make<juce::Label>();
                hint.setText ("Add an Aux or Group bus to create sends.", juce::dontSendNotification);
                hint.setColour (juce::Label::textColourId, Colours::textDim);
                hint.setFont (juce::FontOptions (12.0f));
                emptyHint = &hint;
            }
        }

        void resized() override
        {
            auto b = contentArea();
            if (emptyHint != nullptr)
                emptyHint->setBounds (b.removeFromTop (40));

            const int h = rows.empty() ? 0 : juce::jlimit (16, 24, b.getHeight() / static_cast<int> (rows.size()));
            for (auto& row : rows)
            {
                auto r = b.removeFromTop (h);
                row.label->setBounds (r.removeFromLeft (80));
                row.on->setBounds (r.removeFromLeft (34).reduced (1));
                row.pre->setBounds (r.removeFromLeft (38).reduced (1));
                row.level->setBounds (r);
            }
        }

    private:
        struct Row
        {
            juce::Label* label = nullptr;
            ParamButton *on = nullptr, *pre = nullptr;
            ParamSlider* level = nullptr;
        };

        std::vector<Row> rows;
        juce::Label* emptyHint = nullptr;
    };

    //==============================================================================
    class MonitorSection final : public Section
    {
    public:
        explicit MonitorSection (MixerEngine& engine)
            : Section ("Monitor / Solo", 230),
              afl (make<ParamButton> (engine.soloAfl, "AFL (off = PFL)", Colours::solo)),
              level (make<Knob> ("Level", engine.monitorLevelDb, -100.0, 10.0, 0.1, " dB")),
              outLeft (make<ParamCombo> (engine.monitorOutputLeft)),
              outRight (make<ParamCombo> (engine.monitorOutputRight)),
              label (make<juce::Label>()),
              meter (make<MeterComponent> ([&engine] (int ch) { return engine.monitorPeak[(size_t) ch].exchange (0.0f); },
                                           [] { return 2; }))
        {
            level.slider.setSkewAround (-12.0);
            addOutputItems (outLeft, engine.getHardwareOutputNames());
            addOutputItems (outRight, engine.getHardwareOutputNames());
            label.setText ("Monitor out L / R", juce::dontSendNotification);
            label.setFont (juce::FontOptions (11.0f));
            label.setColour (juce::Label::textColourId, Colours::textDim);
        }

        void resized() override
        {
            auto b = contentArea();
            meter.setBounds (b.removeFromRight (14));
            b.removeFromRight (6);
            afl.setBounds (b.removeFromTop (24));
            b.removeFromTop (4);
            level.setBounds (b.removeFromTop (78).withSizeKeepingCentre (80, 78));
            b.removeFromTop (4);
            label.setBounds (b.removeFromTop (16));
            auto outs = b.removeFromTop (24);
            outLeft.setBounds (outs.removeFromLeft (outs.getWidth() / 2).reduced (1, 0));
            outRight.setBounds (outs.reduced (1, 0));
        }

    private:
        ParamButton& afl;
        Knob& level;
        ParamCombo &outLeft, &outRight;
        juce::Label& label;
        MeterComponent& meter;
    };
} // namespace

//==============================================================================
class StripDetailPanel::Content final : public juce::Component
{
public:
    Content (Strip& s, MixerEngine& engine, PluginHost& host, StripDetailPanel& panel)
    {
        add (std::make_unique<InputSection> (s, engine));
        add (std::make_unique<GateSection> (s));
        add (std::make_unique<EqSection> (s, engine));
        add (std::make_unique<CompSection> (s));
        add (std::make_unique<InsertsSection> (s, engine, host, panel));

        if (s.getKind() == StripKind::Input)
            add (std::make_unique<SendsSection> (s, engine));
        else if (s.getKind() == StripKind::Master)
            add (std::make_unique<MonitorSection> (engine));
    }

    int getPreferredWidth() const
    {
        int w = 0;
        for (auto* s : sections)
            w += s->getPreferredWidth();
        return w;
    }

    void resized() override
    {
        auto b = getLocalBounds();
        for (auto* s : sections)
            s->setBounds (b.removeFromLeft (s->getPreferredWidth()));
    }

private:
    void add (std::unique_ptr<Section> s)
    {
        addAndMakeVisible (*s);
        sections.add (s.release());
    }

    juce::OwnedArray<Section> sections;
};

//==============================================================================
StripDetailPanel::StripDetailPanel (MixerEngine& e, PluginHost& h) : engine (e), host (h)
{
    viewport.setScrollBarsShown (false, true);
    addAndMakeVisible (viewport);
}

StripDetailPanel::~StripDetailPanel()
{
    viewport.setViewedComponent (nullptr, false);
}

void StripDetailPanel::setStrip (Strip* strip)
{
    const auto scroll = viewport.getViewPosition();
    viewport.setViewedComponent (nullptr, false);
    content.reset();
    current = strip;

    if (strip != nullptr)
    {
        content = std::make_unique<Content> (*strip, engine, host, *this);
        viewport.setViewedComponent (content.get(), false);
        resized();
        viewport.setViewPosition (scroll);
    }

    repaint();
}

void StripDetailPanel::paint (juce::Graphics& g)
{
    g.fillAll (Colours::background.darker (0.2f));

    auto header = getLocalBounds().removeFromTop (24).reduced (10, 0);
    g.setFont (juce::FontOptions (14.0f, juce::Font::bold));

    if (current == nullptr)
    {
        g.setColour (Colours::textDim);
        g.drawText ("Select a channel (SEL) to edit its processing", getLocalBounds(), juce::Justification::centred);
        return;
    }

    g.setColour (current->colour);
    g.fillRect (header.removeFromLeft (6).reduced (0, 5));
    header.removeFromLeft (6);
    g.setColour (Colours::text);
    g.drawText (current->name + "  -  " + juce::String (toString (current->getKind())).toUpperCase(),
                header, juce::Justification::centredLeft);
}

void StripDetailPanel::resized()
{
    auto b = getLocalBounds();
    b.removeFromTop (24);
    viewport.setBounds (b);

    if (content != nullptr)
        content->setSize (juce::jmax (b.getWidth(), content->getPreferredWidth()),
                          b.getHeight() - viewport.getScrollBarThickness());
}

} // namespace livedaw::ui
