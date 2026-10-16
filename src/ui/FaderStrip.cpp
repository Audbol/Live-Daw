#include "ui/FaderStrip.h"

#include <optional>

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

    // Scribble strip, like the console's colour LCD: label and format top left,
    // the kind's icon top right, the name on a bar in the strip's colour.
    auto s = scribbleArea;
    juce::Colour colour = Colours::outline;
    juce::String label, format;
    std::optional<Icon> kindIcon;

    if (strip != nullptr)
    {
        colour = strip->getColour();
        label = strip->getLabel();
        format = strip->isOutput() || strip->params.stereo.load() ? "ST" : "MONO";
        kindIcon = stripIcon (strip->getKind());
    }
    else if (dca >= 0)
    {
        colour = wingColour (ctx.engine.getDca (dca).colour.load());
        label = "DCA " + juce::String (dca + 1);
        format = "DCA";
    }

    auto lcd = s.reduced (2, 0).toFloat();
    g.setColour (juce::Colours::black);
    g.fillRoundedRectangle (lcd, 2.0f);

    if (target)
    {
        auto t = lcd.reduced (3.0f, 2.0f);
        auto bar = t.removeFromBottom (juce::jmax (14.0f, t.getHeight() * 0.36f));
        g.setColour (colour);
        g.fillRect (bar);
        g.setColour (colour.getPerceivedBrightness() > 0.45f ? juce::Colours::black : juce::Colours::white);
        g.setFont (juce::FontOptions (juce::jlimit (11.0f, 16.0f, bar.getHeight() * 0.82f), juce::Font::bold));
        g.drawFittedText (shownName.isNotEmpty() ? shownName : label, bar.toNearestInt().reduced (2, 0), juce::Justification::centred, 1, 0.75f);

        t.removeFromBottom (1.0f);
        auto icon = t.removeFromRight (juce::jmin (t.getHeight(), t.getWidth() * 0.4f));
        if (kindIcon.has_value())
            drawIcon (g, *kindIcon, icon.reduced (2.0f), juce::Colours::white);
        g.setColour (Colours::text);
        g.setFont (juce::FontOptions (juce::jlimit (10.0f, 14.0f, t.getHeight() * 0.42f), juce::Font::bold));
        g.drawFittedText (label, t.removeFromTop (t.getHeight() / 2.0f).toNearestInt(), juce::Justification::bottomLeft, 1, 0.7f);
        if (sendSource != nullptr)
        {
            g.setColour (Colours::solo);
            g.drawFittedText (sendSource == strip ? "> BUS " + juce::String (sendSlot + 1) : "< " + sendSource->getLabel(), t.toNearestInt(),
                              juce::Justification::topLeft, 1, 0.7f);
        }
        else
        {
            g.setColour (Colours::textDim);
            g.drawFittedText (format, t.toNearestInt(), juce::Justification::topLeft, 1, 0.7f);
        }
    }

    if (shownSelected)
    {
        g.setColour (Colours::select);
        g.drawRoundedRectangle (lcd.reduced (0.5f), 2.0f, 2.0f);
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
    const int scribbleH = juce::jlimit (46, 70, b.getHeight() / 7);
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
