#include "ui/FaderStrip.h"
#include "dsp/DspUtils.h"
#include "engine/MidiRemote.h"
#include "engine/Models.h"

#include <optional>

namespace livedaw::ui
{

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
    fader.getProperties().set ("surfaceFader", true);
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
        // The COMPACT has one setting for its surface (autosel_CMPCT).
        const char* autoSelect = ctx.consoleModel == ConsoleModel::Compact ? uiprefs::autoSelectCompact
                                                                           : uiprefs::autoSelect[juce::jlimit (0, 2, (int) section)];
        if (strip != nullptr && ctx.settings.getBoolValue (autoSelect, false)
            && now - lastAutoSelect > 300 && ctx.selectStrip && (! ctx.getSelectedStrip || ctx.getSelectedStrip() != strip))
            ctx.selectStrip (strip);
        lastAutoSelect = now;
    };
    addAndMakeVisible (fader);

    selectKey.ledColour = Colours::keySelect;
    soloKey.ledColour = Colours::keySolo;
    muteKey.ledColour = Colours::keyMute;
    for (auto* k : { &selectKey, &soloKey, &muteKey })
    {
        k->backlit = true;
        addAndMakeVisible (k);
    }

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
    // A blank layer slot is an inactive strip, as past the end of a layer.
    target = t.has_value() && t->isEmpty() ? std::nullopt : t;
    refresh();
}

void FaderStrip::refresh()
{
    strip = nullptr;
    dca = -1;
    send.reset();
    fxp.reset();
    midi.reset();

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
        else if (target->midiCc)
            midi = MidiTarget { target->midiChannel, target->cc };
        else
            strip = state::findStrip (ctx.engine, target->strip); // a send fader: its source
    }

    // Sends on Fader between the selected strip (or the ALTERNATIVE SOF
    // strip) and this one: the send from an input (or bus) to a bus, main or
    // matrix. RIGHT SECTION SENDS ON FADERS off keeps the right section's faders.
    const bool sofHere = section != LayerSet::Right || ctx.settings.getBoolValue (uiprefs::rightSectionSof, uiprefs::rightSectionSofDefault);
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

int FaderStrip::midiValue() const noexcept
{
    return midi && ctx.midi != nullptr ? ctx.midi->getExternalCc (midi->channel + 1, midi->cc) : 0;
}

double FaderStrip::faderPosition() const
{
    if (midi)
        return fader.getNormalisableRange().convertFrom0to1 ((double) midiValue() / 127.0);
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
    if (midi)
    {
        const int value = juce::roundToInt (fader.getNormalisableRange().convertTo0to1 (position) * 127.0);
        if (ctx.midi != nullptr && value != midiValue())
        {
            ctx.midi->setExternalCc (midi->channel + 1, midi->cc, value);
            ctx.midi->sendExternal (juce::MidiMessage::controllerEvent (midi->channel + 1, midi->cc, value));
        }
    }
    else if (fxp)
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

bool FaderStrip::hasControl() const noexcept
{
    return strip != nullptr || dca >= 0 || fxp.has_value() || midi.has_value();
}

void FaderStrip::bindControls()
{
    const bool any = strip != nullptr || dca >= 0 || fxp.has_value();
    fader.setEnabled (any || midi.has_value());
    // A DCA's SELECT spills it when DCA SPILL is on (SETUP > SURFACE).
    selectKey.setEnabled (strip != nullptr || dca >= 0);
    soloKey.setEnabled ((strip != nullptr || dca >= 0) && ! send);
    muteKey.setEnabled (any);
    fader.setColour (juce::Slider::thumbColourId, send ? Colours::solo : (fxp ? kFxColour : (midi ? Colours::midiCc : Colours::text)));
    fader.textFromValueFunction = [this] (double v)
    {
        if (midi)
            return juce::String (juce::roundToInt (fader.getNormalisableRange().convertTo0to1 (v) * 127.0));
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
        const int mode = ctx.settings.getIntValue ("ui.selDblClick", 1);
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
        setScribbleStrip (info, *strip, ctx.settings.getBoolValue (uiprefs::sourceOnScribble, true));
    }
    else if (dca >= 0)
    {
        info.tag = "DCA";
        info.tagFirst = true;
        info.number = juce::String (dca + 1);
        // The default name ("DCA.3") just repeats the number.
        const bool defaultName = shownName.retainCharacters ("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz")
                                     .equalsIgnoreCase ("DCA" + juce::String (dca + 1));
        info.name = defaultName ? juce::String() : shownName;
        info.wingIcon = ctx.engine.getDca (dca).icon.load();
    }
    else if (fxp)
    {
        // FX SPILL: the slot and its device, the parameter's value large and its name.
        const auto& m = models::model (models::Fx, fxp->model);
        info.tag = "FX" + juce::String (fxp->slot + 1);
        info.tagFirst = true;
        info.number = m.name.toUpperCase();
        info.value = shownValue;
        info.name = m.params[(size_t) fxp->index].label.toUpperCase();
    }
    else if (midi)
    {
        // A MIDI CC: the CC and its value large, the MIDI channel.
        info.tag = "CC";
        info.tagFirst = true;
        info.number = juce::String (midi->cc);
        info.value = shownValue;
        info.name = "MIDI CH " + juce::String (midi->channel + 1);
    }

    // Sends on Fader: the send this fader sets ("> BUS 3", "< CH5").
    if (send)
        info.sendTag = send->tag;

    // SHOW FADER VALUE ON SCRIBBLE: the level in place of the icon for a
    // moment after a move.
    if (shownFlash && ! fxp && ! midi)
    {
        info.value = shownValue;
        info.caption = send ? "SEND" : "";
    }
    info.backlight = uiprefs::brightness (ctx.settings, "scribble", 60);
    info.contrast = uiprefs::brightness (ctx.settings, "scrCont", 50);
    return info;
}

void FaderStrip::timerCallback()
{
    if ((faderParam() != nullptr || midi) && ! fader.isMouseButtonDown())
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
    else if (midi)
        value = juce::String (midiValue());
    else if (auto* p = faderParam())
        value = fader::text (p->load());

    if (strip != nullptr)
    {
        gate = strip->meters.gateActive.load();
        dyn = strip->meters.dynActive.load();
        name = stripName (*strip);
        // The light bar goes off on a channel without a source (firmware 1.10).
        light = strip->params.led.load() && stripHasSource (*strip);
        colour = strip->getColourIndex();
        // The source tag (ALT source switches, SHOW SOURCE ON SCRIBBLE) and the icon.
        source = scribbleSourceTag (*strip, ctx.settings.getBoolValue (uiprefs::sourceOnScribble, true)) + "|"
                 + juce::String (strip->getIcon());

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
        light = d.led.load();
        colour = d.colour.load();
        source = juce::String (d.icon.load()); // repaints when the icon changes
    }
    else if (midi)
        light = true;

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
        if (shownFlash || fxp || midi)
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
    if (moduleFirst == first && moduleLast == last)
        return;
    moduleFirst = first;
    moduleLast = last;
    resized();
    repaint();
}

void FaderStrip::paint (juce::Graphics& g)
{
    g.fillAll (surfaceColours().surface);

    // The black display window and the meter panel run across the strips of
    // a module of four, rounded at its outer edges.
    auto band = [this, &g] (juce::Rectangle<int> rows, int margin, juce::Colour colour)
    {
        if (rows.isEmpty())
            return;
        const float left = moduleFirst ? 4.0f : 0.0f, right = (float) getWidth() - (moduleLast ? 4.0f : 0.0f);
        juce::Path p;
        p.addRoundedRectangle (left, (float) (rows.getY() - margin), right - left, (float) (rows.getHeight() + 2 * margin), 4.0f, 4.0f,
                               moduleFirst, moduleLast, moduleFirst, moduleLast);
        g.setColour (colour);
        g.fillPath (p);
        // A faint bevel along the window's top edge.
        const float inset = moduleFirst ? 3.0f : 0.0f;
        g.setColour (juce::Colours::white.withAlpha (0.07f));
        g.fillRect (left + inset, (float) (rows.getY() - margin), right - left - inset - (moduleLast ? 3.0f : 0.0f), 1.0f);
    };
    if (scribbleShown)
        band (scribbleArea, 0, juce::Colours::black);
    band (meterArea, 4, juce::Colour (0xff0c0d0e));

    // A blank slot, a strip past the end of the layer or a target that's gone:
    // the light bar dark, the LCD unlit and empty, no value under the fader.
    const bool active = hasControl();
    // Colour light bar (COLOR LED brightness) and scribble strip; one LCD
    // runs across the module, with a fine rule between the strips.
    if (scribbleShown)
    {
        const juce::Colour colour = strip != nullptr ? stripDisplayColour (*strip)
                                    : (dca >= 0 ? wingColour (shownColour) : (fxp ? kFxColour : (midi ? Colours::midiCc : surfaceColours().ledOff)));
        drawLightBar (g, barArea.toFloat(), lightBarColour (colour).withMultipliedBrightness (uiprefs::brightness (ctx.settings, "colorLed", 25)),
                      active && shownLight);
        const auto info = active ? scribbleInfo() : blankScribble();
        drawScribble (g, lcdArea.toFloat(), info);
        drawScribbleFrame (g, lcdArea.toFloat(), moduleFirst, moduleLast, info.backlight);
    }

    // Meter block: DYN LED, two 6-segment columns (input level), GATE LED.
    if (strip != nullptr && ! meterArea.isEmpty())
    {
        auto m = meterArea.toFloat();
        const float rowH = m.getHeight() / (float) kMeterRows;
        const float ledW = juce::jlimit (5.0f, 10.0f, m.getWidth() * 0.12f);
        const float pairW = ledW * 2.85f;
        auto row = [&] (int i) { return juce::Rectangle<float> (m.getX(), m.getY() + rowH * (float) i, m.getWidth(), rowH); };
        // SETUP > GENERAL > METERS sets the LED brightness.
        const float bright = uiprefs::brightness (ctx.settings, "meters", 40);
        auto barLed = [&] (int i, bool on, juce::Colour c)
        {
            auto r = row (i).withSizeKeepingCentre (pairW, juce::jmax (2.0f, rowH * 0.6f));
            g.setColour (on ? c.withMultipliedBrightness (bright) : surfaceColours().ledOff);
            g.fillRect (r);
        };
        barLed (0, shownDyn, Colours::ledDyn);
        barLed (kMeterRows - 1, shownGate, Colours::ledGate);

        auto columns = juce::Rectangle<float> (m.getCentreX() - pairW * 0.5f, m.getY() + rowH, pairW, rowH * kMeterSegments);
        const bool stereo = strip->getNumChannels() > 1;
        auto colourFor = [bright] (int i) { return leds::meterColour (kMeterThresholds[i]).withMultipliedBrightness (bright); };
        leds::drawColumn (g, columns.removeFromLeft (ledW), kMeterSegments, shownLit[0], colourFor);
        leds::drawColumn (g, columns.removeFromRight (ledW), kMeterSegments, stereo ? shownLit[1] : shownLit[0], colourFor);
    }

    if (showValue && active)
    {
        g.setColour (send ? Colours::solo : Colours::textDim);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (shownValue, valueArea, juce::Justification::centred);
    }
}

int FaderStrip::topRowsHeight (int w)
{
    return layout::stripTopRowsHeight (w);
}

void FaderStrip::setScribbleShown (bool shown)
{
    if (shown != scribbleShown)
    {
        scribbleShown = shown;
        resized();
        repaint();
    }
}

void FaderStrip::resized()
{
    auto b = getLocalBounds().reduced (0, 3);
    const int w = b.getWidth();
    showValue = ctx.settings.getBoolValue ("ui.faderValues", false);

    if (scribbleShown)
    {
        // The display window: the thin colour light bar over the LCD, which
        // runs edge to edge across the module (inset at the module's ends).
        using namespace layout;
        const int barH = scribbleBarHeight (w), barRow = scribbleBarRow (w);
        scribbleArea = b.removeFromTop (scribbleWindowHeight (w));
        barArea = juce::Rectangle<int> (juce::roundToInt ((float) w * 0.78f), barH).withCentre ({ w / 2, scribbleArea.getY() + barRow / 2 });
        lcdArea = scribbleArea.withTrimmedTop (barRow).withTrimmedBottom (5)
                      .withTrimmedLeft (moduleFirst ? 9 : 0).withTrimmedRight (moduleLast ? 9 : 0);
        b.removeFromTop (gapUnderScribble (w));
    }
    else
    {
        // No display window: SELECT at the top.
        scribbleArea = barArea = lcdArea = {};
    }

    // SELECT, SOLO and MUTE are small keys, about two thirds of the strip wide.
    b = b.reduced (2, 0);
    const int keyH = juce::jlimit (18, 28, juce::roundToInt ((float) w * 0.42f));
    const int keyW = juce::jmin (juce::roundToInt ((float) w * 0.68f), keyH * 3);
    auto key = [&] (SurfaceKey& k)
    {
        k.setBounds (b.removeFromTop (keyH).withSizeKeepingCentre (keyW, keyH));
    };
    key (selectKey);
    b.removeFromTop (6);
    key (soloKey);
    b.removeFromTop (9);
    // Short windows: the meter gives way to the fader (at least kMinFaderTravel
    // tall), and folds away when it would be too short to read.
    int meterH = juce::jlimit (56, 100, juce::roundToInt ((float) keyH * 3.3f));
    const int belowMeter = 9 + keyH + juce::jlimit (6, 16, w / 8) + (showValue ? 14 : 0);
    meterH = juce::jmin (meterH, b.getHeight() - belowMeter - kMinFaderTravel);
    // Without a scribble strip (the COMPACT's fader 13) the meter is all that
    // shows the level: it keeps its rows and the fader gives way (to 40 px).
    if (! scribbleShown)
        meterH = juce::jmax (meterH, juce::jmin (56, b.getHeight() - belowMeter - 40));
    if (meterH < 40)
        meterH = 0;
    meterArea = b.removeFromTop (meterH);
    if (meterH > 0)
        b.removeFromTop (9);
    key (muteKey);
    b.removeFromTop (juce::jlimit (6, 16, w / 8));

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
