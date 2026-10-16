#include "ui/SurfaceSections.h"
#include "ui/ResponseGraph.h"

namespace livedaw::ui
{

BlockKey::BlockKey (const juce::String& keyText) : juce::TextButton (keyText)
{
    setColour (buttonOnColourId, Colours::block);
    setColour (buttonColourId, Colours::panelLight.darker (0.2f));
    setTooltip (keyText + ": click to edit, right-click to switch on/off");
    startTimerHz (8);
}

void BlockKey::mouseUp (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu() || e.mods.isCommandDown())
    {
        if (onToggle && getLocalBounds().contains (e.getPosition()))
            onToggle();
        juce::TextButton::mouseUp (e.withNewPosition (juce::Point<float> (-1.0f, -1.0f))); // release without clicking
        return;
    }
    juce::TextButton::mouseUp (e);
}

void BlockKey::timerCallback()
{
    const bool on = isOn && isOn();
    if (getToggleState() != on)
        setToggleState (on, juce::dontSendNotification);
}

//==============================================================================
ChannelStripSection::ChannelStripSection (UiContext& c)
    : ctx (c),
      inMeter ([this] (int ch) { return ctx.meters.get (strip, true, ch); }, [this] { return strip != nullptr ? strip->getNumChannels() : 1; }),
      outMeter ([this] (int ch) { return ctx.meters.get (strip, false, ch); }, [this] { return strip != nullptr ? strip->getNumChannels() : 1; })
{
    addAndMakeVisible (inMeter);
    addAndMakeVisible (outMeter);

    const char* names[kNumBlocks] = { "INPUT", "FILTER", "GATE", "DYN", "INSERT", "MAIN" };
    const HomeTab tabs[kNumBlocks] = { HomeTab::Input, HomeTab::Filter, HomeTab::Gate, HomeTab::Dynamics, HomeTab::PreInsert, HomeTab::Main };
    for (int i = 0; i < kNumBlocks; ++i)
    {
        blockKnobs[i].setCompact (true);
        addAndMakeVisible (blockKnobs[i]);
        blockKeys[i] = std::make_unique<BlockKey> (names[i]);
        const auto tab = tabs[i];
        blockKeys[i]->onOpen = [this, tab] { if (ctx.showHomeTab) ctx.showHomeTab (tab); };
        blockKeys[i]->onClick = [this, tab] { if (ctx.showHomeTab) ctx.showHomeTab (tab); };
        addAndMakeVisible (*blockKeys[i]);
    }

    for (auto& k : eqKnobs)
    {
        k.setCompact (true);
        addAndMakeVisible (k);
    }
    eqMix.setCompact (true);
    addAndMakeVisible (eqMix);

    eqKey = std::make_unique<BlockKey> ("EQ");
    eqKey->onClick = [this] { if (ctx.showHomeTab) ctx.showHomeTab (HomeTab::Eq); };
    eqKey->isOn = [this] { return strip != nullptr && strip->params.eq.on.load(); };
    eqKey->onToggle = [this] { if (strip != nullptr) strip->params.eq.on = ! strip->params.eq.on.load(); };
    addAndMakeVisible (*eqKey);

    for (auto* b : { &gainButton, &widthButton, &freqButton, &shiftButton })
    {
        b->setColour (juce::TextButton::buttonOnColourId, Colours::accent);
        addAndMakeVisible (b);
    }
    gainButton.onClick = [this] { eqElement = 0; bindEq(); };
    widthButton.onClick = [this] { eqElement = 1; bindEq(); };
    freqButton.onClick = [this] { eqElement = 2; bindEq(); };
    shiftButton.onClick = [this] { eqShift = ! eqShift; bindEq(); };

    startTimerHz (5);
    refresh();
}

void ChannelStripSection::refresh()
{
    strip = ctx.getSelectedStrip ? ctx.getSelectedStrip() : nullptr;
    shownName = {};

    for (int i = 0; i < kNumBlocks; ++i)
    {
        blockKnobs[i].setBinding ({});
        blockKeys[i]->isOn = nullptr;
        blockKeys[i]->onToggle = nullptr;
        blockKeys[i]->setEnabled (strip != nullptr);
    }

    if (strip != nullptr)
    {
        auto& p = strip->params;
        auto threshold = [] (DynSlotParams& slot, const juce::String& label) -> KnobBinding
        {
            const auto& spec = models::model (models::Dynamics, slot.model.load());
            if (spec.params.empty())
                return {};
            const int i = juce::jmax (0, spec.find ("thr"));
            auto b = bindModelParam (slot.values[i], spec.params[(size_t) i]);
            b.label = label;
            return b;
        };

        blockKnobs[0].setBinding (bindLinear ("GAIN", p.input.trimDb, -18.0f, 18.0f, " dB", 1, 0.0f));
        blockKeys[0]->isOn = [&p] { return p.input.invert.load(); };
        blockKeys[0]->onToggle = [&p] { p.input.invert = ! p.input.invert.load(); };

        const bool channel = strip->getKind() == StripKind::Channel;
        blockKeys[1]->setEnabled (channel);
        blockKeys[2]->setEnabled (channel);
        if (channel)
        {
            blockKnobs[1].setBinding (bindFreq ("LO CUT", p.filter.lowCutFreq, 20.0f, 2000.0f, 80.0f));
            blockKeys[1]->isOn = [&p] { return p.filter.lowCut.load(); };
            blockKeys[1]->onToggle = [&p] { p.filter.lowCut = ! p.filter.lowCut.load(); };
            blockKnobs[2].setBinding (threshold (p.gate, "GATE"));
            blockKeys[2]->isOn = [&p] { return p.gate.on.load(); };
            blockKeys[2]->onToggle = [&p] { p.gate.on = ! p.gate.on.load(); };
        }

        blockKnobs[3].setBinding (threshold (p.dynamics, "DYN"));
        blockKeys[3]->isOn = [&p] { return p.dynamics.on.load(); };
        blockKeys[3]->onToggle = [&p] { p.dynamics.on = ! p.dynamics.on.load(); };

        if (const int slot = p.preInsert.fx.load(); slot >= 0)
            blockKnobs[4].setBinding (bindLinear ("FX MIX", ctx.engine.getFx().params (slot).mix, 0.0f, 100.0f, "%", 0, 100.0f));
        blockKeys[4]->isOn = [&p] { return p.preInsert.on.load() && p.preInsert.fx.load() >= 0; };
        blockKeys[4]->onToggle = [&p] { p.preInsert.on = ! p.preInsert.on.load(); };

        blockKnobs[5].setBinding (bindPan ("PAN", p.pan));
        blockKeys[5]->isOn = [&p] { return p.mains[0].on.load(); };
        blockKeys[5]->onToggle = [&p] { p.mains[0].on = ! p.mains[0].on.load(); };
    }

    eqModel = -1;
    bindEq();
    repaint();
}

void ChannelStripSection::bindEq()
{
    gainButton.setToggleState (eqElement == 0, juce::dontSendNotification);
    widthButton.setToggleState (eqElement == 1, juce::dontSendNotification);
    freqButton.setToggleState (eqElement == 2, juce::dontSendNotification);
    shiftButton.setToggleState (eqShift, juce::dontSendNotification);

    for (auto& k : eqKnobs)
        k.setBinding ({});
    eqMix.setBinding ({});
    eqKey->setEnabled (strip != nullptr);
    if (strip == nullptr)
        return;

    auto& eq = strip->params.eq;
    eqModel = eq.model.load();
    eqMix.setBinding (bindLinear ("MIX", eq.mix, 0.0f, 125.0f, "%", 0, 100.0f));
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

    // WING EQ: bands 1-4, SHIFT for the shelves (and bus bands 5-6).
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
            b.label = shown[i].toUpperCase() + " " + spec.params[(size_t) index].label.upToFirstOccurrenceOf (" ", false, false);
            eqKnobs[i].setBinding (b);
        }
    }
}

void ChannelStripSection::timerCallback()
{
    if (strip != nullptr && strip->params.eq.model.load() != eqModel)
        bindEq();

    const auto name = strip != nullptr ? describeStrip (*strip) : juce::String();
    if (name != shownName)
    {
        shownName = name;
        repaint (lcdArea);
    }
}

void ChannelStripSection::paint (juce::Graphics& g)
{
    g.fillAll (Colours::panel);
    g.setColour (Colours::outline);
    g.drawRect (getLocalBounds().removeFromRight (1));

    // The section's display, styled like a scribble strip: label, icon and the
    // name on a bar in the strip's colour.
    auto lcd = lcdArea.toFloat();
    g.setColour (juce::Colours::black);
    g.fillRoundedRectangle (lcd, 3.0f);
    auto t = lcdArea.reduced (8, 5).withTrimmedRight (48).toFloat();
    if (strip != nullptr)
    {
        auto icon = t.removeFromLeft (t.getHeight());
        drawIcon (g, stripIcon (strip->getKind()), icon.reduced (4.0f), juce::Colours::white);
        t.removeFromLeft (6.0f);
        auto top = t.removeFromTop (t.getHeight() * 0.4f);
        g.setColour (Colours::text);
        g.setFont (juce::FontOptions (juce::jmin (15.0f, top.getHeight()), juce::Font::bold));
        g.drawText (strip->getLabel() + "   " + (strip->isOutput() || strip->params.stereo.load() ? "ST" : "MONO"), top.toNearestInt(),
                    juce::Justification::centredLeft);
        t.removeFromTop (2.0f);
        g.setColour (strip->getColour());
        g.fillRect (t);
        g.setColour (strip->getColour().getPerceivedBrightness() > 0.45f ? juce::Colours::black : juce::Colours::white);
        g.setFont (juce::FontOptions (juce::jlimit (13.0f, 22.0f, t.getHeight() * 0.8f), juce::Font::bold));
        g.drawFittedText (shownName, t.toNearestInt().reduced (6, 0), juce::Justification::centredLeft, 1);
    }
    else
    {
        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
        g.drawFittedText ("PRESS SEL ON A STRIP", t.toNearestInt(), juce::Justification::centredLeft, 1);
    }

    // Silk-screen outlines around the knob groups.
    g.setColour (Colours::outline);
    g.drawRoundedRectangle (blockArea.toFloat().expanded (4.0f, 2.0f), 4.0f, 1.0f);
    g.drawRoundedRectangle (eqArea.toFloat().expanded (4.0f, 0.0f), 4.0f, 1.0f);
    g.setColour (Colours::textDim);
    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    g.drawText ("EQ", eqArea.removeFromTop (14).reduced (6, 0), juce::Justification::centredLeft);
}

void ChannelStripSection::resized()
{
    auto b = getLocalBounds().reduced (8, 6);
    lcdArea = b.removeFromTop (juce::jlimit (44, 64, b.getHeight() / 6));
    auto meters = lcdArea.reduced (6, 6).removeFromRight (40);
    inMeter.setBounds (meters.removeFromLeft (18));
    meters.removeFromLeft (4);
    outMeter.setBounds (meters.removeFromLeft (18));
    b.removeFromTop (8);

    blockArea = b.removeFromTop (b.getHeight() / 2);
    const int w = blockArea.getWidth() / kNumBlocks;
    for (int i = 0; i < kNumBlocks; ++i)
    {
        auto cell = juce::Rectangle<int> (blockArea.getX() + i * w, blockArea.getY(), w, blockArea.getHeight()).reduced (3, 0);
        blockKeys[i]->setBounds (cell.removeFromBottom (30).reduced (0, 2));
        // Keep the knob, its caption and value together rather than spread over a tall cell.
        blockKnobs[i].setBounds (cell.removeFromBottom (juce::jmin (cell.getHeight(), cell.getWidth() * 3 / 2)));
    }

    b.removeFromTop (6);
    eqArea = b;
    auto e = b.withTrimmedTop (14);
    auto keys = e.removeFromBottom (30).reduced (0, 2);
    const int kw = keys.getWidth() / 6;
    eqKey->setBounds (keys.removeFromLeft (kw).reduced (2, 0));
    for (auto* button : { &gainButton, &widthButton, &freqButton, &shiftButton })
        button->setBounds (keys.removeFromLeft (kw).reduced (2, 0));
    const int ew = e.getWidth() / 5;
    auto fit = [] (juce::Rectangle<int> r) { return r.removeFromBottom (juce::jmin (r.getHeight(), r.getWidth() * 3 / 2)); };
    for (auto& k : eqKnobs)
        k.setBounds (fit (e.removeFromLeft (ew).reduced (3, 2)));
    eqMix.setBounds (fit (e.reduced (3, 2)));
}

//==============================================================================
MonitorSection::MonitorSection (UiContext& c)
    : ctx (c),
      meter ([this] (int ch) { return ctx.meters.getMonitor (ch); }, [] { return 2; })
{
    meter.setShowScale (true);
    addAndMakeVisible (meter);
    level.setCompact (true);
    level.setBinding (bindLinear ("MONITOR", ctx.engine.monitorLevelDb, -60.0f, 10.0f, " dB", 1, 0.0f));
    addAndMakeVisible (level);

    dimButton.setClickingTogglesState (true);
    dimButton.setColour (juce::TextButton::buttonOnColourId, Colours::solo);
    dimButton.onClick = [this]
    {
        if (dimButton.getToggleState())
        {
            dimRestoreDb = ctx.engine.monitorLevelDb.load();
            ctx.engine.monitorLevelDb = dimRestoreDb - 20.0f;
        }
        else
        {
            ctx.engine.monitorLevelDb = dimRestoreDb;
        }
        dimmed = dimButton.getToggleState();
    };
    aflButton.setClickingTogglesState (true);
    aflButton.setColour (juce::TextButton::buttonOnColourId, Colours::solo);
    aflButton.setTooltip ("Solo after fader (off: pre fader listen)");
    aflButton.onClick = [this] { ctx.engine.soloAfl = aflButton.getToggleState(); };
    clearSoloButton.setColour (juce::TextButton::buttonOnColourId, Colours::solo);
    clearSoloButton.onClick = [this]
    {
        for (int k = 0; k < kNumStripKinds; ++k)
            for (auto& s : ctx.engine.getStrips (static_cast<StripKind> (k)))
                s->params.solo = false;
        for (int i = 0; i < kNumDcas; ++i)
            ctx.engine.getDca (i).solo = false;
    };

    prevButton.onClick = [this] { logRecall (ctx, "previous scene", ctx.show.recallPrevious()); };
    goButton.setColour (juce::TextButton::buttonColourId, Colours::active.darker (0.4f));
    goButton.setTooltip ("Recall the next scene (Ctrl+G)");
    goButton.onClick = [this] { logRecall (ctx, "next scene", ctx.show.recallNext()); };
    scenesButton.onClick = [this] { if (ctx.showScreen) ctx.showScreen (Screen::Library); };

    for (int i = 0; i < kNumMuteGroups; ++i)
    {
        auto* b = muteGroupButtons.add (new juce::TextButton (juce::String (i + 1)));
        b->setColour (juce::TextButton::buttonOnColourId, Colours::mute);
        b->onClick = [this, i] { ctx.engine.muteGroups = ctx.engine.muteGroups.load() ^ (1u << i); };
        addAndMakeVisible (b);
    }

    for (auto* b : { &dimButton, &aflButton, &clearSoloButton, &prevButton, &goButton, &scenesButton })
        addAndMakeVisible (b);
    startTimerHz (8);
    refresh();
}

void MonitorSection::refresh()
{
    const int i = ctx.show.getCurrentSceneIndex();
    const auto scene = ctx.show.getScene (i);
    sceneText = scene ? juce::String (i + 1).paddedLeft ('0', 2) + "  " + scene->name : juce::String ("no scene");
    prevButton.setEnabled (ctx.show.getNumScenes() > 0);
    goButton.setEnabled (ctx.show.getNumScenes() > 0);
    repaint (sceneArea);
}

void MonitorSection::timerCallback()
{
    aflButton.setToggleState (ctx.engine.soloAfl.load(), juce::dontSendNotification);
    clearSoloButton.setToggleState (ctx.engine.anySoloActive.load(), juce::dontSendNotification);
    const auto groups = ctx.engine.muteGroups.load();
    for (int i = 0; i < muteGroupButtons.size(); ++i)
    {
        muteGroupButtons[i]->setToggleState ((groups >> i) & 1u, juce::dontSendNotification);
        muteGroupButtons[i]->setTooltip (ctx.engine.getMuteGroupName (i));
    }
    repaint (meterArea.withHeight (16));
}

void MonitorSection::paint (juce::Graphics& g)
{
    g.fillAll (Colours::panel);
    g.setColour (Colours::textDim);
    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    g.drawText (ctx.engine.anySoloActive.load() ? "SOLO" : "MAIN 1", meterArea.withHeight (16), juce::Justification::centred);

    // Silk-screen outlines with their titles.
    for (auto [area, title] : { std::pair { monitorArea, "MONITOR" }, std::pair { sceneArea, "SHOW CONTROL" }, std::pair { mgArea, "MUTE GROUPS" } })
    {
        g.setColour (Colours::outline);
        g.drawRoundedRectangle (area.toFloat().withTrimmedTop (7.0f), 4.0f, 1.0f);
        auto label = area.withHeight (14).withTrimmedLeft (10);
        const int w = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), title) + 10;
        g.setColour (Colours::panel);
        g.fillRect (label.withWidth (w));
        g.setColour (Colours::textDim);
        g.drawText (title, label.withWidth (w), juce::Justification::centred);
    }

    g.setColour (juce::Colours::black);
    g.fillRoundedRectangle (sceneDisplay.toFloat(), 3.0f);
    g.setColour (Colours::active);
    g.setFont (juce::FontOptions (juce::jmin (18.0f, sceneDisplay.getHeight() * 0.6f), juce::Font::bold));
    g.drawFittedText (sceneText, sceneDisplay.reduced (8, 0), juce::Justification::centredLeft, 1);
}

void MonitorSection::resized()
{
    auto b = getLocalBounds().reduced (8, 6);
    meterArea = b.removeFromLeft (juce::jlimit (44, 64, b.getWidth() / 5));
    meter.setBounds (meterArea.withTrimmedTop (18));
    b.removeFromLeft (8);

    const int gap = 8;
    const int sceneH = juce::jmin (120, b.getHeight() / 4);
    const int mgH = juce::jmin (130, b.getHeight() / 4);
    monitorArea = b.removeFromTop (b.getHeight() - sceneH - mgH - gap * 2);
    b.removeFromTop (gap);
    sceneArea = b.removeFromTop (sceneH);
    b.removeFromTop (gap);
    mgArea = b;

    auto m = monitorArea.withTrimmedTop (16).reduced (8, 6);
    level.setBounds (m.removeFromLeft (juce::jmin (130, m.getWidth() / 2)).withSizeKeepingCentre (juce::jmin (130, m.getWidth() / 2), juce::jmin (m.getHeight(), 150)));
    m.removeFromLeft (6);
    const int h = juce::jmin (36, m.getHeight() / 3);
    auto keys = m.withSizeKeepingCentre (m.getWidth(), h * 3);
    dimButton.setBounds (keys.removeFromTop (h).reduced (2));
    aflButton.setBounds (keys.removeFromTop (h).reduced (2));
    clearSoloButton.setBounds (keys.removeFromTop (h).reduced (2));

    auto s2 = sceneArea.withTrimmedTop (16).reduced (8, 6);
    const int rowH = juce::jmin (34, (s2.getHeight() - 6) / 2);
    sceneDisplay = s2.removeFromTop (rowH);
    s2.removeFromTop (6);
    auto row = s2.removeFromTop (rowH);
    prevButton.setBounds (row.removeFromLeft (row.getWidth() / 4).reduced (2, 0));
    scenesButton.setBounds (row.removeFromRight (row.getWidth() / 3).reduced (2, 0));
    goButton.setBounds (row.reduced (2, 0));

    auto mg = mgArea.withTrimmedTop (16).reduced (8, 6);
    const int w = mg.getWidth() / 4, mh = juce::jmin (40, mg.getHeight() / 2);
    mg = mg.withSizeKeepingCentre (mg.getWidth(), mh * 2);
    for (int i = 0; i < muteGroupButtons.size(); ++i)
        muteGroupButtons[i]->setBounds (mg.getX() + (i % 4) * w, mg.getY() + (i / 4) * mh, w - 3, mh - 3);
}

} // namespace livedaw::ui
