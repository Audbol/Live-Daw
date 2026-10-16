#include "ui/StripComponent.h"
#include "dsp/DspUtils.h"

namespace livedaw::ui
{

StripComponent::StripComponent (Strip& s, UiContext& c)
    : strip (s),
      ctx (c),
      inserts (s, c),
      gate (s),
      eq (s, c),
      comp (s),
      pan (s.params.pan, -1.0, 1.0, 0.01, ""),
      mute (s.params.mute, "MUTE", Colours::mute),
      solo (s.params.solo, "SOLO", Colours::solo),
      fader (s.params.faderDb, -100.0, 10.0, 0.1, " dB", juce::Slider::LinearVertical),
      meter ([&s] (int ch) { return s.outputPeak[(size_t) ch].exchange (0.0f); },
             [&s] { return s.getKind() == StripKind::Master ? 2 : s.currentWidth.load(); })
{
    const bool isMaster = strip.getKind() == StripKind::Master;

    nameLabel.setText (strip.getName(), juce::dontSendNotification);
    nameLabel.setJustificationType (juce::Justification::centred);
    nameLabel.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    nameLabel.setEditable (false, true);
    nameLabel.onTextChange = [this] { strip.setName (nameLabel.getText()); };
    nameLabel.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (nameLabel);

    routeButton.onClick = [this] { showRoutingMenu(); };
    routeButton.setTooltip (strip.getKind() == StripKind::Input ? "Input patch" : "Bus width");
    widthButton.onClick = [this] { toggleWidth(); };
    widthButton.setTooltip ("Mono / stereo");
    widthButton.setColour (juce::TextButton::buttonOnColourId, Colours::accent);
    addChildComponent (routeButton);
    addChildComponent (widthButton);
    routeButton.setVisible (! isMaster);
    widthButton.setVisible (! isMaster);

    for (auto* h : std::initializer_list<juce::Component*> { &inserts, &gate, &eq, &comp })
        addAndMakeVisible (h);

    pan.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    pan.setDoubleClickReturnValue (true, 0.0);
    pan.setTooltip ("Pan / balance (double-click to centre)");
    pan.setScrollWheelEnabled (false); // the wheel scrolls the mixer; it must never move a mix
    addAndMakeVisible (pan);

    addAndMakeVisible (mute);
    addChildComponent (solo);
    solo.setVisible (! isMaster);

    selectButton.onClick = [this] { if (ctx.selectStrip) ctx.selectStrip (&strip); };
    selectButton.setColour (juce::TextButton::buttonOnColourId, Colours::accent);
    addAndMakeVisible (selectButton);

    fader.setSkewFactorFromMidPoint (-12.0);
    fader.setDoubleClickReturnValue (true, 0.0);
    fader.setScrollWheelEnabled (false);
    addAndMakeVisible (fader);
    addAndMakeVisible (meter);

    levelLabel.setJustificationType (juce::Justification::centred);
    levelLabel.setFont (juce::FontOptions (11.0f));
    levelLabel.setColour (juce::Label::textColourId, Colours::textDim);
    addAndMakeVisible (levelLabel);

    startTimerHz (10);
    timerCallback();
}

void StripComponent::setSelected (bool shouldBeSelected)
{
    selected = shouldBeSelected;
    selectButton.setToggleState (selected, juce::dontSendNotification);
    repaint();
}

void StripComponent::setNumInsertRows (int rows)
{
    if (rows != insertRows)
    {
        insertRows = rows;
        inserts.setNumRows (rows);
        resized();
    }
}

juce::String StripComponent::routingText() const
{
    const auto& p = strip.params;

    if (strip.getKind() != StripKind::Input)
        return "Bus";

    const int l = p.inputLeft.load();
    if (l < 0)
        return "No input";

    if (! p.stereo.load())
        return "In " + juce::String (l + 1);

    const int r = p.inputRight.load() >= 0 ? p.inputRight.load() : l + 1;
    return "In " + juce::String (l + 1) + "/" + juce::String (r + 1);
}

void StripComponent::timerCallback()
{
    routeButton.setButtonText (routingText());

    const bool stereo = strip.params.stereo.load();
    widthButton.setButtonText (stereo ? "ST" : "MO");
    widthButton.setToggleState (stereo, juce::dontSendNotification);

    if (! nameLabel.isBeingEdited() && nameLabel.getText() != strip.getName())
        nameLabel.setText (strip.getName(), juce::dontSendNotification);

    const float db = strip.params.faderDb.load();
    levelLabel.setText (db <= dsp::kMinusInfDb ? "-inf" : juce::String (db, 1), juce::dontSendNotification);

    const bool safe = strip.params.recallSafe.load();
    if (safe != (bool) getProperties()["safe"])
    {
        getProperties().set ("safe", safe);
        repaint();
    }
}

void StripComponent::toggleWidth()
{
    auto& p = strip.params;
    const bool toStereo = ! p.stereo.load();

    if (toStereo && strip.getKind() == StripKind::Input && p.inputRight.load() < 0 && p.inputLeft.load() >= 0)
        p.inputRight = p.inputLeft.load() + 1;

    p.stereo = toStereo;
    timerCallback();
}

void StripComponent::paint (juce::Graphics& g)
{
    auto b = getLocalBounds();
    g.setColour (selected ? Colours::panelLight : Colours::panel);
    g.fillRoundedRectangle (b.toFloat().reduced (1.0f), 4.0f);

    if (selected)
    {
        g.setColour (Colours::accent);
        g.drawRoundedRectangle (b.toFloat().reduced (1.5f), 4.0f, 1.5f);
    }

    g.setColour (strip.getColour());
    g.fillRect (b.reduced (4, 0).removeFromTop (4).translated (0, 3));

    if (strip.params.recallSafe.load())
    {
        // Recall-safe badge across the colour bar.
        auto badge = juce::Rectangle<int> (34, 11).withCentre ({ b.getCentreX(), 8 });
        g.setColour (Colours::solo);
        g.fillRoundedRectangle (badge.toFloat(), 2.0f);
        g.setColour (juce::Colours::black);
        g.setFont (juce::FontOptions (9.0f, juce::Font::bold));
        g.drawText ("SAFE", badge, juce::Justification::centred);
    }
}

void StripComponent::resized()
{
    // The fader always gets at least kMinFader px. When the strip is short, the
    // least important sections are dropped first: inserts, then the mini EQ,
    // then the gate/comp handles.
    constexpr int kMinFader = 110;
    const int eqHeight = juce::jlimit (40, 110, getHeight() / 12);
    const int panSize = juce::jlimit (32, 52, getWidth() / 3);
    const int insertsHeight = insertRows * InsertSlotsHandle::kRowHeight + 4;
    const int dynamicsHeight = 16 + 3 + 16 + 3;

    // Name, routing, pan, mute/solo, select, level label and padding.
    const int essentials = 8 + 10 + 20 + 22 + panSize + 3 + 23 + 22 + 14;
    int spare = getHeight() - essentials - kMinFader;

    const bool showInserts = (spare -= insertsHeight) >= 0;
    const bool showEq = showInserts ? (spare -= eqHeight + 3) >= 0 : false;
    const bool showDynamics = showEq ? (spare -= dynamicsHeight) >= 0 : false;

    inserts.setVisible (showInserts);
    eq.setVisible (showEq);
    gate.setVisible (showDynamics);
    comp.setVisible (showDynamics);

    auto b = getLocalBounds().reduced (4);
    b.removeFromTop (10);
    nameLabel.setBounds (b.removeFromTop (18));
    b.removeFromTop (2);

    auto route = b.removeFromTop (18);
    if (strip.getKind() != StripKind::Master)
    {
        widthButton.setBounds (route.removeFromRight (26));
        route.removeFromRight (2);
        routeButton.setBounds (route);
    }
    b.removeFromTop (4);

    if (showInserts)
    {
        inserts.setBounds (b.removeFromTop (insertRows * InsertSlotsHandle::kRowHeight));
        b.removeFromTop (4);
    }

    if (showDynamics)
    {
        gate.setBounds (b.removeFromTop (16));
        b.removeFromTop (3);
    }

    if (showEq)
    {
        // The mini EQ and pan knob grow with the strip (tall windows, wide strips).
        eq.setBounds (b.removeFromTop (eqHeight));
        b.removeFromTop (3);
    }

    if (showDynamics)
    {
        comp.setBounds (b.removeFromTop (16));
        b.removeFromTop (3);
    }

    pan.setBounds (b.removeFromTop (panSize).withSizeKeepingCentre (panSize, panSize));
    b.removeFromTop (3);

    auto buttons = b.removeFromTop (20);
    if (strip.getKind() == StripKind::Master)
    {
        mute.setBounds (buttons);
    }
    else
    {
        mute.setBounds (buttons.removeFromLeft (buttons.getWidth() / 2).reduced (1, 0));
        solo.setBounds (buttons.reduced (1, 0));
    }
    b.removeFromTop (3);
    selectButton.setBounds (b.removeFromTop (18));
    b.removeFromTop (4);

    levelLabel.setBounds (b.removeFromBottom (14));
    auto faderArea = b;
    meter.setBounds (faderArea.removeFromRight (juce::jlimit (16, 24, getWidth() / 6)));
    fader.setBounds (faderArea);
}

void StripComponent::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
        showContextMenu();
    else if (ctx.selectStrip)
        ctx.selectStrip (&strip);
}

void StripComponent::showRoutingMenu()
{
    if (strip.getKind() != StripKind::Input)
    {
        toggleWidth();
        return;
    }

    juce::PopupMenu menu;
    auto& p = strip.params;
    const auto& names = ctx.engine.getHardwareInputNames();
    const bool stereo = p.stereo.load();

    auto inputMenu = [&] (std::atomic<int>& target)
    {
        juce::PopupMenu m;
        m.addItem ("None", true, target.load() < 0, [&target] { target = -1; });
        for (int i = 0; i < names.size(); ++i)
            m.addItem (juce::String (i + 1) + ": " + names[i], true, target.load() == i, [&target, i] { target = i; });
        return m;
    };

    menu.addItem ("Mono", true, ! stereo, [this] { if (strip.params.stereo.load()) toggleWidth(); });
    menu.addItem ("Stereo", true, stereo, [this] { if (! strip.params.stereo.load()) toggleWidth(); });
    menu.addSeparator();
    menu.addSubMenu (stereo ? "Left input" : "Input", inputMenu (p.inputLeft));
    if (stereo)
        menu.addSubMenu ("Right input", inputMenu (p.inputRight));

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (routeButton));
}

void StripComponent::showContextMenu()
{
    juce::PopupMenu menu;
    menu.addItem ("Rename", [this] { nameLabel.showEditor(); });
    menu.addItem ("Recall safe", true, strip.params.recallSafe.load(),
                  [this] { strip.params.recallSafe = ! strip.params.recallSafe.load(); repaint(); });

    // Layers: put this strip on any user layer.
    juce::PopupMenu layersMenu;
    const auto ref = state::StripRef::of (strip);
    for (auto& layer : ctx.show.layers.getUserLayers())
    {
        const bool onLayer = std::find (layer.strips.begin(), layer.strips.end(), ref) != layer.strips.end();
        layersMenu.addItem (layer.name, true, onLayer, [this, ref, id = layer.id, onLayer]
        {
            if (onLayer)
                ctx.show.layers.removeFromUserLayer (id, ref);
            else
                ctx.show.layers.addToUserLayer (id, ref);
            notifyLayersChangedAsync (ctx);
        });
    }
    if (! ctx.show.layers.getUserLayers().empty())
        layersMenu.addSeparator();
    layersMenu.addItem ("New user layer with this strip", [this, ref]
    {
        ctx.show.layers.addUserLayer ("User " + juce::String (ctx.show.layers.getUserLayers().size() + 1), { ref });
        notifyLayersChangedAsync (ctx);
    });
    menu.addSubMenu ("User layers", layersMenu);

    juce::PopupMenu colours;
    const std::pair<const char*, juce::uint32> palette[] = {
        { "Blue", 0xff3d7ab8 }, { "Green", 0xff3daa6e }, { "Orange", 0xffb8863d },
        { "Red", 0xffb83d4f }, { "Purple", 0xff8a4fc4 }, { "Teal", 0xff3aa6a6 }, { "Grey", 0xff7a7f87 },
    };
    for (auto& [name, argb] : palette)
        colours.addItem (name, [this, c = juce::Colour (argb)] { strip.setColour (c); repaint(); });
    menu.addSubMenu ("Colour", colours);

    if (strip.getKind() != StripKind::Master)
    {
        menu.addSeparator();
        menu.addItem ("Remove", [this] { if (onRemove) onRemove (strip); });
    }

    menu.showMenuAsync (juce::PopupMenu::Options());
}

} // namespace livedaw::ui
