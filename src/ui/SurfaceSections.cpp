#include "ui/SurfaceSections.h"
#include "daw/Session.h"
#include "daw/TimeGrid.h"
#include "ui/MidiRemoteUi.h"
#include "dsp/DspUtils.h"
#include "ui/FaderStrip.h"
#include "ui/ResponseGraph.h"
#include "ui/UsbRecorderPage.h"

namespace livedaw::ui
{

namespace
{
    // The LED panel's meters (manual fig. 15).
    constexpr float kLevelThresholds[] = { -0.1f, -3.0f, -6.0f, -12.0f, -18.0f, -24.0f, -36.0f, -48.0f };
    constexpr const char* kLevelLabels[] = { "CLIP", "-3", "-6", "-12", "-18", "-24", "-36", "-48" };
    constexpr float kGrThresholds[] = { 2.0f, 4.0f, 6.0f, 8.0f, 12.0f, 15.0f, 18.0f, 21.0f };

    KnobBinding delayBinding (InputParams& in)
    {
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

    /** Draws a printed connector line between two points of the panel. */
    void drawConnector (juce::Graphics& g, float x1, float x2, float y)
    {
        if (x2 - x1 > 4.0f)
        {
            g.setColour (Colours::silk.withAlpha (0.55f));
            g.fillRect (x1 + 2.0f, y - 0.75f, x2 - x1 - 4.0f, 1.5f);
        }
    }
} // namespace

//==============================================================================
ChannelStripSection::ChannelStripSection (UiContext& c) : ctx (c)
{
    const char* names[kNumSlots] = { "INPUT", "FILTER", "GATE", "COMP", "INSERT", "MAIN" };
    for (int i = 0; i < kNumSlots; ++i)
    {
        slotKnobs[i].setHardware (true);
        slotKnobs[i].onTouch = [this, i] { showSlot (i); followScreen (i, true); };
        addAndMakeVisible (slotKnobs[i]);

        auto& key = *(slotKeys[i] = std::make_unique<SurfaceKey> (names[i]));
        key.ledColour = Colours::block;
        key.setTooltip (juce::String (names[i]) + ": press to show it on the screen (again: next parameter), hold to switch it on/off");
        key.onPress = [this, i] { pressSlot (i); };
        key.onHold = [this, i] { toggleSlot (i); };
        key.ledState = [this, i] { return slotOn (i) ? SurfaceKey::Lit : SurfaceKey::Off; };
        addAndMakeVisible (key);
    }

    for (int i = 0; i < 4; ++i)
    {
        eqKnobs[i].setHardware (true);
        eqKnobs[i].onTouch = [this, i]
        {
            eqBand = i;
            eqMixShown = false;
            showSlot (EqSlot);
            followScreen (EqSlot, true);
        };
        addAndMakeVisible (eqKnobs[i]);
    }
    eqMix.setHardware (true);
    eqMix.onTouch = [this]
    {
        eqMixShown = true;
        showSlot (EqSlot);
        followScreen (EqSlot, true);
    };
    addAndMakeVisible (eqMix);

    shiftKey.shape = SurfaceKey::Shape::Square;
    shiftKey.ledColour = Colours::silk;
    shiftKey.setTooltip ("SHIFT: the shelving filters (bus EQs: bands 5-8)");
    shiftKey.onPress = [this] { eqShift = ! eqShift; bindEq(); };
    shiftKey.ledState = [this] { return eqShift ? SurfaceKey::Lit : SurfaceKey::Off; };

    eqKey.ledColour = Colours::block;
    eqKey.setTooltip ("EQ: press to show it on the screen, hold to switch it on/off");
    eqKey.onPress = [this] { eqMixShown = false; showSlot (EqSlot); followScreen (EqSlot, false); };
    eqKey.onHold = [this] { if (strip != nullptr) strip->params.eq.on = ! strip->params.eq.on.load(); };
    eqKey.ledState = [this] { return strip != nullptr && strip->params.eq.on.load() ? SurfaceKey::Lit : SurfaceKey::Off; };

    SurfaceKey* elements[] = { &gainKey, &widthKey, &freqKey };
    for (int i = 0; i < 3; ++i)
    {
        auto& key = *elements[i];
        key.shape = SurfaceKey::Shape::Square;
        key.ledColour = Colours::silk;
        key.onPress = [this, i] { eqElement = i; eqMixShown = false; bindEq(); showSlot (EqSlot); };
        key.ledState = [this, i] { return eqElement == i ? SurfaceKey::Lit : SurfaceKey::Off; };
    }

    // LISTEN (EQ-BAND/KEY): the selected EQ band on the solo bus (the band
    // knob last touched), or the key of the GATE / COMP slot while one is
    // shown. Press again to stop.
    listenKey.dark = true;
    listenKey.setTooltip ("LISTEN: the selected EQ band (with GATE or COMP shown: its key) on the solo bus");
    auto listenTarget = [this]() -> std::pair<int, int>
    {
        if (shownSlot == Gate)
            return { ld::kListenGateKey, 0 };
        if (shownSlot == Comp)
            return { ld::kListenDynKey, 0 };
        // The band on the knob last touched, as bindEq lays them out.
        const auto bands = ResponseGraph::stdBands (*strip);
        juce::StringArray shown;
        for (auto& b : bands)
            if (b != "l" && b != "h")
                shown.add (b);
        if (eqShift)
        {
            shown.removeRange (0, 4);
            shown.add ("l");
            shown.add ("h");
        }
        return { ld::kListenBand, bands.indexOf (shown[eqBand]) };
    };
    listenKey.onPress = [this, listenTarget]
    {
        if (strip == nullptr)
            return;
        Analysis analysis (*strip);
        const auto [mode, band] = listenTarget();
        if (analysis.isListening (*strip, mode) && (mode != ld::kListenBand || analysis.controls.listenBand.load() == band))
            analysis.stopListening();
        else if (band >= 0)
            analysis.listen (*strip, mode, band);
    };
    listenKey.ledState = [this]
    {
        if (strip == nullptr)
            return (int) SurfaceKey::Off;
        Analysis analysis (*strip);
        for (int mode : { ld::kListenBand, ld::kListenGateKey, ld::kListenDynKey, ld::kListenXover })
            if (analysis.isListening (*strip, mode))
                return (int) SurfaceKey::Lit;
        return (int) SurfaceKey::Off;
    };

    viewKey.setTooltip ("VIEW: the shown slot on the HOME screen");
    viewKey.onPress = [this]
    {
        if (ctx.showHomeTab)
            ctx.showHomeTab (homeTabOf (shownSlot));
        viewOpened = true;
    };
    viewKey.isActive = [this]
    {
        // Lit from the press until the display leaves HOME.
        viewOpened = viewOpened && ctx.getScreen && ctx.getScreen() == Screen::Home;
        return viewOpened;
    };

    for (juce::Component* k : { (juce::Component*) &shiftKey, (juce::Component*) &eqKey, (juce::Component*) &listenKey,
                                (juce::Component*) &gainKey, (juce::Component*) &widthKey, (juce::Component*) &freqKey,
                                (juce::Component*) &viewKey })
        addAndMakeVisible (k);

    startTimerHz (20);
    refresh();
}

void ChannelStripSection::refresh()
{
    strip = ctx.getSelectedStrip ? ctx.getSelectedStrip() : nullptr;
    const bool channel = strip != nullptr && strip->getKind() == StripKind::Channel;
    for (int i = 0; i < kNumSlots; ++i)
    {
        slotKeys[i]->setEnabled (strip != nullptr && ((i != Filter && i != Gate) || channel));
        if (strip != nullptr)
        {
            // Gate and compressor start on their threshold.
            if (i == Gate || i == Comp)
            {
                auto& slot = i == Gate ? strip->params.gate : strip->params.dynamics;
                paramIndex[i] = juce::jmax (0, models::model (models::Dynamics, slot.model.load()).find ("thr"));
            }
            else
            {
                paramIndex[i] = 0;
            }
        }
        bindSlot (i);
    }
    // Buses have no FILTER or GATE slot.
    if (shownSlot < kNumSlots && ! slotKeys[shownSlot]->isEnabled())
        shownSlot = Input;
    if (strip != nullptr)
    {
        auto& p = strip->params;
        const int models[] = { p.filter.toolModel.load(), p.gate.model.load(), p.dynamics.model.load(), p.preInsert.fx.load() };
        std::copy (std::begin (models), std::end (models), std::begin (boundModels));
    }
    eqModel = -1;
    bindEq();
    shownState = {};
    repaint();
}

std::vector<KnobBinding> ChannelStripSection::slotParams (int slot) const
{
    std::vector<KnobBinding> list;
    if (strip == nullptr)
        return list;
    auto& p = strip->params;
    const bool channel = strip->getKind() == StripKind::Channel;

    auto modelParams = [&list] (models::Table table, int model, std::atomic<float>* values)
    {
        const auto& spec = models::model (table, model);
        for (size_t i = 0; i < spec.params.size(); ++i)
            list.push_back (bindModelParam (values[i], spec.params[i]));
    };

    switch (slot)
    {
        case Input:
            list.push_back (bindLinear ("TRIM", p.input.trimDb, -18.0f, 18.0f, " dB", 1, 0.0f));
            list.push_back (bindLinear ("BALANCE", p.input.balanceDb, -9.0f, 9.0f, " dB", 1, 0.0f));
            list.push_back (delayBinding (p.input));
            break;
        case Filter:
            if (channel)
            {
                list.push_back (bindFreq ("LO CUT", p.filter.lowCutFreq, 20.0f, 2000.0f, 80.0f));
                list.push_back (bindFreq ("HI CUT", p.filter.highCutFreq, 200.0f, 20000.0f, 10000.0f));
                modelParams (models::FilterTools, p.filter.toolModel.load(), p.filter.toolParams);
            }
            break;
        case Gate:
            if (channel)
                modelParams (models::Dynamics, p.gate.model.load(), p.gate.values);
            break;
        case Comp:
            modelParams (models::Dynamics, p.dynamics.model.load(), p.dynamics.values);
            break;
        case Insert:
            if (const int fx = p.preInsert.fx.load(); fx >= 0)
                list.push_back (bindLinear ("FX MIX", ctx.engine.getFx().params (fx).mix, 0.0f, 100.0f, "%", 0, 100.0f));
            break;
        case Main:
            list.push_back (bindPan ("PAN", p.pan));
            list.push_back (bindLinear ("WIDTH", p.width, -150.0f, 150.0f, "%", 0, 100.0f));
            list.push_back (bindFader ("MAIN 1", p.mains[0].levelDb));
            break;
        default:
            break;
    }
    return list;
}

void ChannelStripSection::bindSlot (int slot)
{
    auto list = slotParams (slot);
    numParams[slot] = (int) list.size();
    if (list.empty())
    {
        slotKnobs[slot].setBinding ({});
        return;
    }
    paramIndex[slot] = juce::jlimit (0, (int) list.size() - 1, paramIndex[slot]);
    slotKnobs[slot].setBinding (list[(size_t) paramIndex[slot]]);
}

void ChannelStripSection::showSlot (int slot)
{
    if (shownSlot != slot)
    {
        shownSlot = slot;
        repaint (screenArea);
    }
}

HomeTab ChannelStripSection::homeTabOf (int slot)
{
    static const HomeTab tabs[] = { HomeTab::Input, HomeTab::Filter, HomeTab::Gate, HomeTab::Dynamics, HomeTab::PreInsert, HomeTab::Main, HomeTab::Eq };
    return tabs[juce::jlimit (0, (int) EqSlot, slot)];
}

void ChannelStripSection::followScreen (int slot, bool touch)
{
    // SCREEN FOLLOWS CH STRIP: the display shows the block being edited here;
    // CH STRIP TOUCH SELECT: touching an encoder does too (SETUP > SURFACE).
    const bool follow = ctx.settings.getBoolValue (uiprefs::screenFollowsStrip, false)
                        || (touch && ctx.settings.getBoolValue (uiprefs::stripTouchSelect, false));
    if (follow && strip != nullptr && ctx.showHomeTab)
        ctx.showHomeTab (homeTabOf (slot));
}

void ChannelStripSection::pressSlot (int slot)
{
    // Pressed again: the next parameter on the encoder above.
    if (shownSlot == slot && numParams[slot] > 1)
    {
        paramIndex[slot] = (paramIndex[slot] + 1) % numParams[slot];
        bindSlot (slot);
    }
    showSlot (slot);
    followScreen (slot, false);
    repaint (screenArea);
}

bool ChannelStripSection::slotOn (int slot) const
{
    if (strip == nullptr)
        return false;
    auto& p = strip->params;
    switch (slot)
    {
        case Input:  return p.input.invert.load();
        case Filter: return p.filter.lowCut.load() || p.filter.highCut.load() || p.filter.tool.load();
        case Gate:   return p.gate.on.load();
        case Comp:   return p.dynamics.on.load();
        case Insert: return p.preInsert.on.load() && p.preInsert.fx.load() >= 0;
        case Main:   return p.mains[0].on.load();
        default:     return false;
    }
}

void ChannelStripSection::toggleSlot (int slot)
{
    if (strip == nullptr)
        return;
    auto& p = strip->params;
    auto flip = [] (std::atomic<bool>& b) { b = ! b.load(); };
    switch (slot)
    {
        case Input:  flip (p.input.invert); break;
        case Filter: flip (p.filter.lowCut); break;
        case Gate:   flip (p.gate.on); break;
        case Comp:   flip (p.dynamics.on); break;
        case Insert: flip (p.preInsert.on); break;
        case Main:   flip (p.mains[0].on); break;
        default:     break;
    }
    showSlot (slot);
}

void ChannelStripSection::bindEq()
{
    for (auto& k : eqKnobs)
        k.setBinding ({});
    eqMix.setBinding ({});
    eqKey.setEnabled (strip != nullptr);
    if (strip == nullptr)
        return;

    auto& eq = strip->params.eq;
    eqModel = eq.model.load();
    eqMix.setBinding (bindLinear ("BLEND/MIX", eq.mix, 0.0f, 125.0f, "%", 0, 100.0f));
    const auto& spec = models::model (static_cast<models::Table> (strip->getEqTable()), eqModel);
    const auto bands = ResponseGraph::stdBands (*strip);

    if (bands.isEmpty())
    {
        // Analog models: their parameters, four at a time.
        for (int i = 0; i < 4; ++i)
        {
            const int index = (eqShift ? 4 : 0) + i;
            if (index < (int) spec.params.size())
                eqKnobs[i].setBinding (bindModelParam (eq.values[index], spec.params[(size_t) index]));
        }
        return;
    }

    // WING EQ: bands 1-4, SHIFT for the shelves (and bus bands 5-8).
    juce::StringArray shown;
    for (auto& b : bands)
        if (b != "l" && b != "h")
            shown.add (b);
    if (eqShift)
    {
        juce::StringArray shifted;
        for (int i = 4; i < shown.size(); ++i)
            shifted.add (shown[i]);
        shifted.add ("l");
        shifted.add ("h");
        shown = shifted;
    }

    const char* suffix = eqElement == 0 ? "g" : (eqElement == 1 ? "q" : "f");
    for (int i = 0; i < 4 && i < shown.size(); ++i)
    {
        const int index = spec.find (shown[i] + suffix);
        if (index >= 0)
        {
            auto b = bindModelParam (eq.values[index], spec.params[(size_t) index]);
            const auto band = shown[i] == "l" ? juce::String ("LOW SHELF") : (shown[i] == "h" ? juce::String ("HIGH SHELF") : "BAND " + shown[i].toUpperCase());
            b.label = band + " " + spec.params[(size_t) index].label.upToFirstOccurrenceOf (" ", false, false);
            eqKnobs[i].setBinding (b);
        }
    }
}

void ChannelStripSection::timerCallback()
{
    if (strip != nullptr && strip->params.eq.model.load() != eqModel)
        bindEq();

    // Model changes (filter tool, gate, comp, insert FX) change the slots' parameter lists.
    if (strip != nullptr)
    {
        auto& p = strip->params;
        const int models[] = { p.filter.toolModel.load(), p.gate.model.load(), p.dynamics.model.load(), p.preInsert.fx.load() };
        const int slots[] = { Filter, Gate, Comp, Insert };
        for (int i = 0; i < 4; ++i)
        {
            if (models[i] != boundModels[i])
            {
                boundModels[i] = models[i];
                bindSlot (slots[i]);
            }
        }
    }

    const auto state = panelState();
    if (state != shownState)
    {
        shownState = state;
        repaint (panelArea);
    }
}

juce::String ChannelStripSection::panelState() const
{
    // Everything the LED panel and its screen show, to repaint only on changes.
    if (strip == nullptr)
        return "-";
    juce::String s;
    auto& p = strip->params;
    auto bit = [] (bool b) { return b ? '1' : '0'; };
    for (float thr : kLevelThresholds)
        for (int ch = 0; ch < 2; ++ch)
            s << bit (dsp::gainToDb (ctx.meters.get (strip, true, ch)) >= thr);
    s << juce::roundToInt (strip->meters.dynReductionDb.load()) << bit (slotOn (Input)) << bit (p.filter.lowCut.load())
      << bit (p.filter.highCut.load()) << bit (p.filter.tool.load()) << p.filter.toolModel.load() << bit (p.input.delayOn.load())
      << bit (p.stereo.load()) << bit (strip->isUsingAltInput()) << (int) p.dcaMask.load() << ':' << (int) p.muteGroupMask.load() << ':' << p.postInsert.mode.load();
    for (auto& m : p.mains)
        s << bit (m.on.load());
    for (int i = 0; i < 16; ++i)
        if (auto* send = strip->sends.find (i))
            s << (send->enabled.load() ? i : 0) << ',';
    s << stripName (*strip) << strip->getColourIndex() << shownSlot << eqBand << bit (eqMixShown) << bit (eqShift);
    auto knob = shownSlot == EqSlot ? (eqMixShown ? &eqMix : &eqKnobs[eqBand]) : &slotKnobs[shownSlot];
    if (knob->getBinding().isValid())
        s << knob->getBinding().label << knob->getBinding().text();
    return s;
}

void ChannelStripSection::paint (juce::Graphics& g)
{
    g.fillAll (Colours::panel);
    g.setColour (Colours::outline);
    g.drawRect (getLocalBounds().removeFromRight (1));

    paintPanel (g);

    drawSilk (g, processingHeading.toFloat(), "CHANNEL PROCESSING", 12.0f);
    drawSilk (g, eqHeading.toFloat(), "CHANNEL EQUALIZER", 12.0f);

    // Printed lines joining the slot keys, and SHIFT to the band encoders.
    for (int i = 0; i + 1 < kNumSlots; ++i)
        drawConnector (g, (float) slotKeys[i]->getRight(), (float) slotKeys[i + 1]->getX(), (float) slotKeys[i]->getBounds().getCentreY());
    const float eqY = (float) eqKnobs[0].getBounds().getCentreY();
    drawConnector (g, (float) shiftKey.getRight(), (float) eqKnobs[0].getX(), eqY);
    for (int i = 0; i < 3; ++i)
        drawConnector (g, (float) eqKnobs[i].getRight(), (float) eqKnobs[i + 1].getX(), eqY);

    // Captions.
    const float small = 9.0f;
    auto below = [] (juce::Component& c, int h) { return c.getBounds().withY (c.getBottom() + 1).withHeight (h).expanded (16, 0).toFloat(); };
    drawSilk (g, below (*slotKeys[Input], 12), "(HOLD FOR ON/OFF)", small);
    drawSilk (g, below (eqKey, 12), "(HOLD FOR ON/OFF)", small);
    drawSilk (g, shiftKey.getBounds().withHeight (12).translated (0, -13).expanded (8, 0).toFloat(), "SHIFT", small);
    drawSilk (g, below (shiftKey, 12), "5-8", small);
    drawSilk (g, below (gainKey, 12), "GAIN", small);
    drawSilk (g, below (widthKey, 12), "WIDTH", small);
    drawSilk (g, below (freqKey, 12), "FREQ", small);
    drawSilk (g, below (listenKey, 12), "EQ-BAND/KEY", small);
    drawSilk (g, below (eqMix, 12), "BLEND/MIX", small);
    for (int i = 0; i < 4; ++i)
    {
        // "[1] 5": the band digit inverted; the selected band's box lit.
        auto cap = below (eqKnobs[i], 12).withSizeKeepingCentre (26.0f, 12.0f);
        auto box = cap.removeFromLeft (12.0f);
        const bool selected = shownSlot == EqSlot && ! eqMixShown && eqBand == i;
        g.setColour (selected ? Colours::yellow : Colours::silk);
        g.fillRect (box);
        drawSilk (g, box, juce::String (i + 1), small, juce::Justification::centred, Colours::panel);
        drawSilk (g, cap, juce::String (i + 5), small, juce::Justification::centred, eqShift ? Colours::yellow : Colours::silk);
    }

    // The black legend plates beside GAIN / WIDTH / FREQ.
    for (auto [plate, icons] : { std::pair { legendLow, std::array<Icon, 3> { Icon::LowCut, Icon::Bell, Icon::LowShelf } },
                                 std::pair { legendHigh, std::array<Icon, 3> { Icon::HighShelf, Icon::Bell, Icon::HighCut } } })
    {
        g.setColour (juce::Colours::black);
        g.fillRoundedRectangle (plate.toFloat(), 3.0f);
        auto r = plate.toFloat().reduced (4.0f, 2.0f);
        const float w = r.getWidth() / 3.0f;
        for (auto icon : icons)
            drawIcon (g, icon, r.removeFromLeft (w).reduced (1.0f), Colours::silk);
    }
}

void ChannelStripSection::paintPanel (juce::Graphics& g)
{
    auto panel = panelArea.toFloat();
    g.setColour (juce::Colours::black);
    g.fillRoundedRectangle (panel, 5.0f);

    auto r = panel.reduced (8.0f, 5.0f);
    const float titleH = juce::jlimit (11.0f, 16.0f, r.getHeight() * 0.13f);
    const float rowH = (r.getHeight() - titleH - 2.0f) / 8.0f;
    const float font = juce::jlimit (6.5f, 11.0f, juce::jmin (rowH * 0.78f, r.getWidth() * 0.0185f));
    const float titleFont = juce::jlimit (8.0f, 13.0f, titleH * 0.95f);
    const float led = juce::jlimit (3.0f, 7.0f, rowH * 0.42f);
    const float top = r.getY() + titleH + 2.0f;
    auto rowY = [&] (int row) { return top + rowH * (float) row; };
    auto ledAt = [&] (float x, int row, bool on, juce::Colour colour = juce::Colours::white)
    {
        g.setColour (on ? colour : Colours::ledOff);
        g.fillRect (x, rowY (row) + (rowH - led * 0.75f) * 0.5f, led, led * 0.75f);
    };
    auto text = [&] (float x, float w, int row, const juce::String& t, juce::Justification j = juce::Justification::centredLeft)
    {
        drawSilk (g, { x, rowY (row), w, rowH }, t, font, j, Colours::silk);
    };
    auto title = [&] (float x, float w, const juce::String& t, juce::Justification j = juce::Justification::centred)
    {
        drawSilk (g, { x, r.getY(), w, titleH }, t, titleFont, j, Colours::silk);
    };
    auto rule = [&] (float x)
    {
        g.setColour (Colours::silk.withAlpha (0.7f));
        g.fillRect (x, top, 1.5f, rowH * 8.0f);
    };

    const float W = r.getWidth();
    float x = r.getX();
    auto column = [&] (float fraction) { const float c = x; x += W * fraction; return c; };
    const float details = column (0.37f), level = column (0.095f), screen = column (0.215f), gr = column (0.075f);
    const float busSend = column (0.105f), groups = column (0.14f);

    // With no strip selected every LED is off.
    const bool any = strip != nullptr;
    StripParams* p = any ? &strip->params : nullptr;

    // CHANNEL STRIP DETAILS: two LED columns.
    {
        title (details, W * 0.36f, "CHANNEL STRIP DETAILS", juce::Justification::centredLeft);
        const float c1 = details, c2 = details + W * 0.165f, labelW = W * 0.15f;
        const bool input = any && strip->isInput();
        juce::String toolToken;
        if (any && p->filter.tool.load())
            toolToken = models::model (models::FilterTools, p->filter.toolModel.load()).token;
        const std::tuple<float, int, const char*, bool> items[] = {
            { c1, 0, "MAIN SOURCE", input && ! strip->isUsingAltInput() },
            { c1, 1, "ALT SOURCE", input && strip->isUsingAltInput() },
            { c1, 3, "48 V", false },
            { c1, 4, "INVERT", any && p->input.invert.load() },
            { c1, 6, "STEREO", any && (strip->isOutput() || p->stereo.load()) },
            { c1, 7, "MONO", any && strip->isInput() && ! p->stereo.load() },
            { c2, 0, "24 dB LOW-CUT", any && p->filter.lowCut.load() },
            { c2, 1, "12 dB HIGH-CUT", any && p->filter.highCut.load() },
            { c2, 2, "ALLPASS", toolToken.startsWith ("AP") },
            { c2, 3, "TILT EQUALIZER", toolToken == "TILT" },
            { c2, 4, "SOUND MAXER", toolToken == "MAX" },
            { c2, 7, "INPUT DELAY", any && p->input.delayOn.load() },
        };
        for (auto [cx, row, label, on] : items)
        {
            ledAt (cx, row, on);
            text (cx + led + 4.0f, labelW, row, label);
        }
        rule (c2 - W * 0.012f);
        rule (level - W * 0.01f);
    }

    // LEVEL: input level, stereo.
    {
        title (level, W * 0.085f, "LEVEL");
        const int channels = any ? juce::jlimit (1, 2, strip->getNumChannels()) : 1;
        for (int ch = 0; ch < 2; ++ch)
        {
            const float db = any ? dsp::gainToDb (ctx.meters.get (strip, true, juce::jmin (ch, channels - 1))) : -200.0f;
            for (int i = 0; i < 8; ++i)
                ledAt (level + (float) ch * (led + 3.0f), i, db >= kLevelThresholds[i], leds::meterColour (kLevelThresholds[i]));
        }
        for (int i = 0; i < 8; ++i)
            text (level + led * 2.0f + 6.0f, W * 0.05f, i, kLevelLabels[i]);
    }

    // The section's screen.
    screenArea = juce::Rectangle<float> (screen, r.getY() + 1.0f, W * 0.205f, r.getHeight() - 2.0f).toNearestInt();
    paintScreen (g, screenArea.toFloat());
    // SETUP > GENERAL > BTN LCD: the channel strip screen's brightness.
    if (const float b = uiprefs::brightness (ctx.settings, "btnLcd", 80); b < 1.0f)
    {
        g.setColour (juce::Colours::black.withAlpha (juce::jlimit (0.0f, 0.8f, 1.0f - b)));
        g.fillRect (screenArea);
    }

    // GR: gain reduction of the compressor, lit from the top.
    {
        title (gr, W * 0.07f, "GR");
        const float reduction = any ? strip->meters.dynReductionDb.load() : 0.0f;
        for (int i = 0; i < 8; ++i)
        {
            text (gr, W * 0.04f, i, "-" + juce::String ((int) kGrThresholds[i]), juce::Justification::centredRight);
            ledAt (gr + W * 0.045f, i, reduction >= kGrThresholds[i], Colours::meterYellow);
        }
        rule (busSend - W * 0.008f);
    }

    // BUS SEND: the sends to buses 1-16 that are on.
    {
        title (busSend, W * 0.1f, "BUS SEND");
        for (int i = 0; i < 16; ++i)
        {
            const int row = i / 2;
            const bool left = i % 2 == 0;
            auto* send = any ? strip->sends.find (i) : nullptr;
            const bool on = send != nullptr && send->enabled.load() && ctx.engine.getBusInSlot (i) != nullptr;
            const float lx = busSend + W * (left ? 0.03f : 0.055f);
            ledAt (lx, row, on);
            if (left)
                text (busSend, W * 0.027f, row, juce::String (i + 1), juce::Justification::centredRight);
            else
                text (lx + led + 3.0f, W * 0.03f, row, juce::String (i + 1));
        }
    }

    // GROUPS: mains, automix, DCAs and mute groups.
    {
        title (groups, W * 0.14f, "GROUPS");
        const float step = led + juce::jmax (3.0f, W * 0.012f);
        auto rowOf = [&] (int row, int count, std::function<bool (int)> on)
        {
            for (int i = 0; i < count; ++i)
                ledAt (groups + step * (float) i, row, on (i));
        };
        rowOf (0, 4, [&] (int i) { return any && p->mains[i].on.load(); });
        const float labelX = groups + step * 4.0f + 1.0f, labelW = W * 0.06f;
        text (labelX, labelW, 0, "MAIN");

        const int automix = any ? p->postInsert.mode.load() : 0;
        text (groups, step, 1, "X", juce::Justification::centred);
        ledAt (groups + step, 1, automix == (int) PostInsertMode::AutomixX);
        text (groups + step * 2.0f, step, 1, "Y", juce::Justification::centred);
        ledAt (groups + step * 3.0f, 1, automix == (int) PostInsertMode::AutomixY);
        text (labelX, labelW, 1, "AUTO");

        // Eight DCA LEDs for 16 DCAs: LED i shows DCA i or i + 8.
        const auto dcas = any ? p->dcaMask.load() : 0u;
        rowOf (3, 4, [&] (int i) { return ((dcas >> i) | (dcas >> (i + 8))) & 1u; });
        rowOf (4, 4, [&] (int i) { return ((dcas >> (i + 4)) | (dcas >> (i + 12))) & 1u; });
        drawSilk (g, { labelX, rowY (3), labelW, rowH * 2.0f }, "DCA", font, juce::Justification::centredLeft, Colours::silk);
        const auto mutes = any ? p->muteGroupMask.load() : 0u;
        rowOf (6, 4, [&] (int i) { return (mutes >> i) & 1u; });
        rowOf (7, 4, [&] (int i) { return (mutes >> (i + 4)) & 1u; });
        drawSilk (g, { labelX, rowY (6), labelW, rowH * 2.0f }, "MUTE", font, juce::Justification::centredLeft, Colours::silk);
    }
}

void ChannelStripSection::paintScreen (juce::Graphics& g, juce::Rectangle<float> area)
{
    g.setColour (juce::Colour (0xff101113));
    g.fillRect (area);
    g.setColour (juce::Colour (0xff4a4c50));
    g.drawRect (area, 1.0f);
    auto r = area.reduced (3.0f);
    if (strip == nullptr)
    {
        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawFittedText ("PRESS SELECT", r.toNearestInt(), juce::Justification::centred, 2);
        return;
    }

    // The selected channel: number in a black box, the name on its colour.
    const auto colour = strip->getColour();
    auto tile = r.removeFromTop (juce::jmax (14.0f, r.getHeight() * 0.26f));
    g.setColour (colour);
    g.fillRect (tile);
    const auto number = strip->getLabel().removeCharacters (" ");
    g.setFont (juce::FontOptions (juce::jlimit (9.0f, 16.0f, tile.getHeight() * 0.75f), juce::Font::bold));
    auto box = tile.removeFromLeft (juce::jmin (tile.getWidth() * 0.4f,
                                                juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), number) + 8.0f)).reduced (1.0f);
    g.setColour (juce::Colours::black);
    g.fillRect (box);
    g.setColour (colour);
    g.drawText (number, box, juce::Justification::centred);
    g.setColour (colour.getPerceivedBrightness() > 0.45f ? juce::Colours::black : juce::Colours::white);
    g.drawFittedText (stripName (*strip), tile.reduced (3.0f, 0.0f).toNearestInt(), juce::Justification::centredLeft, 1, 0.7f);

    // The slot, the parameter on its encoder and the value.
    static const char* slotNames[] = { "INPUT", "FILTER", "GATE", "COMP", "INSERT", "MAIN", "EQ" };
    const bool eq = shownSlot == EqSlot;
    const auto& knob = eq ? (eqMixShown ? eqMix : eqKnobs[eqBand]) : slotKnobs[shownSlot];
    const bool on = eq ? strip->params.eq.on.load() : slotOn (shownSlot);
    r.removeFromTop (2.0f);
    auto head = r.removeFromTop (r.getHeight() * 0.28f);
    g.setFont (juce::FontOptions (juce::jlimit (9.0f, 15.0f, head.getHeight() * 0.8f), juce::Font::bold));
    g.setColour (Colours::text);
    g.drawText (slotNames[juce::jlimit (0, 6, shownSlot)], head, juce::Justification::centredLeft);
    drawChip (g, head.removeFromRight (juce::jmin (26.0f, head.getWidth() * 0.3f)).reduced (0.0f, head.getHeight() * 0.12f),
              on ? "ON" : "OFF", on ? Colours::yellow : Colours::screenHeader, on ? juce::Colours::black : Colours::textDim);

    if (const auto& b = knob.getBinding(); b.isValid())
    {
        auto label = r.removeFromTop (r.getHeight() * 0.4f);
        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (juce::jlimit (8.0f, 13.0f, label.getHeight() * 0.8f), juce::Font::bold));
        g.drawFittedText (b.label.toUpperCase(), label.toNearestInt(), juce::Justification::centredLeft, 1, 0.7f);
        auto value = r.withTrimmedBottom (r.getHeight() * 0.18f);
        g.setColour (Colours::text);
        g.setFont (juce::FontOptions (juce::jlimit (10.0f, 22.0f, value.getHeight() * 0.85f), juce::Font::bold));
        g.drawFittedText (b.text ? b.text() : juce::String(), value.toNearestInt(), juce::Justification::centredLeft, 1, 0.7f);
    }

    // Dots: how many parameters the slot's encoder steps through.
    const int n = eq ? 0 : numParams[shownSlot];
    if (n > 1)
    {
        const float d = juce::jlimit (2.0f, 4.0f, area.getHeight() * 0.03f);
        float dx = area.getRight() - 4.0f - (float) n * (d + 2.0f);
        for (int i = 0; i < n; ++i, dx += d + 2.0f)
        {
            g.setColour (i == paramIndex[shownSlot] ? Colours::text : Colours::screenTile);
            g.fillEllipse (dx, area.getBottom() - 4.0f - d, d, d);
        }
    }
}

void ChannelStripSection::resized()
{
    auto b = getLocalBounds().reduced (8, 6);
    panelArea = b.removeFromTop (juce::jlimit (96, 200, b.getHeight() * 30 / 100));
    b.removeFromTop (4);

    // Six columns, with VIEW in a margin on the left.
    const int margin = juce::jlimit (34, 60, b.getWidth() / 10);
    auto viewColumn = b.removeFromLeft (margin);
    const float colW = (float) b.getWidth() / (float) kNumSlots;
    auto colCentre = [&] (float i) { return b.getX() + juce::roundToInt (colW * (i + 0.5f)); };

    const int h = b.getHeight();
    const int headingH = 14, keyH = juce::jlimit (18, 28, h / 15), captionH = 12;
    const int knob = juce::jlimit (26, 64, juce::jmin ((int) (colW * 0.62f), (h - 2 * headingH - 2 * keyH - 3 * captionH - 30) / 2));
    const int keyW = juce::jmin ((int) (colW * 0.66f), 64);

    // Spread what's left over the gaps between the rows.
    const int used = 2 * headingH + 2 * (knob + 4) + 2 * keyH + 2 + 3 * captionH + 18;
    const int gap = juce::jlimit (0, 30, (h - used) / 5);
    b.removeFromTop (gap / 2);

    processingHeading = b.removeFromTop (headingH);
    auto knobRow = b.removeFromTop (knob + 4);
    for (int i = 0; i < kNumSlots; ++i)
        slotKnobs[i].setBounds (juce::Rectangle<int> (knob, knob).withCentre ({ colCentre ((float) i), knobRow.getCentreY() }));
    const int viewSize = juce::jmin (margin - 4, 44);
    viewKey.setBounds (juce::Rectangle<int> (viewSize, viewSize).withCentre ({ viewColumn.getCentreX(), knobRow.getCentreY() }));

    b.removeFromTop (4 + gap / 2);
    auto keyRow = b.removeFromTop (keyH);
    for (int i = 0; i < kNumSlots; ++i)
        slotKeys[i]->setBounds (juce::Rectangle<int> (keyW, keyH).withCentre ({ colCentre ((float) i), keyRow.getCentreY() }));
    b.removeFromTop (captionH + 2 + gap);

    eqHeading = b.removeFromTop (headingH);
    b.removeFromTop (captionH - 4 + gap / 2); // room for "SHIFT" over its key
    auto eqRow = b.removeFromTop (knob + 4);
    const int square = juce::jmin (keyH + 4, knob);
    shiftKey.setBounds (juce::Rectangle<int> (square, square).withCentre ({ colCentre (0.0f), eqRow.getCentreY() }));
    for (int i = 0; i < 4; ++i)
        eqKnobs[i].setBounds (juce::Rectangle<int> (knob, knob).withCentre ({ colCentre ((float) i + 1.0f), eqRow.getCentreY() }));
    eqMix.setBounds (juce::Rectangle<int> (knob, knob).withCentre ({ colCentre (5.0f), eqRow.getCentreY() }));
    b.removeFromTop (captionH + 4 + gap);

    // EQ - legend - GAIN WIDTH FREQ - legend - LISTEN.
    auto bottom = b.removeFromTop (keyH + 2);
    const int cy = bottom.getCentreY();
    eqKey.setBounds (juce::Rectangle<int> (keyW, keyH).withCentre ({ colCentre (0.0f), cy }));
    listenKey.setBounds (juce::Rectangle<int> (keyW, keyH).withCentre ({ colCentre (5.0f), cy }));
    const int sq = keyH + 2;
    const float pitch = colW * 0.62f;
    gainKey.setBounds (juce::Rectangle<int> (sq, sq).withCentre ({ colCentre (2.5f) - juce::roundToInt (pitch), cy }));
    widthKey.setBounds (juce::Rectangle<int> (sq, sq).withCentre ({ colCentre (2.5f), cy }));
    freqKey.setBounds (juce::Rectangle<int> (sq, sq).withCentre ({ colCentre (2.5f) + juce::roundToInt (pitch), cy }));
    const int plateW = juce::jmin ((int) (colW * 0.8f), 76);
    legendLow = juce::Rectangle<int> (plateW, juce::jmax (12, keyH - 6)).withCentre ({ colCentre (1.0f), cy });
    legendHigh = juce::Rectangle<int> (plateW, juce::jmax (12, keyH - 6)).withCentre ({ colCentre (4.0f), cy });
}

//==============================================================================
MonitorSection::MonitorSection (UiContext& c) : ctx (c)
{
    viewKey.setTooltip ("VIEW: the MONITORS screen (hold: the USB recorder / player)");
    viewKey.onPress = [this]
    {
        if (ctx.showMonitors)
            ctx.showMonitors();
        viewOpened = true;
    };
    // Held: the USB Recorder / Player (manual p. 20, table 4.2).
    viewKey.onHold = [this]
    {
        if (ctx.showScreen)
            ctx.showScreen (Screen::UsbRecorder);
        viewOpened = true;
    };
    viewKey.isActive = [this]
    {
        // Lit from the press until the display leaves SETUP (or the USB recorder).
        viewOpened = viewOpened && ctx.getScreen && (ctx.getScreen() == Screen::Setup || ctx.getScreen() == Screen::UsbRecorder);
        return viewOpened;
    };
    addAndMakeVisible (viewKey);

    for (auto* k : { &talkLevel, &phonesLevel, &monitorLevel })
    {
        k->setHardware (true);
        addAndMakeVisible (k);
    }
    // MONITOR LEVEL: the SPEAKERS path; PHONES LEVEL: the PHONES path (the
    // LEVEL faders on MONITORS follow them).
    monitorLevel.setBinding (bindLinear ("MONITOR LEVEL", ctx.engine.monitorLevelDb, -60.0f, 10.0f, " dB", 1, 0.0f));
    phonesLevel.setBinding (bindLinear ("PHONES LEVEL", ctx.engine.phones.levelDb, -60.0f, 10.0f, " dB", 1, 0.0f));
    talkLevel.setBinding (bindLinear ("TALK LEVEL", ctx.engine.talk.levelDb, -60.0f, 10.0f, " dB", 1, 0.0f));
    talkLevel.setTooltip ("TALK LEVEL: the talkback level (unless the path uses the talkback channel's send levels)");
    phonesLevel.setTooltip ("PHONES LEVEL: the level of the PHONES monitor path");

    // TALK A / TALK B: talk while held (PUSH), toggle (LATCH) or either (AUTO),
    // as the path's SWITCH MODE on the MONITORS screen says.
    for (int path = 0; path < 2; ++path)
    {
        auto& k = path == 0 ? talkA : talkB;
        k.shape = SurfaceKey::Shape::Square;
        k.ledColour = Colours::yellow;
        k.onTalk = [this, path] (bool down) { ctx.engine.talkKey (path, down); };
        // Lit while talking; blinking while the key is on without a talkback channel.
        k.ledState = [this, path]
        {
            if (! ctx.engine.isTalkOn (path))
                return SurfaceKey::Off;
            return ctx.engine.getTalkStrip() != nullptr ? SurfaceKey::Lit : SurfaceKey::Blink;
        };
        k.setTooltip (juce::String ("TALK ") + (path == 0 ? "A" : "B") + ": talkback to the path's buses, matrices and mains (SETUP > TALKBACK picks the channel)");
        addAndMakeVisible (k);
    }

    // DIM attenuates the monitor by the DIM amount (lit); held, it mutes the
    // monitor (blinking). MONO sums the monitor to mono (lit); held, it swaps
    // left and right (blinking). A press while blinking disengages (manual 4.7).
    auto monitorKey = [this] (SurfaceKey& key, std::atomic<bool>& pressed, std::atomic<bool>& held, const juce::String& tip)
    {
        key.shape = SurfaceKey::Shape::Square;
        key.ledColour = Colours::yellow;
        key.setTooltip (tip);
        key.onPress = [&pressed, &held]
        {
            if (held.load())
                held = false;
            else
                pressed = ! pressed.load();
        };
        key.onHold = [&held] { held = ! held.load(); };
        key.ledState = [&pressed, &held] { return held.load() ? SurfaceKey::Blink : (pressed.load() ? SurfaceKey::Lit : SurfaceKey::Off); };
        addAndMakeVisible (key);
    };
    monitorKey (dimKey, ctx.engine.monitorDim, ctx.engine.monitorMute, "DIM: attenuate the monitor by the DIM amount (hold: mute the monitor)");
    monitorKey (monoKey, ctx.engine.monitorMono, ctx.engine.monitorSwap, "MONO: sum the monitor to mono (hold: swap left and right)");
}

void MonitorSection::paint (juce::Graphics& g)
{
    g.fillAll (Colours::panel);

    g.setColour (Colours::outline);
    g.drawRoundedRectangle (wingPanel.toFloat(), 6.0f, 1.0f);
    drawSilk (g, usbTitle.toFloat(), "USB AUDIO/DATA", 12.0f);
    drawSilk (g, monitorTitle.toFloat(), "MONITORING/TALKBACK", 12.0f);
    g.setColour (Colours::silk.withAlpha (0.6f));
    g.fillRect (monitorTitle.getX() + monitorTitle.getWidth() / 10, monitorTitle.getY() - 8, monitorTitle.getWidth() * 8 / 10, 1);

    // The USB socket plate: socket, 5V 1000mA, the USB mark and the ACCESS LED.
    auto plate = usbPlate.toFloat();
    g.setColour (juce::Colours::black);
    g.fillRoundedRectangle (plate, 3.0f);
    auto inner = plate.reduced (6.0f, 4.0f);
    auto caption = inner.removeFromTop (inner.getHeight() * 0.3f);
    drawSilk (g, caption.withWidth (caption.getWidth() * 0.55f), "5V  1000mA", 8.0f, juce::Justification::centredLeft);
    drawSilk (g, caption.withTrimmedLeft (caption.getWidth() * 0.6f), "ACCESS", 8.0f, juce::Justification::centredLeft);
    auto socket = inner.removeFromLeft (inner.getWidth() * 0.5f).reduced (2.0f, 1.0f);
    g.setColour (Colours::silk);
    g.drawRoundedRectangle (socket, 2.0f, 1.5f);
    g.drawRect (socket.reduced (socket.getWidth() * 0.18f, socket.getHeight() * 0.3f), 1.0f);
    drawIcon (g, Icon::Usb, inner.removeFromLeft (inner.getWidth() * 0.4f), Colours::silk);
    g.setColour (Colours::ledOff.brighter (0.2f));
    g.fillRect (inner.withSizeKeepingCentre (juce::jmin (16.0f, inner.getWidth() * 0.8f), 5.0f));

    // Captions under the pots and keys.
    auto below = [] (juce::Component& c, int h) { return c.getBounds().withY (c.getBottom() + 3).withHeight (h).expanded (24, 0).toFloat(); };
    drawSilk (g, below (talkLevel, 12), "TALK LEVEL", 10.0f);
    drawSilk (g, below (phonesLevel, 12), "PHONES LEVEL", 10.0f);
    drawSilk (g, below (monitorLevel, 12), "MONITOR LEVEL", 10.0f);
    drawSilk (g, below (talkA, 12), "TALK A", 9.0f);
    drawSilk (g, below (talkB, 12), "TALK B", 9.0f);
    drawSilk (g, below (dimKey, 12), "ATTENUATE", 9.0f);
    drawSilk (g, below (monoKey, 12), "MONO", 9.0f);
}

void MonitorSection::resized()
{
    auto b = getLocalBounds().reduced (8, 6);
    wingPanel = b;

    // VIEW top left; USB AUDIO/DATA with its plate; MONITORING/TALKBACK below a rule.
    auto p = wingPanel.reduced (10, 8);
    const int viewSize = juce::jlimit (30, 44, p.getWidth() / 9);
    viewKey.setBounds (p.getX(), p.getY(), viewSize, viewSize);
    usbTitle = p.removeFromTop (16).withTrimmedLeft (viewSize);
    const int plateH = juce::jlimit (30, 52, p.getHeight() / 7);
    usbPlate = p.removeFromTop (plateH + 4).withSizeKeepingCentre (juce::jmin (150, p.getWidth() / 2), plateH).translated (viewSize / 3, 0);
    p.removeFromTop (juce::jmax (18, p.getHeight() / 8));
    monitorTitle = p.removeFromTop (16);

    const int keyH = juce::jlimit (24, 40, p.getHeight() / 6);
    auto keys = p.removeFromBottom (keyH + 16).withTrimmedBottom (16);
    p.removeFromBottom (p.getHeight() / 8);
    const int pot = juce::jlimit (32, 84, juce::jmin (p.getWidth() / 5, p.getHeight() - 24));
    auto pots = p.withSizeKeepingCentre (p.getWidth(), pot).translated (0, -6);
    const int third = pots.getWidth() / 3;
    talkLevel.setBounds (juce::Rectangle<int> (pot, pot).withCentre ({ pots.getX() + third / 2, pots.getCentreY() }));
    phonesLevel.setBounds (juce::Rectangle<int> (pot, pot).withCentre ({ pots.getCentreX(), pots.getCentreY() }));
    monitorLevel.setBounds (juce::Rectangle<int> (pot, pot).withCentre ({ pots.getRight() - third / 2, pots.getCentreY() }));

    // A  B  (gap)  DIM  MONO
    const float slot = (float) keys.getWidth() / 5.0f;
    SurfaceKey* row[] = { &talkA, &talkB, nullptr, &dimKey, &monoKey };
    for (int i = 0; i < 5; ++i)
        if (row[i] != nullptr)
            row[i]->setBounds (juce::Rectangle<int> (keyH, keyH).withCentre ({ keys.getX() + juce::roundToInt (slot * ((float) i + 0.5f)), keys.getCentreY() }));
}

//==============================================================================
CustomControlSection::CustomControlSection (UiContext& c) : ctx (c)
{
    // Upper part: 16 layers of four encoders and eight keys (CC EDIT assigns
    // them); VIEW opens CC EDIT on the layer shown.
    upperView.setTooltip ("VIEW: CUSTOM CONTROLS (CC EDIT) for this layer");
    upperView.onPress = [this]
    {
        if (ctx.showCustomControls)
            ctx.showCustomControls (currentLayer());
        viewOpened = true;
    };
    upKey.onPress = [this] { stepLayer (1); };
    downKey.onPress = [this] { stepLayer (-1); };
    upKey.setTooltip ("Next custom control layer");
    downKey.setTooltip ("Previous custom control layer");
    for (auto& e : encoders)
    {
        e.setHardware (true);
        addAndMakeVisible (e);
    }
    for (int i = 0; i < 8; ++i)
    {
        // Keys 0-3: the upper key of each control, 4-7: the lower one.
        auto* k = new TalkKey();
        upperKeys.add (k);
        k->shape = SurfaceKey::Shape::Square;
        k->onPress = [this, i]
        {
            if (upperIsDaw())
                midiui::pressUpperKey (ctx, i, true);
            else
                cc::press (ctx, ccFunction (currentLayer(), i));
        };
        k->onTalk = [this, i] (bool down)
        {
            if (down)
                return;
            if (upperIsDaw())
                midiui::pressUpperKey (ctx, i, false);
            else
                cc::release (ctx, ccFunction (currentLayer(), i));
        };
        k->ledState = [this, i] { return upperIsDaw() ? midiui::upperKeyLed (ctx, i) : cc::ledState (ctx, ccFunction (currentLayer(), i)); };
        addAndMakeVisible (k);
    }

    // Lower part: its layer keys and eight keys.
    const char* layerNames[kNumLowerLayers] = { "USER", "2-TRACK\nTRANSP", "MULTI\nTRANSP", "MUTE\nGROUPS", "SHOW\nCNTRL" };
    for (int i = 0; i < kNumLowerLayers; ++i)
    {
        auto* k = layerKeys.add (new SurfaceKey (layerNames[i]));
        k->ledColour = Colours::silk;
        k->onPress = [this, i] { setLowerLayer (i); };
        k->ledState = [this, i] { return lowerLayer == i ? SurfaceKey::Lit : SurfaceKey::Off; };
        addAndMakeVisible (k);
    }
    layerKeys[TwoTrack]->setTooltip ("2-TRACK TRANSP: the USB recorder / player's transport (VIEW: the USB recorder screen)");
    layerKeys[MultiTrack]->setTooltip ("MULTI TRANSP: the multitrack recorder's transport (VIEW: the recorder screen)");
    layerKeys[User]->setTooltip ("USER: eight custom control keys (CC EDIT, layer USER)");

    for (int i = 0; i < 8; ++i)
    {
        auto* k = new TalkKey();
        lowerKeys.add (k);
        k->shape = SurfaceKey::Shape::Square;
        k->onPress = [this, i] { pressLowerKey (i); };
        k->onTalk = [this, i] (bool down)
        {
            if (const int l = lowerCcLayer(); l >= 0 && ! down)
                cc::release (ctx, ccFunction (l, i));
        };
        k->ledState = [this, i]
        {
            if (const int l = lowerCcLayer(); l >= 0)
                return cc::ledState (ctx, ccFunction (l, i));
            return lowerKeyLit (i) ? (int) SurfaceKey::Lit : (int) SurfaceKey::Off;
        };
        addAndMakeVisible (k);
    }

    lowerView.setTooltip ("VIEW: the MUTE GROUPS screen, the scenes for SHOW CNTRL, the recorder for MULTI TRANSP, the USB recorder for 2-TRACK TRANSP");
    lowerView.onPress = [this]
    {
        // USER and the DAW layers: their CC EDIT page.
        if (const int l = lowerCcLayer(); l >= 0 && ctx.showCustomControls)
            ctx.showCustomControls (l);
        else if (ctx.showScreen && lowerLayer == MuteGroups)
            ctx.showScreen (Screen::MuteGroups);
        else if (ctx.showScreen && lowerLayer == ShowControl)
            ctx.showScreen (Screen::Library);
        else if (ctx.showScreen && lowerLayer == MultiTrack)
            ctx.showScreen (Screen::Recorder);
        else if (ctx.showScreen && lowerLayer == TwoTrack)
            ctx.showScreen (Screen::UsbRecorder);
        viewOpened = true;
    };
    lowerView.isActive = [this]
    {
        const auto screen = ctx.getScreen ? ctx.getScreen() : Screen::Home;
        viewOpened = viewOpened && ((lowerLayer == MuteGroups && screen == Screen::MuteGroups)
                                    || (lowerLayer == ShowControl && screen == Screen::Library)
                                    || (lowerLayer == MultiTrack && screen == Screen::Recorder)
                                    || (lowerLayer == TwoTrack && screen == Screen::UsbRecorder));
        return viewOpened || screen == Screen::MuteGroups;
    };

    // REMOTE CONTROL / DAW: DAW remote control (SETUP > DAW > DAW ENABLE): the
    // fader sections and the custom controls control the DAW. Without DAW
    // ENABLE, and held, it opens the multitrack editor.
    dawKey.dark = true;
    dawKey.onPress = [this]
    {
        if (! midiui::pressDawKey (ctx) && ctx.showEditor)
            ctx.showEditor (true);
    };
    dawKey.onHold = [this] { if (ctx.showEditor) ctx.showEditor (true); };
    dawKey.ledState = [this] { return midiui::dawModeOn (ctx) ? SurfaceKey::Lit : SurfaceKey::Off; };
    dawKey.ledColour = Colours::silk;
    dawKey.setTooltip ("DAW: DAW remote control on / off (SETUP > DAW > DAW ENABLE; otherwise the multitrack editor). Hold: the multitrack editor");

    // Navigation arrows and the wheel. With USE CRSR/WHEEL (SETUP > GENERAL)
    // they move the display's focus frame and edit the focused control;
    // otherwise the wheel picks the next scene (SHOW CNTRL) or scrubs the
    // recorder (MULTI TRANSP).
    const int arrows[] = { -2, 2, -1, 1 };
    SurfaceKey* nav[] = { &navUp, &navDown, &navLeft, &navRight };
    for (int i = 0; i < 4; ++i)
    {
        nav[i]->arrow = arrows[i];
        nav[i]->shape = SurfaceKey::Shape::Square;
        nav[i]->setTooltip ("Navigation arrows: move the focus on the display (SETUP > GENERAL > USE CRSR/WHEEL)");
        const int dx = arrows[i] == -1 ? -1 : arrows[i] == 1 ? 1 : 0, dy = arrows[i] == -2 ? -1 : arrows[i] == 2 ? 1 : 0;
        nav[i]->onPress = [this, dx, dy]
        {
            if (midiui::dawModeOn (ctx))
                midiui::pressArrow (ctx, dx, dy);
            else if (useCursorWheel() && ctx.moveScreenFocus)
                ctx.moveScreenFocus (dx, dy);
        };
        addAndMakeVisible (nav[i]);
    }
    wheel.setHardware (true);
    wheel.setTooltip ("Wheel: edits the focused control (USE CRSR/WHEEL), picks the next scene (SHOW CNTRL) or scrubs the recorder (MULTI TRANSP)");
    wheel.setBinding (endlessBinding ("WHEEL", wheelPos, [this] (int steps) { turnWheel (steps); }));
    addAndMakeVisible (wheel);

    for (juce::Component* k : { (juce::Component*) &upperView, (juce::Component*) &lowerView, (juce::Component*) &upKey,
                                (juce::Component*) &downKey, (juce::Component*) &dawKey })
        addAndMakeVisible (k);

    setLowerLayer (MuteGroups);
    startTimerHz (8);
}

void CustomControlSection::setLowerLayer (int layer)
{
    lowerLayer = layer;
    if (const int l = lowerCcLayer(); l >= 0)
    {
        // USER or a DAW layer: the layer's eight keys.
        for (int i = 0; i < lowerKeys.size(); ++i)
        {
            const auto name = lowerKeyName (i);
            const auto control = ctx.customControls != nullptr ? ctx.customControls->get (l, i % cc::kNumControls) : cc::Control {};
            lowerKeys[i]->setLabel (name);
            lowerKeys[i]->setIcon (std::nullopt);
            lowerKeys[i]->setEnabled (name.isNotEmpty());
            lowerKeys[i]->ledColour = control.colour > 0 ? wingColour (control.colour) : Colours::silk;
            lowerKeys[i]->setTooltip (cc::layerName (l) + ": " + name);
        }
        shownState = {};
        repaint();
        return;
    }
    // MULTI TRANSP: PREV / NEXT marker, MARK, LOOP over START, STOP, PLAY, REC.
    static const Icon transportIcons[8] = { Icon::MarkerPrev, Icon::MarkerNext, Icon::MarkerAdd, Icon::Loop,
                                            Icon::ReturnToStart, Icon::Stop, Icon::Play, Icon::Record };
    static const char* transportTips[8] = { "Previous marker", "Next marker", "Add a marker at the position", "Loop on/off",
                                            "Return to the start", "Stop", "Play", "Record the armed tracks (again: stop)" };
    // 2-TRACK TRANSP (the USB recorder / player): < > clips, NEW FILE, REPEAT
    // over PAUSE, STOP, PLAY, REC.
    static const Icon usbIcons[8] = { Icon::ChevronLeft, Icon::ChevronRight, Icon::Plus, Icon::Loop,
                                      Icon::Pause, Icon::Stop, Icon::Play, Icon::Record };
    static const char* usbTips[8] = { "Previous clip", "Next clip", "NEW FILE: the recording goes on in a new file", "REPEAT the playlist",
                                      "Pause / go on (the player)", "Stop the player", "Play", "Record the RECORDER outputs (again: stop)" };
    for (int i = 0; i < lowerKeys.size(); ++i)
    {
        const auto name = lowerKeyName (i);
        const bool transport = layer == MultiTrack || layer == TwoTrack;
        lowerKeys[i]->setLabel (transport ? juce::String() : name);
        lowerKeys[i]->setIcon (transport ? std::optional<Icon> (layer == TwoTrack ? usbIcons[i] : transportIcons[i]) : std::nullopt);
        lowerKeys[i]->setEnabled (transport || name.isNotEmpty());
        lowerKeys[i]->ledColour = layer == MuteGroups ? Colours::mute
                                  : transport && i == 7 ? Colours::mute
                                  : transport && i == 6 ? Colours::meterGreen
                                                        : Colours::silk;
        lowerKeys[i]->setTooltip (layer == MuteGroups ? "Mute group " + juce::String (i + 1) + ": " + ctx.engine.getMuteGroupName (i)
                                  : layer == TwoTrack ? juce::String (usbTips[i])
                                  : transport         ? juce::String (transportTips[i])
                                                      : juce::String());
    }
    shownState = {};
    repaint();
}

juce::String CustomControlSection::lowerKeyName (int key) const
{
    if (const int l = lowerCcLayer(); l >= 0)
        return cc::describe (ctx, key < 4 ? cc::Slot::Upper : cc::Slot::Lower, ccFunction (l, key));
    switch (lowerLayer)
    {
        case User: return cc::describe (ctx, key < 4 ? cc::Slot::Upper : cc::Slot::Lower, ccFunction (cc::kUserLayer, key));
        case MuteGroups: return juce::String (key + 1);
        case ShowControl:
        {
            static const char* names[] = { "PREV", "GO", "SCENES" };
            return key < 3 ? names[key] : "";
        }
        case MultiTrack:
        {
            static const char* names[] = { "PREV", "NEXT", "MARK", "LOOP", "START", "STOP", "PLAY", "REC" };
            return names[juce::jlimit (0, 7, key)];
        }
        case TwoTrack:
        {
            static const char* names[] = { "PREV", "NEXT", "NEW FILE", "REPEAT", "PAUSE", "STOP", "PLAY", "REC" };
            return names[juce::jlimit (0, 7, key)];
        }
        default: return {};
    }
}

void CustomControlSection::pressLowerKey (int key)
{
    if (const int l = lowerCcLayer(); l >= 0)
    {
        cc::press (ctx, ccFunction (l, key));
        return;
    }
    if (lowerLayer == MuteGroups)
    {
        ctx.engine.muteGroups = ctx.engine.muteGroups.load() ^ (1u << key);
    }
    else if (lowerLayer == ShowControl)
    {
        if (key == 0)
            logRecall (ctx, "previous scene", ctx.show.recallPrevious());
        else if (key == 1)
        {
            // GO: the scene the wheel picked, else the next one.
            if (sceneCursor >= 0)
                logRecall (ctx, "scene " + juce::String (sceneCursor + 1), ctx.show.recallScene (sceneCursor));
            else
                logRecall (ctx, "next scene", ctx.show.recallNext());
            sceneCursor = -1;
        }
        else if (key == 2 && ctx.showScreen)
            ctx.showScreen (Screen::Library);
    }
    else if (lowerLayer == TwoTrack)
    {
        auto& usb = ctx.engine.getUsbRecorder();
        switch (key)
        {
            case 0: usb.previous(); break;
            case 1: usb.next(); break;
            case 2:
                if (auto r = usb.newFile(); r.failed())
                    ctx.log.warning ("USB recorder: " + r.getErrorMessage());
                break;
            case 3: usb.setRepeat (! usb.getRepeat()); break;
            case 4: usb.togglePlayPause(); break;
            case 5: usb.stop(); break;
            case 6: UsbRecorderPage::pressPlay (ctx); break;
            case 7: UsbRecorderPage::pressRec (ctx); break;
            default: break;
        }
    }
    else if (lowerLayer == MultiTrack && ctx.recording != nullptr)
    {
        auto& rec = *ctx.recording;
        switch (key)
        {
            case 0: rec.jumpToMarker (false); break;
            case 1: rec.jumpToMarker (true); break;
            case 2: rec.addMarkerAtPosition(); break;
            case 3: rec.setLoopEnabled (! rec.isLoopEnabled()); break;
            case 4: rec.locate (0); break;
            case 5: rec.stop(); break;
            case 6: if (! rec.isRolling()) rec.play(); break;
            case 7:
                if (rec.isRecording())
                    rec.stop();
                else if (auto r = rec.record(); r.failed())
                    ctx.log.warning ("Record: " + r.getErrorMessage());
                break;
            default: break;
        }
    }
}

bool CustomControlSection::useCursorWheel() const
{
    return ctx.settings.getBoolValue (uiprefs::useCursorWheel, false);
}

int CustomControlSection::currentLayer() const
{
    return ctx.customControls != nullptr ? ctx.customControls->currentLayer : 0;
}

void CustomControlSection::stepLayer (int delta)
{
    if (upperIsDaw())
    {
        midiui::stepUpperBlock (ctx, delta);
        repaint (upperLcd);
        return;
    }
    if (ctx.customControls == nullptr)
        return;
    ctx.customControls->currentLayer = (currentLayer() + delta + cc::kNumLayers) % cc::kNumLayers;
    if (ctx.customControlsChanged)
        ctx.customControlsChanged();
    else
        bindCustomControls();
}

cc::Function CustomControlSection::ccFunction (int layer, int key) const
{
    if (ctx.customControls == nullptr)
        return {};
    const auto& c = ctx.customControls->get (layer, key % cc::kNumControls);
    return key < cc::kNumControls ? c.upper : c.lower;
}

void CustomControlSection::bindCustomControls()
{
    // The upper part follows the layer, the assignments and the selection
    // (SEL BUS SEND works on the selected strip).
    shownCcRevision = ctx.customControls != nullptr ? ctx.customControls->getRevision() : 0;
    shownCcLayer = currentLayer();
    shownSelection = ctx.getSelectedStrip ? ctx.getSelectedStrip() : nullptr;
    for (int i = 0; i < cc::kNumControls; ++i)
    {
        const auto control = ctx.customControls != nullptr ? ctx.customControls->get (shownCcLayer, i) : cc::Control {};
        const bool daw = upperIsDaw();
        encoders[i].setBinding (daw ? midiui::bindUpperVpot (ctx, i) : cc::bindEncoder (ctx, control.encoder));
        for (int row = 0; row < 2; ++row)
        {
            auto* k = upperKeys[row * cc::kNumControls + i];
            k->setEnabled (daw || (row == 0 ? control.upper : control.lower).isAssigned());
            k->ledColour = daw ? (row == 0 ? Colours::silk : Colours::mute) : control.colour > 0 ? wingColour (control.colour) : Colours::silk;
        }
    }
    applyDawMode();
    if (lowerCcLayer() >= 0)
        setLowerLayer (lowerLayer);
    shownState = {};
    repaint();
}

int CustomControlSection::lowerCcLayer() const
{
    if (lowerLayer == User)
        return cc::kUserLayer;
    if (midiui::dawModeOn (ctx) && lowerLayer >= TwoTrack && lowerLayer <= ShowControl)
        return cc::kFirstDawLayer + lowerLayer - TwoTrack;
    return -1;
}

bool CustomControlSection::upperIsDaw() const
{
    return midiui::upperIsDaw (ctx, lowerCcLayer() >= cc::kFirstDawLayer);
}

void CustomControlSection::applyDawMode()
{
    const bool daw = midiui::dawModeOn (ctx);
    shownDaw = daw;
    if (daw && layerLabels.isEmpty())
    {
        // DAW 1-4 on the layer keys, all of them usable.
        for (int i = 0; i < kNumLowerLayers; ++i)
        {
            layerLabels.add (layerKeys[i]->getLabel());
            layerKeysEnabled[(size_t) i] = layerKeys[i]->isEnabled();
        }
        for (int i = TwoTrack; i <= ShowControl; ++i)
        {
            layerKeys[i]->setLabel ("DAW " + juce::String (i - TwoTrack + 1));
            layerKeys[i]->setEnabled (true);
        }
    }
    else if (! daw && ! layerLabels.isEmpty())
    {
        for (int i = 0; i < kNumLowerLayers; ++i)
        {
            layerKeys[i]->setLabel (layerLabels[i]);
            layerKeys[i]->setEnabled (layerKeysEnabled[(size_t) i]);
        }
        layerLabels.clear();
        setLowerLayer (lowerLayer);
    }
}

int CustomControlSection::nextScene() const
{
    return sceneCursor >= 0 ? sceneCursor : ctx.show.getCurrentSceneIndex() + 1;
}

void CustomControlSection::turnWheel (int steps)
{
    if (midiui::dawModeOn (ctx))
        midiui::turnWheel (ctx, steps); // the jog wheel
    else if (useCursorWheel() && ctx.turnScreenFocus)
        ctx.turnScreenFocus (steps);
    else if (lowerLayer == ShowControl && ctx.show.getNumScenes() > 0)
    {
        sceneCursor = juce::jlimit (0, ctx.show.getNumScenes() - 1, nextScene() + steps);
        shownState = {};
        repaint (lowerLcd);
    }
    else if (lowerLayer == MultiTrack && ctx.recording != nullptr && ! ctx.recording->isRecording())
    {
        // A quarter second per step.
        auto& rec = *ctx.recording;
        const auto step = (juce::int64) (rec.getProject().getSampleRate() / 4.0);
        rec.locate (juce::jmax ((juce::int64) 0, rec.getPosition() + steps * step));
    }
}

bool CustomControlSection::lowerKeyLit (int key) const
{
    if (const int l = lowerCcLayer(); l >= 0)
        return cc::isLit (ctx, ccFunction (l, key));
    if (lowerLayer == MuteGroups)
        return (ctx.engine.muteGroups.load() >> key) & 1u;
    if (lowerLayer == TwoTrack)
    {
        auto& usb = ctx.engine.getUsbRecorder();
        switch (key)
        {
            case 3: return usb.getRepeat();
            case 4: return usb.getPlayState() == UsbRecorder::PlayState::Paused;
            case 5: return usb.getPlayState() == UsbRecorder::PlayState::Stopped;
            case 6: return usb.getPlayState() == UsbRecorder::PlayState::Playing;
            case 7: return usb.getRecState() == UsbRecorder::RecState::Recording || usb.getRecState() == UsbRecorder::RecState::Paused;
            default: return false;
        }
    }
    if (lowerLayer == MultiTrack && ctx.recording != nullptr)
    {
        const auto state = ctx.recording->getState();
        switch (key)
        {
            case 3: return ctx.recording->isLoopEnabled();
            case 5: return state == Recorder::State::Stopped;
            case 6: return state == Recorder::State::Playing || state == Recorder::State::Recording;
            case 7: return state == Recorder::State::Recording;
            default: return false;
        }
    }
    return false;
}

void CustomControlSection::refresh()
{
    bindCustomControls();
    repaint (lowerLcd);
}

void CustomControlSection::setCompact (bool c)
{
    if (compact == c)
        return;
    compact = c;
    juce::Array<juce::Component*> upper { &upperView, &upKey, &downKey, &wheel, &navUp, &navDown, &navLeft, &navRight };
    for (auto& e : encoders)
        upper.add (&e);
    for (auto* k : upperKeys)
        upper.add (k);
    for (auto* comp : upper)
        comp->setVisible (! compact);
    resized();
    repaint();
}

void CustomControlSection::timerCallback()
{
    // The custom controls follow the assignments, the layer and the selection.
    if (ctx.customControls != nullptr
        && (ctx.customControls->getRevision() != shownCcRevision || currentLayer() != shownCcLayer
            || (ctx.getSelectedStrip ? ctx.getSelectedStrip() : nullptr) != shownSelection))
        bindCustomControls();

    // The wheel turns endlessly: centred again between turns.
    if (! wheel.isMouseButtonDown (true))
        *wheelPos = 0.5f;
    const bool arrows = (useCursorWheel() && ctx.moveScreenFocus != nullptr) || midiui::dawModeOn (ctx);
    // DAW mode: the keys follow it, the upper LCD the DAW.
    if (midiui::dawModeOn (ctx) != shownDaw)
        bindCustomControls();
    if (auto* d = midiui::daw (ctx); d != nullptr && d->isOn() && d->getRevision() != shownDawRevision)
    {
        shownDawRevision = d->getRevision();
        if (upperIsDaw())
            repaint (upperLcd);
        repaint (layerDisplay);
    }
    for (auto* k : { &navUp, &navDown, &navLeft, &navRight })
        if (k->isEnabled() != arrows)
            k->setEnabled (arrows);

    // The lower LCD follows the mute group names and the scenes.
    juce::String state (lowerLayer);
    if (lowerLayer == MuteGroups)
        for (int i = 0; i < kNumMuteGroups; ++i)
            state << ctx.engine.getMuteGroupName (i) << '|';
    else if (lowerLayer == ShowControl)
        state << ctx.show.getCurrentSceneIndex() << ctx.show.getNumScenes() << '|' << sceneCursor;
    else if (lowerLayer == TwoTrack)
    {
        auto& usb = ctx.engine.getUsbRecorder();
        state << (int) usb.getPlayState() << '|' << (int) (usb.getPosition() / 48000) << '|' << (int) usb.getRecState() << '|'
              << (int) (usb.getRecTime() / 48000) << '|' << usb.getCurrentFile().getFileName();
    }
    else if (lowerLayer == MultiTrack && ctx.recording != nullptr)
        state << (int) ctx.recording->getState() << '|' << (int) (ctx.recording->getPosition() / 4800) << '|' << ctx.recording->getNumArmed() << '|'
              << ctx.recording->getProject().getName();
    if (state != shownState)
    {
        shownState = state;
        repaint (lowerLcd);
    }

    // Names starting with '*' show their value on the upper LCD.
    juce::String values;
    for (auto& e : encoders)
        if (const auto& b = e.getBinding(); b.isValid() && b.text)
            values << b.text() << '|';
    if (values != shownValues)
    {
        shownValues = values;
        repaint (upperLcd);
    }
}

void CustomControlSection::paintLcd (juce::Graphics& g, juce::Rectangle<int> area, bool colourBars, bool upper)
{
    g.setColour (juce::Colours::black);
    g.fillRoundedRectangle (area.toFloat(), 4.0f);
    auto r = area.reduced (3, 4);
    const int w = r.getWidth() / 4;

    auto scene = [this] (int index) -> juce::String
    {
        if (const auto s = ctx.show.getScene (index))
            return juce::String (index + 1).paddedLeft ('0', 2) + " " + s->name;
        return "-";
    };

    for (int i = 0; i < 4; ++i)
    {
        auto cell = juce::Rectangle<int> (r.getX() + i * w, r.getY(), w, r.getHeight()).reduced (2, 0);
        const int ccLayer = upper ? currentLayer() : juce::jmax (0, lowerCcLayer());
        const auto control = ctx.customControls != nullptr ? ctx.customControls->get (ccLayer, i) : cc::Control {};
        if (colourBars)
        {
            // The control's colour (CC EDIT > COLOR).
            drawLightBar (g, cell.removeFromTop (juce::jmax (4, cell.getHeight() / 9)).toFloat(),
                          wingColour (juce::jmax (1, control.colour)).withMultipliedBrightness (uiprefs::brightness (ctx.settings, "colorLed", 25)),
                          control.colour > 0);
            cell.removeFromTop (juce::jmax (3, cell.getHeight() / 8));
        }
        ScribbleInfo info;
        info.backlight = uiprefs::brightness (ctx.settings, "scribble", 60);
        info.contrast = uiprefs::brightness (ctx.settings, "scrCont", 50);
        if (upper || lowerCcLayer() >= 0)
        {
            // The control's name (a name starting with '*' shows the encoder's
            // value, "|word" is shown inverted), then its two keys.
            auto name = control.name.isNotEmpty() ? control.name : cc::describe (ctx, cc::Slot::Encoder, control.encoder);
            info.number = name.removeCharacters ("*|");
            if (name.startsWith ("*"))
                if (const auto b = cc::bindEncoder (ctx, control.encoder); b.isValid() && b.text)
                    info.number = b.text();
            if (name.containsChar ('|'))
                info.name = name.fromFirstOccurrenceOf ("|", false, false).upToFirstOccurrenceOf (" ", false, false);
            info.line2 = cc::describe (ctx, cc::Slot::Upper, control.upper);
            info.line3 = cc::describe (ctx, cc::Slot::Lower, control.lower);
            if (info.number.isEmpty() && info.line2.isEmpty() && info.line3.isEmpty())
                info.number = upper ? "CC" + juce::String (i + 1) : cc::layerName (ccLayer);
            if (info.name.isEmpty())
                info.name = cc::layerName (ccLayer);
        }
        else if (lowerLayer == MuteGroups)
        {
            // The names of the groups on the keys below: top row, then bottom row.
            info.number = ctx.engine.getMuteGroupName (i);
            info.name = ctx.engine.getMuteGroupName (i + 4);
        }
        else if (lowerLayer == ShowControl)
        {
            const int current = ctx.show.getCurrentSceneIndex();
            static const char* titles[] = { "PREV", "GO", "SCENES", "" };
            info.number = titles[i];
            if (i == 0)
                info.name = current > 0 ? scene (current - 1) : "-";
            else if (i == 1)
                info.name = scene (nextScene());
            else if (i == 2)
                info.name = ctx.show.getNumScenes() > 0 ? scene (current) : juce::String ("no scene");
            info.line2 = i == 1 ? "next" : (i == 2 ? "current" : "");
        }
        else if (lowerLayer == TwoTrack)
        {
            // The player (state and time, the clip) and the recorder (state and time, the format).
            auto& usb = ctx.engine.getUsbRecorder();
            const double rate = usb.getSampleRate();
            auto hms = [rate] (int64_t samples)
            {
                const auto sec = (int64_t) ((double) juce::jmax<int64_t> (0, samples) / juce::jmax (1.0, rate));
                return juce::String (sec / 3600) + ":" + juce::String ((sec / 60) % 60).paddedLeft ('0', 2) + ":" + juce::String (sec % 60).paddedLeft ('0', 2);
            };
            static const char* titles[] = { "PLAYER", "CLIP", "RECORDER", "FORMAT" };
            static const char* playStates[] = { "STOP", "PLAY", "PAUSE", "ERROR" };
            static const char* recStates[] = { "STOP", "REC", "PAUSE", "ERROR" };
            info.number = titles[i];
            switch (i)
            {
                case 0:
                    info.name = hms (usb.getPosition());
                    info.line2 = playStates[juce::jlimit (0, 3, (int) usb.getPlayState())];
                    break;
                case 1: info.name = usb.getCurrentFile().getFileNameWithoutExtension(); break;
                case 2:
                    info.name = hms (usb.getRecTime());
                    info.line2 = recStates[juce::jlimit (0, 3, (int) usb.getRecState())];
                    break;
                default: info.name = juce::String (usb.getRecChannels()) + "-CH " + juce::String (usb.getRecBits()) + " bit"; break;
            }
        }
        else if (lowerLayer == MultiTrack && ctx.recording != nullptr)
        {
            // POSITION, the transport state, the session and the armed tracks.
            auto& rec = *ctx.recording;
            const daw::TimeGrid grid { rec.getProject().getSampleRate() };
            static const char* titles[] = { "POSITION", "STATE", "SESSION", "ARMED" };
            info.number = titles[i];
            switch (i)
            {
                case 0: info.name = grid.formatTime (rec.getPosition()).upToLastOccurrenceOf (".", false, false); break;
                case 1:
                    info.name = rec.isRecording() ? "REC" : rec.isRolling() ? "PLAY" : "STOP";
                    info.line2 = rec.isLoopEnabled() ? "loop" : "";
                    break;
                case 2: info.name = rec.getProject().getName(); break;
                default: info.name = juce::String (rec.getNumArmed()) + " TR"; break;
            }
        }
        else
        {
            info.number = layerKeys[lowerLayer]->getLabel().replace ("\n", " ");
            info.name = "N/A";
        }
        drawScribble (g, cell.toFloat(), info);
    }
}

void CustomControlSection::paint (juce::Graphics& g)
{
    g.fillAll (Colours::panel);
    g.setColour (Colours::outline);
    g.fillRect (getWidth() - 1, 0, 1, getHeight());

    paintLcd (g, lowerLcd, false, false);
    drawSilk (g, dawKey.getBounds().translated (0, -24).withHeight (22).expanded (10, 0).toFloat(), "REMOTE\nCONTROL", 9.0f);
    if (compact)
    {
        drawSilk (g, heading.toFloat(), "CUSTOM CONTROLS", 11.0f);
        return;
    }
    if (upperIsDaw())
        midiui::paintUpperLcd (g, upperLcd, ctx);
    else
        paintLcd (g, upperLcd, true, true);

    // The two-digit layer display (in DAW mode the DAW's assignment display).
    g.setColour (juce::Colours::black);
    g.fillRoundedRectangle (layerDisplay.toFloat(), 2.0f);
    g.setColour (Colours::mute.brighter (0.2f));
    g.setFont (juce::FontOptions ((float) layerDisplay.getHeight() * 0.85f, juce::Font::bold));
    const auto assignment = midiui::assignmentDisplay (ctx);
    g.drawText (assignment.isNotEmpty() ? assignment : juce::String (currentLayer() + 1), layerDisplay, juce::Justification::centred);

    drawSilk (g, heading.toFloat(), "CUSTOM CONTROLS", 11.0f);
    g.setColour (Colours::silk.withAlpha (0.6f));
    g.fillRect (divider);
}

void CustomControlSection::resized()
{
    auto b = getLocalBounds().reduced (6, 6);
    const int keyColumn = juce::jlimit (40, 56, b.getWidth() / 5);
    const int h = b.getHeight();
    const int lcdH = juce::jlimit (40, 76, h / 8);
    const int keyH = juce::jlimit (16, 30, h / 18);
    const int knob = juce::jlimit (22, 46, h / 12);
    const int viewSize = juce::jmin (keyColumn - 6, 42);

    auto left = [&] (juce::Rectangle<int> row) { return row.withWidth (keyColumn); };
    auto right = [&] (juce::Rectangle<int> row) { return row.withTrimmedLeft (keyColumn); };
    auto cells = [&] (juce::Rectangle<int> row, int i, int w, int hh)
    {
        const int cw = row.getWidth() / 4;
        return juce::Rectangle<int> (w, hh).withCentre ({ row.getX() + cw * i + cw / 2, row.getCentreY() });
    };

    if (compact)
    {
        // The lower part only: heading, layer keys, VIEW + LCD, DAW + eight keys.
        const int k = juce::jlimit (16, 30, (h - 16) / 7);
        heading = b.removeFromTop (16);
        auto row = b.removeFromTop (k + 6);
        layerKeys[User]->setBounds (left (row).withSizeKeepingCentre (keyColumn - 8, k + 4));
        for (int i = 1; i < kNumLowerLayers; ++i)
            layerKeys[i]->setBounds (cells (right (row), i - 1, juce::jmin (right (row).getWidth() / 4 - 4, 52), k + 4));
        b.removeFromTop (4);
        row = b.removeFromTop (juce::jmax (lcdH * 3 / 4, k * 2));
        lowerView.setBounds (juce::Rectangle<int> (viewSize, viewSize).withCentre (left (row).getCentre()));
        lowerLcd = right (row);
        b.removeFromTop (juce::jmax (6, b.getHeight() - 2 * (k + 6)));
        for (int r = 0; r < 2; ++r)
        {
            row = b.removeFromTop (k + 6);
            if (r == 1)
                dawKey.setBounds (left (row).withSizeKeepingCentre (keyColumn - 8, k));
            for (int i = 0; i < 4; ++i)
                lowerKeys[r * 4 + i]->setBounds (cells (right (row), i, k + 4, k));
        }
        return;
    }

    // Upper part.
    auto row = b.removeFromTop (lcdH);
    upperView.setBounds (juce::Rectangle<int> (viewSize, viewSize).withCentre (left (row).getCentre()));
    upperLcd = right (row);
    b.removeFromTop (4);
    row = b.removeFromTop (16);
    layerDisplay = left (row).withSizeKeepingCentre (juce::jmin (keyColumn - 8, 30), 18);
    heading = right (row);
    row = b.removeFromTop (knob + 4);
    for (int i = 0; i < 4; ++i)
        encoders[i].setBounds (cells (right (row), i, knob, knob));
    b.removeFromTop (2);
    for (int r = 0; r < 2; ++r)
    {
        row = b.removeFromTop (keyH + 4);
        (r == 0 ? upKey : downKey).setBounds (left (row).withSizeKeepingCentre (keyColumn - 8, keyH));
        for (int i = 0; i < 4; ++i)
            upperKeys[r * 4 + i]->setBounds (cells (right (row), i, keyH + 2, keyH));
    }

    // Divider, then the lower part.
    b.removeFromTop (4);
    divider = right (b.removeFromTop (1));
    b.removeFromTop (5);
    row = b.removeFromTop (keyH + 6);
    layerKeys[User]->setBounds (left (row).withSizeKeepingCentre (keyColumn - 8, keyH + 4));
    for (int i = 1; i < kNumLowerLayers; ++i)
        layerKeys[i]->setBounds (cells (right (row), i - 1, juce::jmin (right (row).getWidth() / 4 - 4, 52), keyH + 4));
    b.removeFromTop (4);
    row = b.removeFromTop (lcdH * 3 / 4);
    lowerView.setBounds (juce::Rectangle<int> (viewSize, viewSize).withCentre (left (row).getCentre()));
    lowerLcd = right (row);
    b.removeFromTop (26);
    for (int r = 0; r < 2; ++r)
    {
        row = b.removeFromTop (keyH + 4);
        if (r == 0)
            dawKey.setBounds (left (row).withSizeKeepingCentre (keyColumn - 8, keyH).translated (0, (keyH + 4) / 2));
        for (int i = 0; i < 4; ++i)
            lowerKeys[r * 4 + i]->setBounds (cells (right (row), i, keyH + 2, keyH));
    }

    // Navigation arrows (a diamond) and the scroll wheel.
    b.removeFromTop (6);
    const int nav = juce::jlimit (14, 24, b.getHeight() / 5);
    const int wheelSize = juce::jmin (b.getHeight() - 4, b.getWidth() / 2);
    auto wheelArea = b.removeFromRight (wheelSize + 8);
    wheel.setBounds (wheelArea.withSizeKeepingCentre (wheelSize, wheelSize));
    const auto c = b.getCentre();
    navUp.setBounds (juce::Rectangle<int> (nav, nav).withCentre ({ c.x, c.y - nav }));
    navDown.setBounds (juce::Rectangle<int> (nav, nav).withCentre ({ c.x, c.y + nav }));
    navLeft.setBounds (juce::Rectangle<int> (nav, nav).withCentre ({ c.x - nav - 2, c.y }));
    navRight.setBounds (juce::Rectangle<int> (nav, nav).withCentre ({ c.x + nav + 2, c.y }));
}

} // namespace livedaw::ui
