#include "ui/FaderStrip.h"
#include "dsp/DspUtils.h"
#include "engine/Models.h"

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
    // A dark grey LCD (the console's dark rendering) on the black display panel;
    // SCRIBBLE sets its backlight and SCR CONT the text contrast.
    g.setColour (juce::Colour (0xff232427).withMultipliedBrightness (juce::jlimit (0.2f, 1.6f, info.backlight)));
    g.fillRect (lcd);
    if (info.number.isEmpty() && info.name.isEmpty())
        return;
    const float ink = juce::jlimit (0.35f, 1.0f, info.contrast);
    const auto white = juce::Colours::white.withMultipliedBrightness (ink);

    auto t = lcd.reduced (juce::jmax (2.0f, lcd.getWidth() * 0.05f), 2.0f);
    const float h = t.getHeight();

    // The name on an inverted bar.
    auto bar = t.removeFromBottom (juce::jmax (11.0f, h * 0.27f));
    g.setColour (white);
    g.fillRect (bar.expanded (1.0f, 0.0f));
    g.setColour (juce::Colours::black);
    g.setFont (juce::FontOptions (juce::jlimit (9.0f, 15.0f, bar.getHeight() * 0.86f), juce::Font::bold));
    g.drawFittedText (info.name, bar.toNearestInt().reduced (1, 0), juce::Justification::centredLeft, 1, 0.7f);
    t.removeFromBottom (1.0f);

    // Number (large) with the icon (or the fader value) top right, then the two source lines.
    auto top = t.removeFromTop (t.getHeight() * 0.48f);
    if (info.value.isNotEmpty())
    {
        auto valueArea = top.removeFromRight (top.getWidth() * 0.5f);
        g.setColour (white);
        g.setFont (juce::FontOptions (juce::jlimit (9.0f, 18.0f, valueArea.getHeight() * 0.8f), juce::Font::bold));
        g.drawFittedText (info.value, valueArea.toNearestInt(), juce::Justification::centredRight, 1, 0.6f);
    }
    else if (info.icon.has_value())
    {
        const float s = juce::jmin (top.getHeight(), top.getWidth() * 0.34f);
        const auto iconArea = top.removeFromRight (s).withSizeKeepingCentre (s, s).reduced (1.0f);
        if (info.strip != nullptr)
            drawStripIcon (g, *info.strip, iconArea, white);
        else
            drawIcon (g, *info.icon, iconArea, white);
    }
    g.setColour (Colours::text.withMultipliedBrightness (ink));
    g.setFont (juce::FontOptions (juce::jlimit (10.0f, 22.0f, top.getHeight() * 0.95f)));
    g.drawFittedText (info.number, top.toNearestInt(), juce::Justification::centredLeft, 1, 0.7f);

    const float lineH = t.getHeight() * 0.5f;
    g.setFont (juce::FontOptions (juce::jlimit (8.0f, 13.0f, lineH * 0.95f)));
    g.setColour (info.line2Colour.withMultipliedBrightness (ink));
    g.drawFittedText (info.line2, t.removeFromTop (lineH).toNearestInt(), juce::Justification::centredLeft, 1, 0.7f);
    g.setColour (Colours::textDim.withMultipliedBrightness (ink));
    g.drawFittedText (info.line3, t.toNearestInt(), juce::Justification::centredLeft, 1, 0.7f);
}

//==============================================================================
namespace
{
    /** CH AUTOSELECT's deadtime: when several faders move at once, only the
        first selects its strip. */
    juce::uint32 lastAutoSelect = 0;

    /** FX parameter faders (FX SPILL): the FX file colour. */
    const juce::Colour kFxColour { 0xffc47ac4 };
} // namespace

FaderStrip::FaderStrip (UiContext& c, LayerSet::Section s) : ctx (c), section (s)
{
    fader.setSliderStyle (juce::Slider::LinearVertical);
    fader.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    fader.setNormalisableRange (fader::range());
    fader.setDoubleClickReturnValue (true, 0.0);
    fader.setMouseDragSensitivity (400);
    fader.textFromValueFunction = [] (double db) { return fader::text ((float) db); };
    fader.setPopupDisplayEnabled (true, false, this);
    fader.onValueChange = [this] { writeFader (fader.getValue()); };
    fader.onDragStart = [this]
    {
        // CH AUTOSELECT (SETUP > SURFACE): moving a fader selects its strip.
        const auto now = juce::Time::getMillisecondCounter();
        if (strip != nullptr && ctx.settings.getBoolValue (uiprefs::autoSelect[juce::jlimit (0, 2, (int) section)], false)
            && now - lastAutoSelect > 300 && ctx.selectStrip && (! ctx.getSelectedStrip || ctx.getSelectedStrip() != strip))
            ctx.selectStrip (strip);
        lastAutoSelect = now;
    };
    addAndMakeVisible (fader);

    selectKey.ledColour = Colours::select;
    soloKey.ledColour = Colours::solo;
    muteKey.ledColour = Colours::mute;
    for (auto* k : { &selectKey, &soloKey, &muteKey })
        addAndMakeVisible (k);

    selectKey.onPress = [this] { pressSelect(); };
    selectKey.ledState = [this]
    {
        // A DCA's SELECT lights while it spills (DCA SPILL).
        if (dca >= 0)
            return isSpilling (ctx, { LayerSet::Spill::Kind::Dca, dca, {} }) ? SurfaceKey::Lit : SurfaceKey::Off;
        return strip != nullptr && ctx.getSelectedStrip && ctx.getSelectedStrip() == strip ? SurfaceKey::Lit : SurfaceKey::Off;
    };

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

    // Under Sends on Fader, MUTE switches the send (lit while it's off). A
    // strip muted by a mute group, a DCA or SIP flashes; with MUTEGROUP/SIP
    // OVERRIDE, MUTE unmutes it (MixerEngine::pressMute).
    muteKey.onPress = [this]
    {
        if (fxp)
        {
            // FX SPILL (firmware 2.1): MUTE sets the parameter to its default (an EQ gain to 0 dB).
            fxp->value->store (models::model (models::Fx, fxp->model).params[(size_t) fxp->index].defaultValue);
        }
        else if (send)
            send->on->store (! send->on->load());
        else if (strip != nullptr)
            ctx.engine.pressMute (*strip);
        else if (dca >= 0)
            ctx.engine.getDca (dca).mute = ! ctx.engine.getDca (dca).mute.load();
    };
    muteKey.ledState = [this]
    {
        if (fxp)
            return atDefault() ? SurfaceKey::Lit : SurfaceKey::Off;
        if (send)
            return ! send->on->load() ? SurfaceKey::Lit : SurfaceKey::Off;
        if (strip != nullptr)
        {
            if (strip->params.mute.load())
                return SurfaceKey::Lit;
            return strip->getMuteState() == ld::kMuteGroup ? SurfaceKey::Blink : SurfaceKey::Off;
        }
        return dca >= 0 && ctx.engine.getDca (dca).mute.load() ? SurfaceKey::Lit : SurfaceKey::Off;
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
    fxp.reset();

    if (target)
    {
        if (target->dca)
            dca = juce::jlimit (0, kNumDcas - 1, target->dcaIndex);
        else if (target->fxParam)
        {
            const int slot = juce::jlimit (0, kNumFx - 1, target->fxSlot);
            const int model = ctx.engine.getFx().getModel (slot);
            if (model > ld::kFxExt && juce::isPositiveAndBelow (target->param, (int) models::model (models::Fx, model).params.size()))
                fxp = ParamTarget { &ctx.engine.getFx().params (slot).values[target->param], slot, model, target->param };
        }
        else
            strip = state::findStrip (ctx.engine, target->strip); // a send fader: its source
    }

    // Sends on Fader between the selected strip (or the ALTERNATIVE SOF
    // strip) and this one: the send from an input (or bus) to a bus, main or
    // matrix. RIGHT SECTION SENDS ON FADERS off keeps the right section's faders.
    const bool sofHere = section != LayerSet::Right || ctx.settings.getBoolValue (uiprefs::rightSectionSof, true);
    const bool sendFader = target.has_value() && target->send;
    if ((sendFader || (ctx.sendsOnFader && sofHere)) && strip != nullptr)
    {
        auto* sel = ctx.sofDestination.has_value() ? state::findStrip (ctx.engine, *ctx.sofDestination)
                                                   : (ctx.getSelectedStrip ? ctx.getSelectedStrip() : nullptr);
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

        auto* dest = sendFader ? state::findStrip (ctx.engine, target->to) : nullptr;
        if (sendFader)
        {
            // A send fader of a layer or spill: the source's send to its destination.
            if (dest != nullptr && (send = sendOf (*strip, *dest)))
                send->tag = "> " + send->tag;
        }
        else if (sel != nullptr && sel != strip)
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

    // ALTERNATIVE SOF MODE: holding SELECT makes this strip the fixed SOF strip.
    if (ctx.settings.getBoolValue (uiprefs::alternativeSof, false))
        selectKey.onHold = [this]
        {
            if (strip == nullptr)
                return;
            ctx.sofDestination = state::StripRef::of (*strip);
            ctx.sendsOnFader = true;
            if (ctx.sofChanged)
                ctx.sofChanged();
        };
    else
        selectKey.onHold = nullptr;

    retargeted = true;
    bindControls();
    repaint();
}

void FaderStrip::glide (double targetDb)
{
    // FADER SPEED (SETUP > SURFACE): the whole travel takes 400, 200 or 80 ms.
    const auto& range = fader.getNormalisableRange();
    const double to = range.convertTo0to1 (juce::jlimit (range.start, range.end, targetDb));
    const double from = range.convertTo0to1 (juce::jlimit (range.start, range.end, fader.getValue()));
    const double step = (1000.0 / 25.0) / (double) uiprefs::faderGlideMs (ctx.settings);
    if (std::abs (to - from) <= step)
        fader.setValue (targetDb, juce::dontSendNotification);
    else
        fader.setValue (range.convertFrom0to1 (from + (to > from ? step : -step)), juce::dontSendNotification);
}

double FaderStrip::faderPosition() const
{
    if (fxp)
    {
        const auto& spec = models::model (models::Fx, fxp->model).params[(size_t) fxp->index];
        return fader.getNormalisableRange().convertFrom0to1 (juce::jlimit (0.0, 1.0, (double) spec.toNormalised (fxp->value->load())));
    }
    auto* p = faderParam();
    return p != nullptr ? (double) p->load() : 0.0;
}

void FaderStrip::writeFader (double position)
{
    if (fxp)
    {
        const auto& spec = models::model (models::Fx, fxp->model).params[(size_t) fxp->index];
        fxp->value->store (spec.fromNormalised ((float) fader.getNormalisableRange().convertTo0to1 (position)));
    }
    else if (auto* p = faderParam())
        p->store ((float) position);
}

bool FaderStrip::atDefault() const
{
    if (! fxp)
        return false;
    const auto& spec = models::model (models::Fx, fxp->model).params[(size_t) fxp->index];
    return std::abs (fxp->value->load() - spec.defaultValue) < 1.0e-4f;
}

std::atomic<float>* FaderStrip::faderParam() const noexcept
{
    if (fxp)
        return fxp->value;
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
    const bool any = strip != nullptr || dca >= 0 || fxp.has_value();
    fader.setEnabled (any);
    // A DCA's SELECT spills it when DCA SPILL is on (SETUP > SURFACE).
    selectKey.setEnabled (strip != nullptr || dca >= 0);
    soloKey.setEnabled ((strip != nullptr || dca >= 0) && ! send);
    muteKey.setEnabled (any);
    fader.setColour (juce::Slider::thumbColourId, send ? Colours::solo : (fxp ? kFxColour : Colours::text));
    fader.textFromValueFunction = [this] (double v)
    {
        if (fxp)
        {
            const auto& spec = models::model (models::Fx, fxp->model).params[(size_t) fxp->index];
            return spec.display (spec.fromNormalised ((float) fader.getNormalisableRange().convertTo0to1 (v)));
        }
        return fader::text ((float) v);
    };
    shownName = {};
    timerCallback();
}

void FaderStrip::pressSelect()
{
    // DCA SPILL: selecting a DCA shows its strips on the left section (the
    // center for a DCA on the right section); again ends the spill.
    if (dca >= 0 && ctx.settings.getBoolValue (uiprefs::dcaSpill, false))
    {
        toggleSpill (ctx, section, { LayerSet::Spill::Kind::Dca, dca, {} });
        return;
    }
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
        info.strip = strip;
        if (strip->isInput())
        {
            // The source ports: "IN 01" / "IN 02", or "IN 03" / "mono"; with
            // SHOW SOURCE ON SCRIBBLE off only "mono" or "stereo".
            const bool stereo = strip->params.stereo.load();
            if (ctx.settings.getBoolValue (uiprefs::sourceOnScribble, true))
            {
                auto port = [] (int i) { return i < 0 ? juce::String ("OFF") : inputPortName (i, 2); };
                const int left = strip->getActiveInputLeft();
                info.line2 = port (left);
                if (stereo)
                {
                    const int right = strip->getActiveInputRight();
                    info.line3 = port (right >= 0 ? right : (left >= 0 ? left + 1 : -1));
                }
                else
                {
                    info.line3 = "mono";
                }
            }
            else
            {
                info.line2 = stereo ? "stereo" : "mono";
            }
        }
    }
    else if (dca >= 0)
    {
        info.number = "DCA" + juce::String (dca + 1);
        info.name = shownName;
    }
    else if (fxp)
    {
        // FX SPILL: the slot, the device, the parameter and its value.
        const auto& m = models::model (models::Fx, fxp->model);
        info.number = "FX" + juce::String (fxp->slot + 1);
        info.line2 = m.name.toUpperCase();
        info.line2Colour = kFxColour;
        info.line3 = shownValue;
        info.name = m.params[(size_t) fxp->index].label.toUpperCase();
        info.icon = Icon::Insert;
    }

    if (send)
    {
        info.line2 = send->tag;
        info.line2Colour = Colours::solo;
    }

    // SHOW FADER VALUE ON SCRIBBLE: the level for a moment after a move.
    if (shownFlash)
        info.value = shownValue;
    info.backlight = uiprefs::brightness (ctx.settings, "scribble", 60);
    info.contrast = uiprefs::brightness (ctx.settings, "scrCont", 50);
    return info;
}

void FaderStrip::timerCallback()
{
    if (faderParam() != nullptr && ! fader.isMouseButtonDown())
    {
        const double v = faderPosition();
        if (std::abs (v - fader.getValue()) > 1.0e-4)
            glide (v);
    }

    bool gate = false, dyn = false, light = false;
    int colour = 0;
    juce::String name, value, source;
    std::array<int, 2> lit {};

    if (fxp)
        value = models::model (models::Fx, fxp->model).params[(size_t) fxp->index].display (fxp->value->load());
    else if (auto* p = faderParam())
        value = fader::text (p->load());

    if (strip != nullptr)
    {
        gate = strip->meters.gateActive.load();
        dyn = strip->meters.dynActive.load();
        name = stripName (*strip);
        light = strip->params.led.load();
        colour = strip->getColourIndex();
        source = juce::String (strip->params.inputLeft.load()) + "/" + juce::String (strip->params.inputRight.load())
                 + (strip->params.stereo.load() ? "s" : "m") + juce::String (strip->getAltInputLeft());

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
    const auto now = juce::Time::getMillisecondCounter();
    if (value != shownValue)
    {
        // A move (not a new target) shows the value on the scribble strip for 1.5 s.
        if (! retargeted && ctx.settings.getBoolValue (uiprefs::faderValueOnScribble, false))
            valueFlashUntil = now + 1500;
        shownValue = value;
        if (showValue)
            repaint (valueArea);
        if (shownFlash)
            repaint (scribbleArea);
    }
    retargeted = false;
    if (const bool flash = now < valueFlashUntil; flash != shownFlash)
    {
        shownFlash = flash;
        repaint (scribbleArea);
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

    // Colour light bar (COLOR LED brightness) and scribble strip.
    const juce::Colour colour = strip != nullptr ? stripDisplayColour (*strip)
                                : (dca >= 0 ? wingColour (shownColour) : (fxp ? kFxColour : Colours::ledOff));
    drawLightBar (g, barArea.toFloat(), colour.withMultipliedBrightness (uiprefs::brightness (ctx.settings, "colorLed", 25)),
                  target.has_value() && shownLight);
    drawScribble (g, lcdArea.toFloat(), target ? scribbleInfo() : ScribbleInfo {});

    // Meter block: DYN LED, two 6-segment columns (input level), GATE LED.
    if (strip != nullptr)
    {
        auto m = meterArea.toFloat();
        const float rowH = m.getHeight() / (float) kMeterRows;
        const float ledW = juce::jlimit (5.0f, 10.0f, m.getWidth() * 0.14f);
        const float pairW = ledW * 2.8f;
        auto row = [&] (int i) { return juce::Rectangle<float> (m.getX(), m.getY() + rowH * (float) i, m.getWidth(), rowH); };
        // SETUP > GENERAL > METERS sets the LED brightness.
        const float bright = uiprefs::brightness (ctx.settings, "meters", 40);
        auto barLed = [&] (int i, bool on, juce::Colour c)
        {
            auto r = row (i).withSizeKeepingCentre (pairW, juce::jmax (2.0f, rowH * 0.55f));
            g.setColour (on ? c.withMultipliedBrightness (bright) : Colours::ledOff);
            g.fillRect (r);
        };
        barLed (0, shownDyn, Colours::block);
        barLed (kMeterRows - 1, shownGate, Colours::active);

        auto columns = juce::Rectangle<float> (m.getCentreX() - pairW * 0.5f, m.getY() + rowH, pairW, rowH * kMeterSegments);
        const bool stereo = strip->getNumChannels() > 1;
        auto colourFor = [bright] (int i) { return leds::meterColour (kMeterThresholds[i]).withMultipliedBrightness (bright); };
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
