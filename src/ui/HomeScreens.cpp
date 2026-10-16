#include "ui/HomeScreens.h"
#include "ui/WingWidgets.h"
#include "ui/Controls.h"
#include "ui/MeterComponent.h"
#include "ui/ModelParams.h"
#include "ui/ResponseGraph.h"

namespace livedaw::ui
{

//==============================================================================
void HomeScreen::paint (juce::Graphics& g)
{
    for (auto& p : boxes)
        drawSectionBox (g, p.area, p.title, p.chevron);
}

juce::Label& HomeScreen::label (const juce::String& text, float size, juce::Colour colour)
{
    auto& l = make<juce::Label>();
    l.setText (text, juce::dontSendNotification);
    l.setFont (juce::FontOptions (size, juce::Font::bold));
    l.setColour (juce::Label::textColourId, colour);
    return l;
}

DisplayKnob& HomeScreen::knob (KnobBinding b)
{
    auto& k = make<DisplayKnob>();
    k.setCompact (true);
    k.setBinding (std::move (b));
    return k;
}

BoolButton& HomeScreen::toggle (const juce::String& text, std::atomic<bool>& p, juce::Colour on)
{
    auto& b = make<BoolButton> (text, on);
    b.bind (&p);
    return b;
}

IntCombo& HomeScreen::combo (const juce::StringArray& items, std::atomic<int32_t>* p, const juce::String& noneText)
{
    auto& c = make<IntCombo>();
    c.setItems (items, noneText);
    c.bind (p);
    return c;
}

juce::Rectangle<int> HomeScreen::box (juce::Rectangle<int> area, const juce::String& title, bool chevron)
{
    boxes.push_back ({ area, title, chevron });
    return sectionContent (area, title.isNotEmpty());
}

void layoutRow (juce::Rectangle<int> r, std::initializer_list<juce::Component*> comps, int gap)
{
    const int n = (int) comps.size();
    if (n == 0)
        return;
    const int w = (r.getWidth() - gap * (n - 1)) / n;
    for (auto* c : comps)
    {
        if (c != nullptr)
            c->setBounds (r.removeFromLeft (w));
        r.removeFromLeft (gap);
    }
}

juce::StringArray channelNames (MixerEngine& engine)
{
    juce::StringArray items;
    for (auto& ch : engine.getChannels())
        items.add (describeStrip (*ch));
    return items;
}

KnobBinding thresholdBinding (DynSlotParams& slot, const juce::String& label)
{
    const auto& spec = models::model (models::Dynamics, slot.model.load());
    if (spec.params.empty())
        return {};
    const int i = juce::jmax (0, spec.find ("thr"));
    auto b = bindModelParam (slot.values[i], spec.params[(size_t) i]);
    b.label = label;
    return b;
}

MaskButtons::MaskButtons (std::atomic<uint32_t>& m, int count, const juce::String& prefix, juce::Colour on) : mask (m)
{
    for (int i = 0; i < count; ++i)
    {
        auto* b = buttons.add (new juce::TextButton (prefix + juce::String (i + 1)));
        b->setClickingTogglesState (true);
        b->setColour (juce::TextButton::buttonOnColourId, on);
        b->onClick = [this, i, b]
        {
            const auto bit = 1u << i;
            mask = b->getToggleState() ? (mask.load() | bit) : (mask.load() & ~bit);
        };
        addAndMakeVisible (b);
    }
    startTimerHz (8);
    timerCallback();
}

void MaskButtons::resized()
{
    const int n = buttons.size();
    const int columns = n > 8 ? 8 : n;
    const int rows = (n + columns - 1) / columns;
    const int w = getWidth() / columns, h = getHeight() / juce::jmax (1, rows);
    for (int i = 0; i < n; ++i)
        buttons[i]->setBounds ((i % columns) * w, (i / columns) * h, w - 3, h - 3);
}

void MaskButtons::timerCallback()
{
    for (int i = 0; i < buttons.size(); ++i)
        buttons[i]->setToggleState ((mask.load() >> i) & 1u, juce::dontSendNotification);
}

namespace
{

juce::StringArray hardwareNames (const juce::StringArray& names, int count)
{
    juce::StringArray items;
    for (int i = 0; i < count; ++i)
        items.add (juce::String (i + 1) + ": " + (i < names.size() ? names[i] : juce::String ("(not available)")));
    return items;
}

//==============================================================================
class InputTab final : public HomeScreen
{
public:
    InputTab (UiContext& c, Strip& s) : HomeScreen (c, s)
    {
        auto& p = strip.params;
        trim = &knob (bindLinear ("TRIM", p.input.trimDb, -18.0f, 18.0f, " dB", 1, 0.0f));
        balance = &knob (bindLinear ("BALANCE", p.input.balanceDb, -9.0f, 9.0f, " dB", 1, 0.0f));
        invert = &toggle (juce::String::fromUTF8 ("\xc3\x98 INVERT"), p.input.invert);
        delayOn = &toggle (strip.isInput() ? "DELAY" : "DELAY (POST)", p.input.delayOn);
        delayMode = &combo (models::list (models::DelayUnits), &p.input.delayMode);
        delay = &knob (delayBinding());

        const int numIns = juce::jmax (ctx.engine.getHardwareInputNames().size(), 64);
        const int numOuts = juce::jmax (ctx.engine.getHardwareOutputNames().size(), 64);
        const auto ins = hardwareNames (ctx.engine.getHardwareInputNames(), numIns);
        const auto outs = hardwareNames (ctx.engine.getHardwareOutputNames(), numOuts);

        if (strip.isInput())
        {
            stereo = &toggle ("STEREO", p.stereo, Colours::accent);
            inLCaption = &label ("SOURCE L / MONO");
            inL = &combo (ins, &p.inputLeft, "OFF");
            inRCaption = &label ("SOURCE R");
            inR = &combo (ins, &p.inputRight, "NEXT");
        }
        else
        {
            mono = &toggle ("MONO SUM", p.mono, Colours::accent);
        }

        outLCaption = &label (strip.isInput() ? "DIRECT OUT L" : "OUTPUT L");
        outL = &combo (outs, &p.outputLeft, "OFF");
        outRCaption = &label (strip.isInput() ? "DIRECT OUT R" : "OUTPUT R");
        outR = &combo (outs, &p.outputRight, "OFF");

        monitorCaption = &label ("SOLO TO MONITOR");
        monitor = &combo (models::list (models::MonitorBuses), &p.monitorBus);
        soloSafe = &toggle ("SOLO SAFE", p.soloSafe, Colours::solo);
        recallSafe = &toggle ("RECALL SAFE", p.recallSafe, Colours::mute);
    }

    KnobBinding delayBinding()
    {
        auto& in = strip.params.input;
        KnobBinding b;
        b.label = "DELAY";
        auto maxFor = [&in] { static const float max[] = { 150.0f, 500.0f, 500.0f, 24000.0f }; return max[juce::jlimit (0, 3, (int) in.delayMode.load())]; };
        b.get = [&in, maxFor] { return juce::jlimit (0.0f, 1.0f, std::sqrt (in.delay.load() / maxFor())); };
        b.set = [&in, maxFor] (float p) { in.delay = p * p * maxFor(); };
        b.text = [&in]
        {
            static const char* units[] = { " m", " ft", " ms", " smp" };
            const int mode = juce::jlimit (0, 3, (int) in.delayMode.load());
            return juce::String (in.delay.load(), mode == 3 ? 0 : 1) + units[mode];
        };
        b.reset = [&in] { in.delay = 0.0f; };
        return b;
    }

    std::vector<KnobBinding> knobs() override
    {
        auto& p = strip.params;
        return { bindLinear ("TRIM", p.input.trimDb, -18.0f, 18.0f, " dB", 1, 0.0f),
                 bindLinear ("BALANCE", p.input.balanceDb, -9.0f, 9.0f, " dB", 1, 0.0f), delayBinding(), {}, {},
                 bindFader ("FADER", p.faderDb) };
    }

    void resized() override
    {
        clearBoxes();
        auto b = getLocalBounds();
        auto top = b.removeFromTop (b.getHeight() / 2);

        auto gain = box (top.removeFromLeft (top.getWidth() / 2), "GAIN");
        auto knobs = gain.removeFromLeft (gain.getWidth() * 2 / 3);
        layoutRow (knobs, { trim, balance });
        invert->setBounds (gain.removeFromTop (kKeyHeight).reduced (4, 0));
        gain.removeFromTop (6);
        if (stereo != nullptr)
            stereo->setBounds (gain.removeFromTop (kKeyHeight).reduced (4, 0));
        if (mono != nullptr)
            mono->setBounds (gain.removeFromTop (kKeyHeight).reduced (4, 0));

        auto dly = box (top, "DELAY");
        auto left = dly.removeFromLeft (dly.getWidth() / 2);
        delayOn->setBounds (left.removeFromTop (kKeyHeight).reduced (4, 0));
        left.removeFromTop (6);
        delayMode->setBounds (left.removeFromTop (kKeyHeight).reduced (4, 0));
        delay->setBounds (dly);

        auto routing = box (b.removeFromLeft (b.getWidth() * 2 / 3), "ROUTING");
        auto col1 = routing.removeFromLeft (routing.getWidth() / 2).reduced (4, 0);
        auto col2 = routing.reduced (4, 0);
        auto place = [] (juce::Rectangle<int>& col, juce::Label* l, juce::Component* c)
        {
            if (l == nullptr)
                return;
            l->setBounds (col.removeFromTop (14));
            c->setBounds (col.removeFromTop (kKeyHeight));
            col.removeFromTop (6);
        };
        place (col1, inLCaption, inL);
        place (col1, inRCaption, inR);
        place (col2, outLCaption, outL);
        place (col2, outRCaption, outR);

        auto solo = box (b, "SOLO / SAFES");
        monitorCaption->setBounds (solo.removeFromTop (14));
        monitor->setBounds (solo.removeFromTop (kKeyHeight));
        solo.removeFromTop (6);
        soloSafe->setBounds (solo.removeFromTop (kKeyHeight));
        solo.removeFromTop (4);
        recallSafe->setBounds (solo.removeFromTop (kKeyHeight));
    }

private:
    DisplayKnob *trim = nullptr, *balance = nullptr, *delay = nullptr;
    BoolButton *invert = nullptr, *delayOn = nullptr, *stereo = nullptr, *mono = nullptr, *soloSafe = nullptr, *recallSafe = nullptr;
    IntCombo *delayMode = nullptr, *inL = nullptr, *inR = nullptr, *outL = nullptr, *outR = nullptr, *monitor = nullptr;
    juce::Label *inLCaption = nullptr, *inRCaption = nullptr, *outLCaption = nullptr, *outRCaption = nullptr, *monitorCaption = nullptr;
};

//==============================================================================
class FilterTab final : public HomeScreen
{
public:
    FilterTab (UiContext& c, Strip& s) : HomeScreen (c, s)
    {
        auto& f = strip.params.filter;
        lcOn = &toggle ("LOW CUT", f.lowCut);
        lcFreq = &knob (bindFreq ("FREQ", f.lowCutFreq, 20.0f, 2000.0f, 80.0f));
        lcSlope = &combo (dbOct (models::list (models::LowCutSlopes)), &f.lowCutSlope);
        hcOn = &toggle ("HIGH CUT", f.highCut);
        hcFreq = &knob (bindFreq ("FREQ", f.highCutFreq, 200.0f, 20000.0f, 10000.0f));
        hcSlope = &combo (dbOct (models::list (models::HighCutSlopes)), &f.highCutSlope);
        toolOn = &toggle ("TOOL", f.tool);
        toolModel = &make<ModelSelector>();
        toolModel->bind (models::FilterTools, &f.toolModel, [this] (int m) { strip.setModel (ModelBlock::FilterTool, m); });
        toolParams = &make<ModelParamGrid>();
        toolParams->bind (models::FilterTools, &f.toolModel, f.toolParams);
        toolParams->onModelChanged = [this] { if (knobsChanged) knobsChanged(); };
        graph = &make<ResponseGraph>();
        graph->setStrip (&strip, ResponseGraph::Filter);
    }

    static juce::StringArray dbOct (juce::StringArray items)
    {
        for (auto& i : items)
            i << " dB/oct";
        return items;
    }

    std::vector<KnobBinding> knobs() override
    {
        auto& f = strip.params.filter;
        std::vector<KnobBinding> k { bindFreq ("LO CUT", f.lowCutFreq, 20.0f, 2000.0f, 80.0f),
                                     bindFreq ("HI CUT", f.highCutFreq, 200.0f, 20000.0f, 10000.0f), {} };
        for (auto& b : toolParams->bindings (2))
            k.push_back (b);
        return k;
    }

    void resized() override
    {
        clearBoxes();
        auto b = getLocalBounds();
        auto controls = b.removeFromBottom (juce::jmin (150, b.getHeight() / 2));
        graph->setBounds (box (b, "RESPONSE"));

        auto lc = box (controls.removeFromLeft (controls.getWidth() / 3), "LOW CUT");
        auto lcLeft = lc.removeFromLeft (lc.getWidth() / 2);
        lcOn->setBounds (lcLeft.removeFromTop (kKeyHeight).reduced (2, 0));
        lcLeft.removeFromTop (6);
        lcSlope->setBounds (lcLeft.removeFromTop (kKeyHeight).reduced (2, 0));
        lcFreq->setBounds (lc);

        auto hc = box (controls.removeFromLeft (controls.getWidth() / 2), "HIGH CUT");
        auto hcLeft = hc.removeFromLeft (hc.getWidth() / 2);
        hcOn->setBounds (hcLeft.removeFromTop (kKeyHeight).reduced (2, 0));
        hcLeft.removeFromTop (6);
        hcSlope->setBounds (hcLeft.removeFromTop (kKeyHeight).reduced (2, 0));
        hcFreq->setBounds (hc);

        auto tool = box (controls, "TOOL FILTER");
        auto top = tool.removeFromTop (kKeyHeight);
        toolOn->setBounds (top.removeFromLeft (70).reduced (2, 0));
        toolModel->setBounds (top.reduced (2, 0));
        tool.removeFromTop (4);
        toolParams->setBounds (tool);
    }

private:
    BoolButton *lcOn = nullptr, *hcOn = nullptr, *toolOn = nullptr;
    DisplayKnob *lcFreq = nullptr, *hcFreq = nullptr;
    IntCombo *lcSlope = nullptr, *hcSlope = nullptr;
    ModelSelector* toolModel = nullptr;
    ModelParamGrid* toolParams = nullptr;
    ResponseGraph* graph = nullptr;
};

//==============================================================================
/** GATE or DYNAMICS slot: model, parameters, sidechain (and crossover). */
class DynTab final : public HomeScreen
{
public:
    DynTab (UiContext& c, Strip& s, bool gateSlot) : HomeScreen (c, s), isGate (gateSlot), slot (gateSlot ? s.params.gate : s.params.dynamics)
    {
        on = &toggle ("ON", slot.on);
        model = &make<ModelSelector>();
        const auto allowed = strip.getKind() == StripKind::Aux && ! isGate ? models::list (models::AuxDynModels) : juce::StringArray();
        model->bind (models::Dynamics, &slot.model,
                     [this] (int m) { strip.setModel (isGate ? ModelBlock::Gate : ModelBlock::Dynamics, m); }, allowed);
        mix = &knob (bindLinear ("MIX", slot.mix, 0.0f, 100.0f, "%", 0, 100.0f));
        gain = &knob (bindLinear ("GAIN", slot.gain, -6.0f, 12.0f, " dB", 1, 0.0f));
        gr = &make<GainReductionMeter> ([this] { return isGate ? strip.meters.gateReductionDb.load() : strip.meters.dynReductionDb.load(); },
                                        isGate ? 40.0f : 24.0f);
        params = &make<ModelParamGrid>();
        params->bind (models::Dynamics, &slot.model, slot.values);
        params->onModelChanged = [this] { if (knobsChanged) knobsChanged(); };

        scCaption = &label ("SIDECHAIN FILTER / SOURCE / TAP");
        scFilter = &combo (models::list (models::SidechainFilters), &slot.scFilter);
        scFreq = &knob (bindFreq ("SC FREQ", slot.scFreq, 20.0f, 20000.0f, 1000.0f));
        scQ = &knob (bindLog ("SC Q", slot.scQ, 0.5f, 10.0f, {}, 1.0f));
        scSource = &combo (channelNames (ctx.engine), &slot.scSource, "SELF");
        scTap = &combo (models::list (models::TapPoints), &slot.scTap);

        if (! isGate)
        {
            xoKind = &combo (models::list (models::XoverTypes), &slot.xoKind);
            xoFreq = &knob (bindFreq ("XO FREQ", slot.xoFreq, 20.0f, 20000.0f, 1000.0f));
            xoQ = &knob (bindLog ("XO Q", slot.xoQ, 0.5f, 10.0f, {}, 1.0f));
        }
    }

    std::vector<KnobBinding> knobs() override { return params->bindings (6); }

    void resized() override
    {
        clearBoxes();
        auto b = getLocalBounds();
        // Model bar: ON, the model, gain reduction.
        auto header = box (b.removeFromTop (52), {});
        on->setBounds (header.removeFromLeft (64).reduced (0, 1));
        header.removeFromLeft (8);
        gr->setBounds (header.removeFromRight (juce::jmin (180, header.getWidth() / 3)).reduced (0, 4));
        header.removeFromRight (8);
        model->setBounds (header);

        auto side = b.removeFromRight (juce::jmin (300, b.getWidth() * 2 / 5));
        auto sc = box (side.removeFromTop (isGate ? side.getHeight() : side.getHeight() * 3 / 5), "SIDECHAIN");
        scCaption->setBounds (sc.removeFromTop (14));
        layoutRow (sc.removeFromTop (kKeyHeight), { scFilter, scSource, scTap });
        sc.removeFromTop (4);
        layoutRow (sc.removeFromTop (juce::jmin (90, sc.getHeight())), { scFreq, scQ });
        if (! isGate)
        {
            auto xo = box (side, "CROSSOVER");
            auto row = xo.removeFromTop (kKeyHeight);
            xoKind->setBounds (row.removeFromLeft (row.getWidth() / 2).reduced (2, 0));
            xo.removeFromTop (2);
            layoutRow (xo, { xoFreq, xoQ });
        }

        auto bottom = b.removeFromBottom (juce::jlimit (110, 140, b.getHeight() / 3));
        layoutRow (box (bottom, "OUTPUT"), { mix, gain });
        params->setBounds (box (b, "PARAMETERS"));
    }

private:
    bool isGate;
    DynSlotParams& slot;
    BoolButton* on = nullptr;
    ModelSelector* model = nullptr;
    DisplayKnob *mix = nullptr, *gain = nullptr, *scFreq = nullptr, *scQ = nullptr, *xoFreq = nullptr, *xoQ = nullptr;
    GainReductionMeter* gr = nullptr;
    ModelParamGrid* params = nullptr;
    juce::Label* scCaption = nullptr;
    IntCombo *scFilter = nullptr, *scSource = nullptr, *scTap = nullptr, *xoKind = nullptr;
};

//==============================================================================
class EqTab final : public HomeScreen
{
public:
    EqTab (UiContext& c, Strip& s) : HomeScreen (c, s)
    {
        auto& eq = strip.params.eq;
        const auto table = static_cast<models::Table> (strip.getEqTable());
        on = &toggle ("ON", eq.on);
        model = &make<ModelSelector>();
        model->bind (table, &eq.model, [this] (int m) { strip.setModel (ModelBlock::Eq, m); });
        mix = &knob (bindLinear ("MIX", eq.mix, 0.0f, 125.0f, "%", 0, 100.0f));
        graph = &make<ResponseGraph>();
        graph->setStrip (&strip, ResponseGraph::Eq);
        graph->onBandSelected = [this] (int) { bindBand(); };
        params = &make<ModelParamGrid>();
        params->bind (table, &eq.model, eq.values);
        params->onModelChanged = [this]
        {
            updateBands();
            resized();
            if (knobsChanged)
                knobsChanged();
        };

        // The WING EQ: one key per band, the selected band on four knobs.
        bandKeys = &make<SegmentedKeys> (juce::StringArray(),
                                         [this] { return juce::jmax (0, graph->getSelectedBand()); },
                                         [this] (int i)
                                         {
                                             graph->setSelectedBand (i);
                                             bindBand();
                                         },
                                         Colours::cyan);
        for (auto*& k : bandKnobs)
            k = &make<DisplayKnob>();
        updateBands();
    }

    std::vector<KnobBinding> knobs() override
    {
        auto& eq = strip.params.eq;
        if (bands.isEmpty())
            return params->bindings (6);
        auto k = bandBindings();
        k.push_back ({});
        k.push_back (bindLinear ("EQ MIX", eq.mix, 0.0f, 125.0f, "%", 0, 100.0f));
        return k;
    }

    void resized() override
    {
        clearBoxes();
        auto b = getLocalBounds();
        auto header = box (b.removeFromTop (52), {});
        on->setBounds (header.removeFromLeft (64).reduced (0, 1));
        header.removeFromLeft (8);
        mix->setVisible (bands.isEmpty());
        if (bands.isEmpty())
            mix->setBounds (header.removeFromRight (90).withTrimmedTop (-6));
        model->setBounds (header);

        const bool wingEq = ! bands.isEmpty();
        params->setVisible (! wingEq);
        bandKeys->setVisible (wingEq);
        for (auto* k : bandKnobs)
            k->setVisible (wingEq);

        auto graphArea = b.removeFromTop (b.getHeight() * (wingEq ? 50 : 42) / 100);
        graph->setBounds (box (graphArea, {}));
        if (! wingEq)
        {
            params->setBounds (box (b, "PARAMETERS"));
            return;
        }

        auto band = box (b, "BAND");
        bandKeys->setBounds (band.removeFromTop (kKeyHeight + 4));
        band.removeFromTop (6);
        auto knobsRow = band.removeFromLeft (band.getWidth() * 5 / 6);
        const int w = knobsRow.getWidth() / 4;
        for (auto* k : bandKnobs)
            k->setBounds (knobsRow.removeFromLeft (w).reduced (4, 0));
        mix->setVisible (true);
        mix->setBounds (band.reduced (4, 0));
    }

private:
    void updateBands()
    {
        bands = ResponseGraph::stdBands (strip);
        juce::StringArray names;
        for (auto& id : bands)
            names.add (id == "l" ? "LOW" : (id == "h" ? "HIGH" : id.toUpperCase()));
        // SegmentedKeys has no setter for its labels: rebuild it in place.
        if (bandKeys != nullptr)
        {
            auto bounds = bandKeys->getBounds();
            owned.removeObject (bandKeys);
            bandKeys = &make<SegmentedKeys> (names,
                                             [this] { return juce::jmax (0, graph->getSelectedBand()); },
                                             [this] (int i)
                                             {
                                                 graph->setSelectedBand (i);
                                                 bindBand();
                                             },
                                             Colours::cyan);
            bandKeys->setBounds (bounds);
        }
        bindBand();
    }

    std::vector<KnobBinding> bandBindings() const
    {
        auto& eq = strip.params.eq;
        std::vector<KnobBinding> k;
        if (bands.isEmpty())
            return k;
        const auto& spec = models::model (static_cast<models::Table> (strip.getEqTable()), eq.model.load());
        const auto band = bands[juce::jlimit (0, bands.size() - 1, graph->getSelectedBand())];
        for (auto suffix : { "g", "f", "q", "eq" })
        {
            const int i = spec.find (band + suffix);
            k.push_back (i >= 0 ? bindModelParam (eq.values[i], spec.params[(size_t) i]) : KnobBinding {});
        }
        return k;
    }

    void bindBand()
    {
        const auto k = bandBindings();
        for (size_t i = 0; i < std::size (bandKnobs); ++i)
            if (bandKnobs[i] != nullptr)
                bandKnobs[i]->setBinding (i < k.size() ? k[i] : KnobBinding {});
        if (bandKeys != nullptr)
            bandKeys->repaint();
        if (knobsChanged)
            knobsChanged();
    }

    BoolButton* on = nullptr;
    ModelSelector* model = nullptr;
    DisplayKnob* mix = nullptr;
    ResponseGraph* graph = nullptr;
    ModelParamGrid* params = nullptr;
    SegmentedKeys* bandKeys = nullptr;
    DisplayKnob* bandKnobs[4] {};
    juce::StringArray bands;
};

//==============================================================================
class InsertTab final : public HomeScreen, private juce::Timer
{
public:
    InsertTab (UiContext& c, Strip& s, bool postInsert) : HomeScreen (c, s), post (postInsert), ins (post ? s.params.postInsert : s.params.preInsert)
    {
        on = &toggle ("ON", ins.on);
        slotCaption = &label ("FX SLOT");
        slotCombo = &make<IntCombo>();
        slotCombo->bind (&ins.fx);
        fillSlots();

        if (post)
        {
            modeCaption = &label ("MODE (FX or automix group)");
            mode = &combo (models::list (models::PostInsertModes), &ins.mode);
            weight = &knob (bindLinear ("AUTOMIX WEIGHT", ins.weight, -12.0f, 12.0f, " dB", 1, 0.0f));
        }

        status = &label ("", 13.0f, Colours::text);
        status->setJustificationType (juce::Justification::topLeft);
        openButton = &make<juce::TextButton> ("OPEN EDITOR");
        openButton->onClick = [this] { if (ins.fx.load() >= 0 && ctx.openPluginEditor) ctx.openPluginEditor (ins.fx.load()); };
        loadButton = &make<juce::TextButton> ("LOAD / REPLACE...");
        loadButton->onClick = [this] { if (ins.fx.load() >= 0) showFxSlotMenu (ctx, ins.fx.load(), loadButton); };
        effectsButton = &make<juce::TextButton> ("EFFECTS SCREEN");
        effectsButton->onClick = [this] { if (ctx.showScreen) ctx.showScreen (Screen::Effects); };
        mixKnob = &knob ({});
        startTimerHz (4);
        timerCallback();
    }

    void fillSlots()
    {
        juce::StringArray items;
        for (int i = 0; i < kNumFx; ++i)
            items.add (ctx.engine.getFx().describe (i));
        slotCombo->setItems (items, "NONE");
    }

    std::vector<KnobBinding> knobs() override
    {
        std::vector<KnobBinding> k;
        if (const int slot = ins.fx.load(); slot >= 0)
            k.push_back (bindLinear ("FX MIX", ctx.engine.getFx().params (slot).mix, 0.0f, 100.0f, "%", 0, 100.0f));
        else
            k.push_back ({});
        if (post)
            k.push_back (bindLinear ("WEIGHT", ins.weight, -12.0f, 12.0f, " dB", 1, 0.0f));
        return k;
    }

    void timerCallback() override
    {
        const int slot = ins.fx.load();
        auto& fx = ctx.engine.getFx();
        juce::String text;
        if (post && ins.mode.load() > 0)
            text << "Automix group " << (ins.mode.load() == 1 ? "X" : "Y") << ": gain is shared between the channels of the group.";
        else if (slot < 0)
            text << "No FX slot assigned.";
        else
        {
            text << fx.describe (slot) << "\n";
            if (! fx.isLoaded (slot))
                text << "The slot is empty: load a plugin into it.";
            else
            {
                text << "Load " << juce::roundToInt (fx.getLoad (slot) * 100.0f) << "%   users " << fx.getNumUsers (slot);
                if (fx.isFaulted (slot))
                    text << "\nFAULT: auto-bypassed (plugin failed or produced NaN)";
                if (fx.isDoubleAssigned (slot))
                    text << "\nDOUBLE ASSIGNED: another insert uses this slot";
            }
        }
        status->setText (text, juce::dontSendNotification);
        openButton->setEnabled (slot >= 0 && fx.isLoaded (slot));
        loadButton->setEnabled (slot >= 0);

        if (slot != shownSlot)
        {
            shownSlot = slot;
            mixKnob->setBinding (slot >= 0 ? bindLinear ("FX MIX", fx.params (slot).mix, 0.0f, 100.0f, "%", 0, 100.0f) : KnobBinding {});
            mixKnob->setVisible (slot >= 0);
            if (knobsChanged)
                knobsChanged();
        }
    }

    void resized() override
    {
        clearBoxes();
        auto b = getLocalBounds();
        auto assign = box (b.removeFromTop (post ? 140 : 84), post ? "POST FADER INSERT" : "PRE FADER INSERT");
        if (post)
            weight->setBounds (assign.removeFromRight (110));
        auto row = assign.removeFromTop (14);
        slotCaption->setBounds (row.withTrimmedLeft (66));
        row = assign.removeFromTop (kKeyHeight);
        on->setBounds (row.removeFromLeft (60).reduced (2, 0));
        slotCombo->setBounds (row.reduced (4, 0));
        if (post)
        {
            assign.removeFromTop (6);
            modeCaption->setBounds (assign.removeFromTop (14).withTrimmedLeft (66));
            auto r = assign.removeFromTop (kKeyHeight);
            r.removeFromLeft (64);
            mode->setBounds (r.reduced (4, 0));
        }

        auto slotArea = box (b, "SLOT");
        auto buttons = slotArea.removeFromBottom (kKeyHeight + 4);
        layoutRow (buttons.reduced (0, 2), { openButton, loadButton, effectsButton }, 6);
        mixKnob->setBounds (slotArea.removeFromRight (110).removeFromTop (100));
        status->setBounds (slotArea);
    }

private:
    bool post;
    InsertParams& ins;
    BoolButton* on = nullptr;
    IntCombo *slotCombo = nullptr, *mode = nullptr;
    juce::Label *slotCaption = nullptr, *modeCaption = nullptr, *status = nullptr;
    DisplayKnob *weight = nullptr, *mixKnob = nullptr;
    juce::TextButton *openButton = nullptr, *loadButton = nullptr, *effectsButton = nullptr;
    int shownSlot = -2;
};

//==============================================================================
class MainTab final : public HomeScreen
{
public:
    MainTab (UiContext& c, Strip& s) : HomeScreen (c, s)
    {
        auto& p = strip.params;
        faderKnob = &knob (bindFader ("FADER", p.faderDb));
        panKnob = &knob (bindPan ("PAN", p.pan));
        widthKnob = &knob (bindLinear ("WIDTH", p.width, -150.0f, 150.0f, "%", 0, 100.0f));

        const bool hasMainSends = strip.getKind() != StripKind::Main && strip.getKind() != StripKind::Matrix;
        if (hasMainSends)
        {
            for (int i = 0; i < kNumMains; ++i)
            {
                auto& m = p.mains[i];
                mainRows.push_back ({ &toggle ("M" + juce::String (i + 1), m.on, Colours::accent), &toggle ("PRE", m.pre, Colours::solo),
                                      &knob (bindFader ("LEVEL", m.levelDb)), nullptr });
            }
        }

        if (strip.getKind() != StripKind::Matrix)
        {
            for (int i = 0; i < kNumMatrices; ++i)
            {
                auto& m = p.matrix[i];
                mtxRows.push_back ({ &toggle ("MX" + juce::String (i + 1), m.on, Colours::accent), &toggle ("PRE", m.pre, Colours::solo),
                                     &knob (bindFader ("LVL", m.levelDb)), &knob (bindPan ("PAN", m.pan)) });
            }
        }
        else
        {
            directOn = &toggle ("DIRECT IN", p.directOn, Colours::accent);
            directInvert = &toggle (juce::String::fromUTF8 ("\xc3\x98 INVERT (MIX-MINUS)"), p.directInvert, Colours::mute);
            directPre = &toggle ("PRE FADER", p.directPre, Colours::solo);
            directSource = &combo (channelNames (ctx.engine), &p.directSource, "NONE");
            directLevel = &knob (bindFader ("LEVEL", p.directLevelDb));
        }

        dcas = &make<MaskButtons> (p.dcaMask, kNumDcas, "DCA", Colours::accent);
        muteGroups = &make<MaskButtons> (p.muteGroupMask, kNumMuteGroups, "", Colours::mute);
    }

    std::vector<KnobBinding> knobs() override
    {
        auto& p = strip.params;
        std::vector<KnobBinding> k { bindFader ("FADER", p.faderDb), bindPan ("PAN", p.pan),
                                     bindLinear ("WIDTH", p.width, -150.0f, 150.0f, "%", 0, 100.0f) };
        if (! mainRows.empty())
            for (int i = 0; i < 3; ++i)
                k.push_back (bindFader ("MAIN " + juce::String (i + 1), p.mains[i].levelDb));
        return k;
    }

    void resized() override
    {
        clearBoxes();
        auto b = getLocalBounds();
        auto groups = b.removeFromBottom (92);
        auto dcaArea = box (groups.removeFromLeft (groups.getWidth() * 2 / 3), "DCA ASSIGN");
        dcas->setBounds (dcaArea);
        muteGroups->setBounds (box (groups, "MUTE GROUPS"));

        auto left = b.removeFromLeft (b.getWidth() * 2 / 5);
        auto out = box (left.removeFromTop (juce::jmin (100, left.getHeight() * 2 / 5)), "OUTPUT");
        layoutRow (out, { faderKnob, panKnob, widthKnob });

        if (! mainRows.empty())
        {
            auto mains = box (left, "MAIN SENDS");
            const int w = mains.getWidth() / kNumMains;
            for (auto& row : mainRows)
            {
                auto cell = mains.removeFromLeft (w).reduced (3, 0);
                row.on->setBounds (cell.removeFromTop (22));
                cell.removeFromTop (2);
                row.pre->setBounds (cell.removeFromTop (22));
                row.level->setBounds (cell);
            }
        }

        if (! mtxRows.empty())
        {
            auto mtx = box (b, "MATRIX SENDS");
            const int columns = 4;
            const int w = mtx.getWidth() / columns, h = mtx.getHeight() / 2;
            for (size_t i = 0; i < mtxRows.size(); ++i)
            {
                auto cell = juce::Rectangle<int> (mtx.getX() + (int) (i % columns) * w, mtx.getY() + (int) (i / columns) * h, w, h).reduced (3);
                auto& row = mtxRows[i];
                auto buttons = cell.removeFromTop (22);
                row.on->setBounds (buttons.removeFromLeft (buttons.getWidth() / 2).reduced (1, 0));
                row.pre->setBounds (buttons.reduced (1, 0));
                row.level->setBounds (cell.removeFromLeft (cell.getWidth() / 2));
                row.pan->setBounds (cell);
            }
        }
        else if (directOn != nullptr)
        {
            auto d = box (b, "DIRECT INPUT");
            directOn->setBounds (d.removeFromTop (kKeyHeight).removeFromLeft (d.getWidth() / 2).reduced (2, 0));
            d.removeFromTop (4);
            directSource->setBounds (d.removeFromTop (kKeyHeight).reduced (2, 0));
            d.removeFromTop (4);
            auto r = d.removeFromTop (kKeyHeight);
            directInvert->setBounds (r.removeFromLeft (r.getWidth() * 2 / 3).reduced (2, 0));
            directPre->setBounds (r.reduced (2, 0));
            directLevel->setBounds (d.withSizeKeepingCentre (juce::jmin (100, d.getWidth()), d.getHeight()));
        }
    }

private:
    struct SendRow
    {
        BoolButton* on;
        BoolButton* pre;
        DisplayKnob* level;
        DisplayKnob* pan;
    };

    DisplayKnob *faderKnob = nullptr, *panKnob = nullptr, *widthKnob = nullptr, *directLevel = nullptr;
    std::vector<SendRow> mainRows, mtxRows;
    BoolButton *directOn = nullptr, *directInvert = nullptr, *directPre = nullptr;
    IntCombo* directSource = nullptr;
    MaskButtons *dcas = nullptr, *muteGroups = nullptr;
};

//==============================================================================
class SendsTab final : public HomeScreen
{
public:
    SendsTab (UiContext& c, Strip& s) : HomeScreen (c, s)
    {
        tapCaption = &label ("TAP (pre-fader sends)");
        tap = &combo (models::list (models::TapPoints), &strip.params.tap);

        viewport = &make<juce::Viewport>();
        viewport->setViewedComponent (&rows, false);
        viewport->setScrollBarsShown (true, false);

        for (auto& bus : ctx.engine.getBuses())
        {
            if (bus.get() == &strip)
                continue;
            if (strip.isBus() && ! (strip.getId() >= kBusToBusTargets && bus->getId() < kBusToBusTargets))
                continue;

            auto& send = strip.sends[(size_t) bus->getId()];
            Row row;
            row.bus = bus.get();
            row.name = new juce::Label ({}, describeStrip (*bus));
            row.name->setColour (juce::Label::textColourId, bus->getColour());
            row.name->setFont (juce::FontOptions (12.0f, juce::Font::bold));
            row.on = new BoolButton ("ON", Colours::accent);
            row.on->bind (&send.enabled);
            row.level = new DisplayKnob();
            row.level->setCompact (true);
            row.level->setBinding (bindFader ("LEVEL", send.levelDb));
            row.mode = new IntCombo();
            row.mode->setItems (models::list (models::SendModes));
            row.mode->bind (&send.mode);
            row.pan = new DisplayKnob();
            row.pan->setCompact (true);
            row.pan->setBinding (bindPan ("PAN", send.pan));
            row.link = new BoolButton ("LINK", Colours::solo);
            row.link->bind (&send.panLink);
            for (juce::Component* comp : { (juce::Component*) row.name, (juce::Component*) row.on, (juce::Component*) row.level,
                                           (juce::Component*) row.mode, (juce::Component*) row.pan, (juce::Component*) row.link })
            {
                rowComponents.add (comp);
                rows.addAndMakeVisible (comp);
            }
            sendRows.push_back (row);
        }

        if (sendRows.empty())
            empty = &label (strip.isBus() ? "Buses 1-8 don't send to other buses (buses 9+ can send to buses 1-8)."
                                          : "This strip has no bus sends.",
                            13.0f, Colours::textDim);
    }

    std::vector<KnobBinding> knobs() override
    {
        std::vector<KnobBinding> k;
        for (size_t i = 0; i < sendRows.size() && k.size() < 6; ++i)
        {
            auto b = bindFader (sendRows[i].bus->getLabel(), strip.sends[(size_t) sendRows[i].bus->getId()].levelDb);
            k.push_back (b);
        }
        return k;
    }

    void resized() override
    {
        clearBoxes();
        auto b = getLocalBounds();
        auto top = box (b.removeFromTop (54), "");
        tapCaption->setBounds (top.removeFromLeft (160));
        tap->setBounds (top.removeFromLeft (140).withSizeKeepingCentre (140, kKeyHeight));

        auto list = box (b, "SENDS TO BUSES");
        viewport->setBounds (list);
        if (empty != nullptr)
            empty->setBounds (list);

        const int w = list.getWidth() - viewport->getScrollBarThickness();
        const int rowH = 62;
        for (size_t i = 0; i < sendRows.size(); ++i)
        {
            auto r = juce::Rectangle<int> (0, (int) i * rowH, w, rowH - 4);
            auto& row = sendRows[i];
            row.name->setBounds (r.removeFromLeft (juce::jmax (120, w / 4)));
            row.on->setBounds (r.removeFromLeft (60).withSizeKeepingCentre (54, kKeyHeight));
            row.level->setBounds (r.removeFromLeft (90));
            row.mode->setBounds (r.removeFromLeft (90).withSizeKeepingCentre (84, kKeyHeight));
            row.pan->setBounds (r.removeFromLeft (80));
            row.link->setBounds (r.removeFromLeft (64).withSizeKeepingCentre (56, kKeyHeight));
        }
        rows.setSize (w, (int) sendRows.size() * rowH);
    }

private:
    struct Row
    {
        Strip* bus;
        juce::Label* name;
        BoolButton* on;
        DisplayKnob* level;
        IntCombo* mode;
        DisplayKnob* pan;
        BoolButton* link;
    };

    juce::Label* tapCaption = nullptr;
    juce::Label* empty = nullptr;
    IntCombo* tap = nullptr;
    juce::Viewport* viewport = nullptr;
    juce::Component rows;
    juce::OwnedArray<juce::Component> rowComponents;
    std::vector<Row> sendRows;
};

} // namespace

std::unique_ptr<HomeScreen> makeInputScreen (UiContext& c, Strip& s) { return std::make_unique<InputTab> (c, s); }
std::unique_ptr<HomeScreen> makeFilterScreen (UiContext& c, Strip& s) { return std::make_unique<FilterTab> (c, s); }
std::unique_ptr<HomeScreen> makeDynamicsScreen (UiContext& c, Strip& s, bool gate) { return std::make_unique<DynTab> (c, s, gate); }
std::unique_ptr<HomeScreen> makeEqScreen (UiContext& c, Strip& s) { return std::make_unique<EqTab> (c, s); }
std::unique_ptr<HomeScreen> makeInsertScreen (UiContext& c, Strip& s, bool post) { return std::make_unique<InsertTab> (c, s, post); }
std::unique_ptr<HomeScreen> makeMainScreen (UiContext& c, Strip& s) { return std::make_unique<MainTab> (c, s); }
std::unique_ptr<HomeScreen> makeSendsScreen (UiContext& c, Strip& s) { return std::make_unique<SendsTab> (c, s); }

} // namespace livedaw::ui
