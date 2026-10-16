#include "ui/FaderStrip.h"
#include "dsp/DspUtils.h"

#include <optional>

namespace livedaw::ui
{

void drawLightBar (juce::Graphics& g, juce::Rectangle<float> bar, juce::Colour colour, bool on)
{
    const float radius = bar.getHeight() * 0.5f;
    g.setColour (on ? colour : Colours::ledOff);
    g.fillRoundedRectangle (bar, radius);
    if (on)
    {
        g.setColour (juce::Colours::white.withAlpha (0.25f));
        g.fillRoundedRectangle (bar.withHeight (bar.getHeight() * 0.4f).reduced (radius, 0.0f), radius * 0.4f);
    }
}

void drawScribble (juce::Graphics& g, juce::Rectangle<float> lcd, const ScribbleInfo& info)
{
    // A dark grey LCD (the console's dark rendering) on the black display panel.
    g.setColour (juce::Colour (0xff232427));
    g.fillRect (lcd);
    if (info.number.isEmpty() && info.name.isEmpty())
        return;

    auto t = lcd.reduced (juce::jmax (2.0f, lcd.getWidth() * 0.05f), 2.0f);
    const float h = t.getHeight();

    // The name on an inverted bar.
    auto bar = t.removeFromBottom (juce::jmax (11.0f, h * 0.27f));
    g.setColour (juce::Colours::white);
    g.fillRect (bar.expanded (1.0f, 0.0f));
    g.setColour (juce::Colours::black);
    g.setFont (juce::FontOptions (juce::jlimit (9.0f, 15.0f, bar.getHeight() * 0.86f), juce::Font::bold));
    g.drawFittedText (info.name, bar.toNearestInt().reduced (1, 0), juce::Justification::centredLeft, 1, 0.7f);
    t.removeFromBottom (1.0f);

    // Number (large) with the icon top right, then the two source lines.
    auto top = t.removeFromTop (t.getHeight() * 0.48f);
    if (info.icon.has_value())
    {
        const float s = juce::jmin (top.getHeight(), top.getWidth() * 0.34f);
        drawIcon (g, *info.icon, top.removeFromRight (s).withSizeKeepingCentre (s, s).reduced (1.0f), juce::Colours::white);
    }
    g.setColour (Colours::text);
    g.setFont (juce::FontOptions (juce::jlimit (10.0f, 22.0f, top.getHeight() * 0.95f)));
    g.drawFittedText (info.number, top.toNearestInt(), juce::Justification::centredLeft, 1, 0.7f);

    const float lineH = t.getHeight() * 0.5f;
    g.setFont (juce::FontOptions (juce::jlimit (8.0f, 13.0f, lineH * 0.95f)));
    g.setColour (info.line2Colour);
    g.drawFittedText (info.line2, t.removeFromTop (lineH).toNearestInt(), juce::Justification::centredLeft, 1, 0.7f);
    g.setColour (Colours::textDim);
    g.drawFittedText (info.line3, t.toNearestInt(), juce::Justification::centredLeft, 1, 0.7f);
}

//==============================================================================
FaderStrip::FaderStrip (UiContext& c) : ctx (c)
{
    fader.setSliderStyle (juce::Slider::LinearVertical);
    fader.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    fader.setNormalisableRange (fader::range());
    fader.setDoubleClickReturnValue (true, 0.0);
    fader.setMouseDragSensitivity (400);
    fader.textFromValueFunction = [] (double db) { return fader::text ((float) db); };
    fader.setPopupDisplayEnabled (true, false, this);
    fader.onValueChange = [this]
    {
        if (auto* p = faderParam())
            p->store ((float) fader.getValue());
    };
    addAndMakeVisible (fader);

    selectKey.ledColour = Colours::select;
    soloKey.ledColour = Colours::solo;
    muteKey.ledColour = Colours::mute;
    for (auto* k : { &selectKey, &soloKey, &muteKey })
        addAndMakeVisible (k);

    selectKey.onPress = [this] { pressSelect(); };
    selectKey.ledState = [this] { return strip != nullptr && ctx.getSelectedStrip && ctx.getSelectedStrip() == strip ? SurfaceKey::Lit : SurfaceKey::Off; };

    soloKey.onPress = [this]
    {
        if (strip != nullptr)
            strip->params.solo = ! strip->params.solo.load();
        else if (dca >= 0)
            ctx.engine.getDca (dca).solo = ! ctx.engine.getDca (dca).solo.load();
    };
    soloKey.ledState = [this]
    {
        const bool on = strip != nullptr ? strip->params.solo.load() : (dca >= 0 && ctx.engine.getDca (dca).solo.load());
        return on ? SurfaceKey::Lit : SurfaceKey::Off;
    };

    // Under Sends on Fader, MUTE switches the send (lit while it's off).
    muteKey.onPress = [this]
    {
        if (send)
            send->on->store (! send->on->load());
        else if (strip != nullptr)
            strip->params.mute = ! strip->params.mute.load();
        else if (dca >= 0)
            ctx.engine.getDca (dca).mute = ! ctx.engine.getDca (dca).mute.load();
    };
    muteKey.ledState = [this]
    {
        bool on = false;
        if (send)
            on = ! send->on->load();
        else if (strip != nullptr)
            on = strip->params.mute.load();
        else if (dca >= 0)
            on = ctx.engine.getDca (dca).mute.load();
        return on ? SurfaceKey::Lit : SurfaceKey::Off;
    };

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
    send.reset();

    if (target)
    {
        if (target->dca)
            dca = juce::jlimit (0, kNumDcas - 1, target->dcaIndex);
        else
            strip = state::findStrip (ctx.engine, target->strip);
    }

    // Sends on Fader between the selected strip and this one: the send from an
    // input (or bus) to a bus, main or matrix.
    if (ctx.sendsOnFader && strip != nullptr)
    {
        auto* sel = ctx.getSelectedStrip ? ctx.getSelectedStrip() : nullptr;
        auto sendOf = [] (Strip& from, Strip& to) -> std::optional<SendTarget>
        {
            if (&from == &to)
                return std::nullopt;
            const auto tag = juce::String (label (to.getKind())) + " " + juce::String (to.getNumber());
            switch (to.getKind())
            {
                case StripKind::Bus:
                    if (from.isInput() || (from.isBus() && from.getId() >= kBusToBusTargets && to.getId() < kBusToBusTargets))
                    {
                        auto& s = from.sends[(size_t) to.getId()];
                        return SendTarget { &s.levelDb, &s.enabled, tag };
                    }
                    break;
                case StripKind::Main:
                    if (from.isInput() || from.isBus())
                    {
                        auto& s = from.params.mains[juce::jlimit (0, kNumMains - 1, to.getId())];
                        return SendTarget { &s.levelDb, &s.on, tag };
                    }
                    break;
                case StripKind::Matrix:
                    if (from.isInput() || from.isBus() || from.getKind() == StripKind::Main)
                    {
                        auto& s = from.params.matrix[juce::jlimit (0, kNumMatrices - 1, to.getId())];
                        return SendTarget { &s.levelDb, &s.on, tag };
                    }
                    break;
                case StripKind::Channel:
                case StripKind::Aux:
                    break;
            }
            return std::nullopt;
        };

        if (sel != nullptr && sel != strip)
        {
            // A destination selected: this fader sets its send to it. A source
            // selected: this destination's fader sets the source's send to it.
            if ((send = sendOf (*strip, *sel)))
                send->tag = "> " + send->tag;
            else if ((send = sendOf (*sel, *strip)))
                send->tag = "< " + sel->getLabel().removeCharacters (" ");
        }
    }

    // SETUP > SURFACE may have switched the value readout under the fader.
    if (showValue != ctx.settings.getBoolValue ("ui.faderValues", false))
        resized();

    bindControls();
    repaint();
}

std::atomic<float>* FaderStrip::faderParam() const noexcept
{
    if (send)
        return send->level;
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
    selectKey.setEnabled (strip != nullptr);
    soloKey.setEnabled (any && ! send);
    muteKey.setEnabled (any);
    fader.setColour (juce::Slider::thumbColourId, send ? Colours::solo : Colours::text);
    shownName = {};
    timerCallback();
}

void FaderStrip::pressSelect()
{
    if (strip == nullptr)
        return;

    // SELECT on the selected strip again (SETUP > GENERAL > SEL DBL CLICK):
    // 0 = Channel Home, 1 = INS1 for buses (Channel Home otherwise), 2 = nothing.
    if (ctx.getSelectedStrip && ctx.getSelectedStrip() == strip)
    {
        const int mode = ctx.settings.getIntValue ("ui.selDblClick", 0);
        if (mode != 2 && ctx.showHomeTab)
            ctx.showHomeTab (mode == 1 && strip->isBus() ? HomeTab::PreInsert : HomeTab::Home);
        return;
    }
    if (ctx.selectStrip)
        ctx.selectStrip (strip);
}

ScribbleInfo FaderStrip::scribbleInfo() const
{
    ScribbleInfo info;
    if (strip != nullptr)
    {
        info.number = strip->getLabel().removeCharacters (" ");
        info.name = shownName;
        info.icon = stripIcon (strip->getKind());
        if (strip->isInput())
        {
            // The source ports: "IN 01" / "IN 02", or "IN 03" / "mono".
            auto port = [] (int i) { return i < 0 ? juce::String ("OFF") : "IN " + juce::String (i + 1).paddedLeft ('0', 2); };
            const int left = strip->params.inputLeft.load();
            info.line2 = port (left);
            if (strip->params.stereo.load())
            {
                const int right = strip->params.inputRight.load();
                info.line3 = port (right >= 0 ? right : (left >= 0 ? left + 1 : -1));
            }
            else
            {
                info.line3 = "mono";
            }
        }
    }
    else if (dca >= 0)
    {
        info.number = "DCA" + juce::String (dca + 1);
        info.name = shownName;
    }

    if (send)
    {
        info.line2 = send->tag;
        info.line2Colour = Colours::solo;
    }
    return info;
}

void FaderStrip::timerCallback()
{
    if (auto* p = faderParam(); p != nullptr && ! fader.isMouseButtonDown())
    {
        const double v = p->load();
        if (std::abs (v - fader.getValue()) > 1.0e-4)
            fader.setValue (v, juce::dontSendNotification);
    }

    bool gate = false, dyn = false, light = false;
    int colour = 0;
    juce::String name, value, source;
    std::array<int, 2> lit {};

    if (auto* p = faderParam())
        value = fader::text (p->load());

    if (strip != nullptr)
    {
        gate = strip->meters.gateActive.load();
        dyn = strip->meters.dynActive.load();
        name = stripName (*strip);
        light = strip->params.led.load();
        colour = strip->getColourIndex();
        source = juce::String (strip->params.inputLeft.load()) + "/" + juce::String (strip->params.inputRight.load())
                 + (strip->params.stereo.load() ? "s" : "m");

        // Input level, like the console's strip meters.
        const int channels = juce::jlimit (1, 2, strip->getNumChannels());
        for (int ch = 0; ch < 2; ++ch)
        {
            const float db = dsp::gainToDb (ctx.meters.get (strip, true, juce::jmin (ch, channels - 1)));
            lit[(size_t) ch] = leds::countLit (kMeterThresholds, kMeterSegments, db);
        }
    }
    else if (dca >= 0)
    {
        auto& d = ctx.engine.getDca (dca);
        name = ctx.engine.getDcaName (dca);
        light = true;
        colour = d.colour.load();
    }

    if (name != shownName || colour != shownColour || light != shownLight || source != shownSource)
    {
        shownName = name;
        shownColour = colour;
        shownLight = light;
        shownSource = source;
        repaint (scribbleArea);
    }
    if (lit != shownLit || gate != shownGate || dyn != shownDyn)
    {
        shownLit = lit;
        shownGate = gate;
        shownDyn = dyn;
        repaint (meterArea);
    }
    if (value != shownValue)
    {
        shownValue = value;
        if (showValue)
            repaint (valueArea);
    }
}

void FaderStrip::setModuleEdges (bool first, bool last)
{
    moduleFirst = first;
    moduleLast = last;
    repaint();
}

void FaderStrip::paint (juce::Graphics& g)
{
    g.fillAll (Colours::panel);

    // The black display panel and the darker meter panel run across the strips
    // of a module of four, rounded at its outer edges.
    auto band = [this, &g] (juce::Rectangle<int> rows, int margin, juce::Colour colour)
    {
        const float left = moduleFirst ? 3.0f : 0.0f, right = (float) getWidth() - (moduleLast ? 3.0f : 0.0f);
        juce::Path p;
        p.addRoundedRectangle (left, (float) (rows.getY() - margin), right - left, (float) (rows.getHeight() + 2 * margin), 4.0f, 4.0f,
                               moduleFirst, moduleLast, moduleFirst, moduleLast);
        g.setColour (colour);
        g.fillPath (p);
    };
    band (scribbleArea, 4, juce::Colours::black);
    band (meterArea, 4, juce::Colour (0xff141517));

    // Colour light bar and scribble strip.
    const juce::Colour colour = strip != nullptr ? strip->getColour() : (dca >= 0 ? wingColour (shownColour) : Colours::ledOff);
    drawLightBar (g, barArea.toFloat(), colour, target.has_value() && shownLight);
    drawScribble (g, lcdArea.toFloat(), target ? scribbleInfo() : ScribbleInfo {});

    // Meter block: DYN LED, two 6-segment columns (input level), GATE LED.
    if (strip != nullptr)
    {
        auto m = meterArea.toFloat();
        const float rowH = m.getHeight() / (float) kMeterRows;
        const float ledW = juce::jlimit (5.0f, 10.0f, m.getWidth() * 0.14f);
        const float pairW = ledW * 2.8f;
        auto row = [&] (int i) { return juce::Rectangle<float> (m.getX(), m.getY() + rowH * (float) i, m.getWidth(), rowH); };
        auto barLed = [&] (int i, bool on, juce::Colour c)
        {
            auto r = row (i).withSizeKeepingCentre (pairW, juce::jmax (2.0f, rowH * 0.55f));
            g.setColour (on ? c : Colours::ledOff);
            g.fillRect (r);
        };
        barLed (0, shownDyn, Colours::block);
        barLed (kMeterRows - 1, shownGate, Colours::active);

        auto columns = juce::Rectangle<float> (m.getCentreX() - pairW * 0.5f, m.getY() + rowH, pairW, rowH * kMeterSegments);
        const bool stereo = strip->getNumChannels() > 1;
        auto colourFor = [] (int i) { return leds::meterColour (kMeterThresholds[i]); };
        leds::drawColumn (g, columns.removeFromLeft (ledW), kMeterSegments, shownLit[0], colourFor);
        leds::drawColumn (g, columns.removeFromRight (ledW), kMeterSegments, stereo ? shownLit[1] : shownLit[0], colourFor);
    }

    if (showValue && target)
    {
        g.setColour (send ? Colours::solo : Colours::textDim);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (shownValue, valueArea, juce::Justification::centred);
    }
}

void FaderStrip::resized()
{
    auto b = getLocalBounds().reduced (2, 3);
    const int w = b.getWidth();
    const int innerW = juce::roundToInt ((float) w * 0.88f);

    // Colour bar above the LCD (about a fifth of its height, half of that apart).
    const int lcdH = juce::jlimit (42, 78, juce::roundToInt ((float) w * 0.9f));
    const int barH = juce::jlimit (4, 8, lcdH / 7);
    scribbleArea = b.removeFromTop (barH + 4 + lcdH).withSizeKeepingCentre (innerW, barH + 4 + lcdH);
    barArea = scribbleArea.withHeight (barH);
    lcdArea = scribbleArea.withTrimmedTop (barH + 4);
    b.removeFromTop (8);

    const int keyH = juce::jlimit (18, 30, juce::roundToInt ((float) w * 0.46f));
    const int keyW = juce::roundToInt ((float) w * 0.76f);
    auto key = [&] (SurfaceKey& k)
    {
        k.setBounds (b.removeFromTop (keyH).withSizeKeepingCentre (keyW, keyH));
    };
    key (selectKey);
    b.removeFromTop (5);
    key (soloKey);
    b.removeFromTop (8);
    meterArea = b.removeFromTop (juce::jlimit (56, 100, juce::roundToInt ((float) keyH * 3.3f)));
    b.removeFromTop (8);
    key (muteKey);
    b.removeFromTop (6);

    showValue = ctx.settings.getBoolValue ("ui.faderValues", false);
    valueArea = showValue ? b.removeFromBottom (14) : juce::Rectangle<int>();
    fader.setBounds (b);
}

void FaderStrip::mouseDown (const juce::MouseEvent& e)
{
    // Touching the scribble strip acts like SELECT.
    if (scribbleArea.contains (e.getPosition()))
        pressSelect();
}

} // namespace livedaw::ui
