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
            : Section (s.getKind() == StripKind::Input ? "Input" : "Output", 250),
              strip (s),
              trim (make<Knob> ("Trim", s.params.trimDb, -20.0, s.getKind() == StripKind::Input ? 40.0 : 20.0, 0.1, " dB")),
              hpfFreq (make<Knob> ("HPF", s.params.hpfFrequency, 20.0, 400.0, 1.0, " Hz")),
              polarity (make<ParamButton> (s.params.polarityInvert, juce::String (juce::CharPointer_UTF8 ("\xc3\x98")))),
              hpfOn (make<ParamButton> (s.params.hpfEnabled, "HPF")),
              stereo (make<ParamButton> (s.params.stereo, "Stereo")),
              safe (make<ParamButton> (s.params.recallSafe, "Safe", Colours::solo)),
              mainAssign (make<ParamButton> (s.params.mainAssign, "To Master", Colours::active)),
              inLeft (make<ParamCombo> (s.params.inputLeft)),
              inRight (make<ParamCombo> (s.params.inputRight)),
              outLeft (make<ParamCombo> (s.params.outputLeft)),
              outRight (make<ParamCombo> (s.params.outputRight)),
              inLabel (make<juce::Label>()),
              outLabel (make<juce::Label>())
        {
            const bool isInput = s.getKind() == StripKind::Input;
            hpfFreq.slider.setSkewAround (80.0);
            stereo.setTooltip ("Mono / stereo channel");
            safe.setTooltip ("Recall safe: scenes won't change this strip");

            addInputItems (inLeft, engine.getHardwareInputNames());
            addInputItems (inRight, engine.getHardwareInputNames());
            addOutputItems (outLeft, engine.getHardwareOutputNames());
            addOutputItems (outRight, engine.getHardwareOutputNames());

            for (auto* l : { &inLabel, &outLabel })
            {
                l->setFont (juce::FontOptions (11.0f));
                l->setColour (juce::Label::textColourId, Colours::textDim);
            }
            inLabel.setText ("Input L / R", juce::dontSendNotification);
            outLabel.setText (isInput ? "Direct out L / R" : "Hardware out L / R", juce::dontSendNotification);

            stereo.onClick = [this]
            {
                auto& p = strip.params;
                if (stereo.getToggleState() && p.inputRight.load() < 0 && p.inputLeft.load() >= 0)
                    p.inputRight = p.inputLeft.load() + 1;
                p.stereo = stereo.getToggleState();
            };

            inLabel.setVisible (isInput);
            inLeft.setVisible (isInput);
            inRight.setVisible (isInput);
            stereo.setVisible (s.getKind() != StripKind::Master);
            mainAssign.setVisible (s.getKind() != StripKind::Master);
        }

        void resized() override
        {
            auto b = contentArea();
            auto knobs = b.removeFromTop (72);
            trim.setBounds (knobs.removeFromLeft (70));
            hpfFreq.setBounds (knobs.removeFromLeft (70));
            auto toggles = knobs.reduced (4, 2);
            polarity.setBounds (toggles.removeFromTop (22));
            toggles.removeFromTop (4);
            hpfOn.setBounds (toggles.removeFromTop (22));

            b.removeFromTop (4);
            auto row = b.removeFromTop (22);
            const int w = row.getWidth() / 3;
            stereo.setBounds (row.removeFromLeft (w).reduced (1, 0));
            safe.setBounds (row.removeFromLeft (w).reduced (1, 0));
            mainAssign.setBounds (row.reduced (1, 0));

            auto combos = [&] (juce::Label& label, ParamCombo& l, ParamCombo& r)
            {
                if (! l.isVisible())
                    return;
                b.removeFromTop (2);
                label.setBounds (b.removeFromTop (14));
                auto outs = b.removeFromTop (22);
                l.setBounds (outs.removeFromLeft (outs.getWidth() / 2).reduced (1, 0));
                r.setBounds (outs.reduced (1, 0));
            };

            combos (inLabel, inLeft, inRight);
            combos (outLabel, outLeft, outRight);
        }

    private:
        static void addInputItems (ParamCombo& combo, const juce::StringArray& names)
        {
            combo.addValue ("-", -1);
            for (int i = 0; i < names.size(); ++i)
                combo.addValue (juce::String (i + 1) + ": " + names[i], i);
            combo.refresh();
        }

        Strip& strip;
        Knob &trim, &hpfFreq;
        ParamButton &polarity, &hpfOn, &stereo, &safe, &mainAssign;
        ParamCombo &inLeft, &inRight, &outLeft, &outRight;
        juce::Label &inLabel, &outLabel;
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
        InsertsSection (Strip& s, UiContext& c)
            : Section ("Inserts", 230), strip (s), ctx (c)
        {
            for (int i = 0; i < kNumInsertSlots; ++i)
            {
                auto& row = rows[i];
                row.name = &make<juce::TextButton>();
                row.name->onClick = [this, i] { clickInsertSlot (ctx, strip, i, rows[i].name); };
                row.menu = &make<juce::TextButton> ("...");
                row.menu->onClick = [this, i] { showInsertSlotMenu (ctx, strip, i, rows[i].menu); };
                row.bypass = &make<juce::TextButton> ("BYP");
                row.bypass->setClickingTogglesState (true);
                row.bypass->setColour (juce::TextButton::buttonOnColourId, Colours::solo);
                row.bypass->onClick = [this, i]
                {
                    strip.inserts.clearFault (i);
                    strip.inserts.setBypassed (i, rows[i].bypass->getToggleState());
                };
            }

            infoLabel = &make<juce::Label>();
            infoLabel->setFont (juce::FontOptions (11.0f));
            infoLabel->setColour (juce::Label::textColourId, Colours::textDim);

            timerCallback();
            startTimerHz (4);
        }

        void resized() override
        {
            auto b = contentArea();
            infoLabel->setBounds (b.removeFromBottom (16));
            const int h = juce::jmin (24, b.getHeight() / kNumInsertSlots);
            for (auto& row : rows)
            {
                auto r = b.removeFromTop (h).reduced (0, 1);
                row.bypass->setBounds (r.removeFromRight (38));
                r.removeFromRight (2);
                row.menu->setBounds (r.removeFromRight (24));
                r.removeFromRight (2);
                row.name->setBounds (r);
            }
        }

    private:
        void timerCallback() override
        {
            float totalLoad = 0.0f;

            for (int i = 0; i < kNumInsertSlots; ++i)
            {
                auto* plugin = strip.inserts.getPlugin (i);
                const bool faulted = strip.inserts.isFaulted (i);
                juce::String text = juce::String (i + 1) + ": -";

                if (plugin != nullptr)
                {
                    const float load = strip.inserts.getSlotLoad (i);
                    totalLoad += load;
                    text = (faulted ? "FAULT " : "") + plugin->getName() + "  " + juce::String (juce::roundToInt (load * 100.0f)) + "%";
                }

                rows[i].name->setButtonText (text);
                rows[i].name->setColour (juce::TextButton::buttonColourId, faulted ? Colours::mute : Colours::panelLight);
                rows[i].bypass->setEnabled (plugin != nullptr);
                rows[i].bypass->setToggleState (strip.inserts.isBypassed (i), juce::dontSendNotification);
            }

            const int latency = strip.inserts.getTotalLatencySamples();
            juce::String info;
            if (latency > 0)
                info << "Latency " << latency << " smp (" << juce::String (1000.0 * latency / ctx.engine.getSampleRate(), 1) << " ms)  ";
            if (totalLoad > 0.0f)
                info << "CPU " << juce::roundToInt (totalLoad * 100.0f) << "% of block";
            infoLabel->setText (info, juce::dontSendNotification);
        }

        struct Row
        {
            juce::TextButton *name = nullptr, *menu = nullptr, *bypass = nullptr;
        };

        Strip& strip;
        UiContext& ctx;
        Row rows[kNumInsertSlots];
        juce::Label* infoLabel = nullptr;
    };

    //==============================================================================
    class SendsSection final : public Section
    {
    public:
        SendsSection (Strip& s, MixerEngine& engine) : Section ("Sends", 340)
        {
            // Rows live in a scrolling list, so any number of buses fits.
            viewport.setViewedComponent (&list, false);
            viewport.setScrollBarsShown (true, false);
            addAndMakeVisible (viewport);

            for (auto& bus : engine.getBuses())
            {
                auto& send = s.params.sends[(size_t) bus->getId()];
                Row row;
                row.label = add (std::make_unique<juce::Label>());
                row.label->setText (bus->name, juce::dontSendNotification);
                row.label->setFont (juce::FontOptions (12.0f));
                row.label->setColour (juce::Label::textColourId, bus->colour.brighter (0.4f));
                row.on = add (std::make_unique<ParamButton> (send.enabled, "ON", Colours::active));
                row.pre = add (std::make_unique<ParamButton> (send.preFader, "PRE", Colours::solo));
                row.level = add (std::make_unique<ParamSlider> (send.levelDb, -100.0, 10.0, 0.1, " dB", juce::Slider::LinearHorizontal));
                row.level->setSkewFactorFromMidPoint (-12.0);
                row.level->setTextBoxStyle (juce::Slider::TextBoxRight, false, 56, 18);
                row.level->setDoubleClickReturnValue (true, 0.0);
                row.level->setScrollWheelEnabled (false); // scrolling the list must never move a mix
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

        ~SendsSection() override { viewport.setViewedComponent (nullptr, false); }

        void resized() override
        {
            auto b = contentArea();
            if (emptyHint != nullptr)
                emptyHint->setBounds (b.removeFromTop (40));

            viewport.setBounds (b);
            constexpr int rowHeight = 24;
            const int listWidth = b.getWidth() - viewport.getScrollBarThickness();
            list.setSize (listWidth, rowHeight * static_cast<int> (rows.size()));

            int y = 0;
            for (auto& row : rows)
            {
                auto r = juce::Rectangle<int> (0, y, listWidth, rowHeight);
                y += rowHeight;
                row.label->setBounds (r.removeFromLeft (80));
                row.on->setBounds (r.removeFromLeft (34).reduced (1));
                row.pre->setBounds (r.removeFromLeft (38).reduced (1));
                row.level->setBounds (r);
            }
        }

    private:
        template <typename T>
        T* add (std::unique_ptr<T> c)
        {
            auto* raw = c.get();
            list.addAndMakeVisible (raw);
            listItems.add (c.release());
            return raw;
        }

        struct Row
        {
            juce::Label* label = nullptr;
            ParamButton *on = nullptr, *pre = nullptr;
            ParamSlider* level = nullptr;
        };

        juce::Viewport viewport;
        juce::Component list;
        juce::OwnedArray<juce::Component> listItems;
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
    Content (Strip& s, UiContext& ctx)
    {
        auto& engine = ctx.engine;
        add (std::make_unique<InputSection> (s, engine));
        add (std::make_unique<GateSection> (s));
        add (std::make_unique<EqSection> (s, engine));
        add (std::make_unique<CompSection> (s));
        add (std::make_unique<InsertsSection> (s, ctx));

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
StripDetailPanel::StripDetailPanel (UiContext& c) : ctx (c)
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
        content = std::make_unique<Content> (*strip, ctx);
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
