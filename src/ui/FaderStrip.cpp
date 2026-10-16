#include "ui/FaderStrip.h"

namespace livedaw::ui
{

FaderStrip::FaderStrip (UiContext& c)
    : ctx (c),
      meter ([this] (int ch) { return sendSource != nullptr ? 0.0f : ctx.meters.get (strip, false, ch); },
             [this] { return strip != nullptr ? strip->getNumChannels() : 1; })
{
    fader.setSliderStyle (juce::Slider::LinearVertical);
    fader.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    fader.setNormalisableRange (fader::range());
    fader.setDoubleClickReturnValue (true, 0.0);
    fader.setMouseDragSensitivity (400);
    fader.onValueChange = [this]
    {
        if (auto* p = faderParam())
            p->store ((float) fader.getValue());
    };
    addAndMakeVisible (fader);

    selButton.setColour (juce::TextButton::buttonOnColourId, Colours::select);
    soloButton.setColour (juce::TextButton::buttonOnColourId, Colours::solo);
    muteButton.setColour (juce::TextButton::buttonOnColourId, Colours::mute);
    for (auto* b : { &selButton, &soloButton, &muteButton })
    {
        b->setColour (juce::TextButton::buttonColourId, Colours::panelLight.darker (0.2f));
        addAndMakeVisible (b);
    }

    selButton.onClick = [this]
    {
        if (strip != nullptr && ctx.selectStrip)
            ctx.selectStrip (strip);
    };

    soloButton.onClick = [this]
    {
        if (strip != nullptr)
            strip->params.solo = ! strip->params.solo.load();
        else if (dca >= 0)
            ctx.engine.getDca (dca).solo = ! ctx.engine.getDca (dca).solo.load();
    };

    muteButton.onClick = [this]
    {
        if (sendSource != nullptr)
        {
            auto& send = sendSource->sends[(size_t) sendSlot];
            send.enabled = ! send.enabled.load();
        }
        else if (strip != nullptr)
            strip->params.mute = ! strip->params.mute.load();
        else if (dca >= 0)
            ctx.engine.getDca (dca).mute = ! ctx.engine.getDca (dca).mute.load();
    };

    addAndMakeVisible (meter);
    startTimerHz (25);
}

void FaderStrip::setTarget (std::optional<state::FaderRef> t)
{
    target = t;
    refresh();
}

void FaderStrip::refresh()
{
    strip = nullptr;
    dca = -1;
    sendSource = nullptr;
    sendSlot = -1;

    if (target)
    {
        if (target->dca)
            dca = juce::jlimit (0, kNumDcas - 1, target->dcaIndex);
        else
            strip = state::findStrip (ctx.engine, target->strip);
    }

    // Sends on Fader between the selected strip and this one.
    if (ctx.sendsOnFader && strip != nullptr)
    {
        auto* sel = ctx.getSelectedStrip ? ctx.getSelectedStrip() : nullptr;
        auto feeds = [] (const Strip& from, const Strip& bus)
        {
            if (! bus.isBus() || &from == &bus)
                return false;
            return from.isInput() || (from.isBus() && from.getId() >= kBusToBusTargets && bus.getId() < kBusToBusTargets);
        };

        if (sel != nullptr && sel != strip)
        {
            if (feeds (*strip, *sel))
            {
                sendSource = strip;
                sendSlot = sel->getId();
            }
            else if (feeds (*sel, *strip))
            {
                sendSource = sel;
                sendSlot = strip->getId();
            }
        }
    }

    bindControls();
    repaint();
}

std::atomic<float>* FaderStrip::faderParam() const noexcept
{
    if (sendSource != nullptr)
        return &sendSource->sends[(size_t) sendSlot].levelDb;
    if (strip != nullptr)
        return &strip->params.faderDb;
    if (dca >= 0)
        return &ctx.engine.getDca (dca).faderDb;
    return nullptr;
}

void FaderStrip::bindControls()
{
    const bool any = strip != nullptr || dca >= 0;
    fader.setEnabled (any);
    selButton.setEnabled (strip != nullptr);
    soloButton.setEnabled (any && sendSource == nullptr);
    muteButton.setEnabled (any);
    meter.setVisible (strip != nullptr);
    fader.setColour (juce::Slider::thumbColourId, sendSource != nullptr ? Colours::solo : Colours::text);
    shownName = {};
    timerCallback();
}

void FaderStrip::timerCallback()
{
    if (auto* p = faderParam(); p != nullptr && ! fader.isMouseButtonDown())
    {
        const double v = p->load();
        if (std::abs (v - fader.getValue()) > 1.0e-4)
            fader.setValue (v, juce::dontSendNotification);
    }

    bool sel = false, solo = false, mute = false, gate = false, dyn = false;
    juce::String name, value;

    if (sendSource != nullptr)
    {
        auto& send = sendSource->sends[(size_t) sendSlot];
        mute = ! send.enabled.load();
        value = fader::text (send.levelDb.load());
    }

    if (strip != nullptr)
    {
        sel = ctx.getSelectedStrip && ctx.getSelectedStrip() == strip;
        solo = strip->params.solo.load();
        if (sendSource == nullptr)
        {
            mute = strip->params.mute.load();
            value = fader::text (strip->params.faderDb.load());
        }
        gate = strip->meters.gateActive.load();
        dyn = strip->meters.dynActive.load();
        name = strip->getName();
    }
    else if (dca >= 0)
    {
        auto& d = ctx.engine.getDca (dca);
        solo = d.solo.load();
        mute = d.mute.load();
        value = fader::text (d.faderDb.load());
        name = ctx.engine.getDcaName (dca);
    }

    selButton.setToggleState (sel, juce::dontSendNotification);
    soloButton.setToggleState (solo, juce::dontSendNotification);
    muteButton.setToggleState (mute, juce::dontSendNotification);

    if (name != shownName || value != shownValue || sel != shownSelected || gate != shownGate || dyn != shownDyn)
    {
        shownName = name;
        shownValue = value;
        shownSelected = sel;
        shownGate = gate;
        shownDyn = dyn;
        repaint();
    }
}

void FaderStrip::paint (juce::Graphics& g)
{
    auto b = getLocalBounds();
    g.setColour (Colours::panel);
    g.fillRect (b.reduced (1, 0));

    // Scribble strip: colour bar + LCD.
    auto s = scribbleArea;
    juce::Colour colour = Colours::outline;
    juce::String label, icon;

    if (strip != nullptr)
    {
        colour = strip->getColour();
        label = strip->getLabel();
        if (const int i = strip->params.icon.load(); i > 0)
            icon = "#" + juce::String (i);
    }
    else if (dca >= 0)
    {
        colour = wingColour (ctx.engine.getDca (dca).colour.load());
        label = "DCA " + juce::String (dca + 1);
    }

    g.setColour (target ? colour : Colours::outline.darker (0.3f));
    g.fillRect (s.removeFromTop (5).reduced (2, 0));
    s.removeFromTop (1);

    auto lcd = s.reduced (2, 0).toFloat();
    g.setColour (juce::Colours::black);
    g.fillRoundedRectangle (lcd, 2.0f);
    if (shownSelected)
    {
        g.setColour (Colours::select);
        g.drawRoundedRectangle (lcd.reduced (0.5f), 2.0f, 1.5f);
    }

    if (target)
    {
        auto t = lcd.reduced (4.0f, 2.0f).toNearestInt();
        auto top = t.removeFromTop (13);
        g.setColour (colour.withMultipliedBrightness (1.15f));
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText (label, top, juce::Justification::centredLeft);
        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (10.0f));
        g.drawText (icon, top, juce::Justification::centredRight);

        if (sendSource != nullptr)
        {
            auto bottom = t.removeFromBottom (12);
            g.setColour (Colours::solo);
            g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
            g.drawFittedText (sendSource == strip ? "> BUS " + juce::String (sendSlot + 1) : "< " + sendSource->getLabel(),
                              bottom, juce::Justification::centred, 1, 0.8f);
        }

        g.setColour (Colours::text);
        g.setFont (juce::FontOptions (juce::jlimit (11.0f, 15.0f, (float) getWidth() / 5.5f), juce::Font::bold));
        g.drawFittedText (shownName.isNotEmpty() ? shownName : label, t, juce::Justification::centred, 2, 0.8f);
    }

    // GATE / DYN LEDs.
    if (strip != nullptr)
    {
        auto leds = ledArea;
        auto drawLed = [&g] (juce::Rectangle<int> r, bool on, juce::Colour c, const char* text)
        {
            const auto pill = r.reduced (1, 0).toFloat();
            g.setColour (on ? c : c.withAlpha (0.12f));
            g.fillRoundedRectangle (pill, 3.0f);
            g.setColour (on ? juce::Colours::black : Colours::textDim);
            g.setFont (juce::FontOptions (8.5f, juce::Font::bold));
            g.drawText (text, r, juce::Justification::centred);
        };
        const int half = leds.getWidth() / 2;
        drawLed (leds.removeFromLeft (half), shownGate, Colours::active, "GATE");
        drawLed (leds, shownDyn, Colours::block, "DYN");
    }

    // Fader value.
    g.setColour (sendSource != nullptr ? Colours::solo : Colours::text);
    g.setFont (juce::FontOptions (11.0f));
    g.drawText (target ? shownValue : juce::String(), valueArea, juce::Justification::centred);
}

void FaderStrip::resized()
{
    auto b = getLocalBounds().reduced (2, 2);
    const int scribbleH = juce::jlimit (40, 60, b.getHeight() / 8);
    scribbleArea = b.removeFromTop (scribbleH);
    b.removeFromTop (3);

    const int buttonH = juce::jlimit (16, 26, b.getHeight() / 16);
    selButton.setBounds (b.removeFromTop (buttonH).reduced (1, 1));
    ledArea = b.removeFromTop (12).reduced (2, 0);
    soloButton.setBounds (b.removeFromTop (buttonH).reduced (1, 1));
    muteButton.setBounds (b.removeFromTop (buttonH).reduced (1, 1));
    b.removeFromTop (3);

    valueArea = b.removeFromBottom (14);
    auto meterArea = b.removeFromLeft (juce::jlimit (8, 14, b.getWidth() / 6));
    meter.setBounds (meterArea.reduced (0, 4));
    fader.setBounds (b);
}

void FaderStrip::mouseDown (const juce::MouseEvent& e)
{
    if (scribbleArea.contains (e.getPosition()) && strip != nullptr && ctx.selectStrip)
        ctx.selectStrip (strip);
}

void FaderStrip::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (scribbleArea.contains (e.getPosition()) && strip != nullptr && ctx.showHomeTab)
    {
        if (ctx.selectStrip)
            ctx.selectStrip (strip);
        ctx.showHomeTab (HomeTab::Name);
    }
}

} // namespace livedaw::ui
