#include "ui/StripComponent.h"
#include "dsp/DspUtils.h"

namespace livedaw::ui
{

StripComponent::StripComponent (Strip& s, MixerEngine& e)
    : strip (s),
      engine (e),
      pan (s.params.pan, -1.0, 1.0, 0.01, ""),
      mute (s.params.mute, "MUTE", Colours::mute),
      solo (s.params.solo, "SOLO", Colours::solo),
      fader (s.params.faderDb, -100.0, 10.0, 0.1, " dB", juce::Slider::LinearVertical),
      meter ([&s] (int ch) { return s.outputPeak[(size_t) ch].exchange (0.0f); },
             [&s] { return s.getKind() == StripKind::Master ? 2 : s.currentWidth.load(); })
{
    nameLabel.setText (strip.name, juce::dontSendNotification);
    nameLabel.setJustificationType (juce::Justification::centred);
    nameLabel.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    nameLabel.setEditable (false, true);
    nameLabel.onTextChange = [this] { strip.name = nameLabel.getText(); };
    nameLabel.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (nameLabel);

    routeButton.onClick = [this] { showRoutingMenu(); };
    routeButton.setVisible (strip.getKind() != StripKind::Master);
    addChildComponent (routeButton);

    pan.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    pan.setDoubleClickReturnValue (true, 0.0);
    pan.setTooltip ("Pan / balance");
    addAndMakeVisible (pan);

    addAndMakeVisible (mute);
    addChildComponent (solo);
    solo.setVisible (strip.getKind() != StripKind::Master);

    selectButton.setClickingTogglesState (false);
    selectButton.setColour (juce::TextButton::buttonOnColourId, Colours::accent);
    selectButton.onClick = [this] { if (onSelect) onSelect (strip); };
    addAndMakeVisible (selectButton);

    fader.setSkewFactorFromMidPoint (-12.0);
    fader.setDoubleClickReturnValue (true, 0.0);
    fader.setScrollWheelEnabled (true);
    addAndMakeVisible (fader);
    addAndMakeVisible (meter);

    levelLabel.setJustificationType (juce::Justification::centred);
    levelLabel.setFont (juce::FontOptions (11.0f));
    levelLabel.setColour (juce::Label::textColourId, Colours::textDim);
    addAndMakeVisible (levelLabel);

    startTimerHz (15);
    timerCallback();
}

void StripComponent::setSelected (bool shouldBeSelected)
{
    selected = shouldBeSelected;
    selectButton.setToggleState (selected, juce::dontSendNotification);
    repaint();
}

juce::String StripComponent::routingText() const
{
    const auto& p = strip.params;

    if (strip.getKind() == StripKind::Input)
    {
        const int l = p.inputLeft.load(), r = p.inputRight.load();
        if (l < 0)
            return "No input";
        return r >= 0 ? "In " + juce::String (l + 1) + "/" + juce::String (r + 1)
                      : "In " + juce::String (l + 1);
    }

    return p.stereo.load() ? "Stereo" : "Mono";
}

void StripComponent::timerCallback()
{
    routeButton.setButtonText (routingText());

    if (! nameLabel.isBeingEdited() && nameLabel.getText() != strip.name)
        nameLabel.setText (strip.name, juce::dontSendNotification);

    const float db = strip.params.faderDb.load();
    levelLabel.setText (db <= dsp::kMinusInfDb ? "-inf" : juce::String (db, 1), juce::dontSendNotification);

    shownGateGain = strip.gateGain.load();
    shownReduction = strip.compReductionDb.load();

    repaint (indicatorArea);
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

    g.setColour (strip.colour);
    g.fillRect (b.reduced (4, 0).removeFromTop (4).translated (0, 3));

    // Processing indicators: HPF, gate, EQ, comp.
    const auto& p = strip.params;
    struct Ind
    {
        const char* text;
        bool on;
        juce::Colour colour;
    };

    const bool gateClosing = p.gate.enabled.load() && shownGateGain < 0.5f;
    const bool compActive = p.comp.enabled.load() && shownReduction > 0.5f;
    const Ind inds[] = {
        { "HP", p.hpfEnabled.load(), Colours::accent },
        { "G", p.gate.enabled.load(), gateClosing ? Colours::mute : Colours::active },
        { "EQ", p.eq.enabled.load(), Colours::accent },
        { "C", p.comp.enabled.load(), compActive ? Colours::solo : Colours::active },
        { "FX", ! strip.inserts.isEmpty(), Colours::accent },
    };

    auto area = indicatorArea;
    const int w = area.getWidth() / static_cast<int> (std::size (inds));
    g.setFont (juce::FontOptions (10.0f, juce::Font::bold));

    for (const auto& ind : inds)
    {
        auto cell = area.removeFromLeft (w).reduced (1);
        g.setColour (ind.on ? ind.colour.withAlpha (0.85f) : juce::Colours::black.withAlpha (0.35f));
        g.fillRoundedRectangle (cell.toFloat(), 2.0f);
        g.setColour (ind.on ? juce::Colours::black : Colours::textDim);
        g.drawText (ind.text, cell, juce::Justification::centred);
    }
}

void StripComponent::resized()
{
    auto b = getLocalBounds().reduced (4);
    b.removeFromTop (6);
    nameLabel.setBounds (b.removeFromTop (20));
    b.removeFromTop (2);

    if (strip.getKind() != StripKind::Master)
        routeButton.setBounds (b.removeFromTop (20));
    else
        b.removeFromTop (20);

    b.removeFromTop (3);
    indicatorArea = b.removeFromTop (14);
    b.removeFromTop (3);
    pan.setBounds (b.removeFromTop (40).withSizeKeepingCentre (40, 40));
    b.removeFromTop (3);

    auto buttons = b.removeFromTop (22);
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
    selectButton.setBounds (b.removeFromTop (20));
    b.removeFromTop (4);

    levelLabel.setBounds (b.removeFromBottom (16));
    auto faderArea = b;
    meter.setBounds (faderArea.removeFromRight (16));
    fader.setBounds (faderArea);
}

void StripComponent::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
        showContextMenu();
    else if (onSelect)
        onSelect (strip);
}

void StripComponent::showRoutingMenu()
{
    juce::PopupMenu menu;
    auto& p = strip.params;

    if (strip.getKind() == StripKind::Input)
    {
        const auto& names = engine.getHardwareInputNames();
        juce::PopupMenu mono, stereo;

        for (int i = 0; i < names.size(); ++i)
        {
            mono.addItem (names[i], true, p.inputLeft.load() == i && p.inputRight.load() < 0,
                          [this, i] { strip.params.inputLeft = i; strip.params.inputRight = -1; });

            if (i + 1 < names.size())
                stereo.addItem (names[i] + " + " + names[i + 1], true,
                                p.inputLeft.load() == i && p.inputRight.load() == i + 1,
                                [this, i] { strip.params.inputLeft = i; strip.params.inputRight = i + 1; });
        }

        menu.addItem ("No input", true, p.inputLeft.load() < 0,
                      [this] { strip.params.inputLeft = -1; strip.params.inputRight = -1; });
        menu.addSubMenu ("Mono input", mono, names.size() > 0);
        menu.addSubMenu ("Stereo input pair", stereo, names.size() > 1);
    }
    else
    {
        menu.addItem ("Mono bus", true, ! p.stereo.load(), [this] { strip.params.stereo = false; });
        menu.addItem ("Stereo bus", true, p.stereo.load(), [this] { strip.params.stereo = true; });
    }

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (routeButton));
}

void StripComponent::showContextMenu()
{
    juce::PopupMenu menu;
    menu.addItem ("Rename", [this] { nameLabel.showEditor(); });

    juce::PopupMenu colours;
    const std::pair<const char*, juce::uint32> palette[] = {
        { "Blue", 0xff3d7ab8 }, { "Green", 0xff3daa6e }, { "Orange", 0xffb8863d },
        { "Red", 0xffb83d4f }, { "Purple", 0xff8a4fc4 }, { "Teal", 0xff3aa6a6 }, { "Grey", 0xff7a7f87 },
    };
    for (auto& [name, argb] : palette)
        colours.addItem (name, [this, c = juce::Colour (argb)] { strip.colour = c; repaint(); });
    menu.addSubMenu ("Colour", colours);

    if (strip.getKind() != StripKind::Master)
    {
        menu.addSeparator();
        menu.addItem ("Remove", [this] { if (onRemove) onRemove (strip); });
    }

    menu.showMenuAsync (juce::PopupMenu::Options());
}

} // namespace livedaw::ui
