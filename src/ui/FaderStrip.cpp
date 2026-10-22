#include "ui/FaderStrip.h"
#include "dsp/DspUtils.h"
#include "engine/MidiRemote.h"
#include "engine/Models.h"

#include <optional>

namespace livedaw::ui
{

void drawLightBar (juce::Graphics& g, juce::Rectangle<float> bar, juce::Colour colour, bool on)
{
    // A thin light bar under a frosted window: off it's a dark line; lit it
    // glows in the strip's colour with a bright core.
    const float radius = bar.getHeight() * 0.5f;
    if (! on)
    {
        // Off: the frosted window, light grey.
        g.setColour (Colours::ledWindow);
        g.fillRoundedRectangle (bar, radius);
        return;
    }
    for (int i = 3; i >= 1; --i)
    {
        const float spread = bar.getHeight() * 0.45f * (float) i;
        g.setColour (colour.withAlpha (0.11f));
        g.fillRoundedRectangle (bar.expanded (spread, spread * 0.8f), radius + spread);
    }
    g.setColour (colour);
    g.fillRoundedRectangle (bar, radius);
    g.setColour (colour.interpolatedWith (juce::Colours::white, 0.55f));
    g.fillRoundedRectangle (bar.reduced (bar.getWidth() * 0.1f, bar.getHeight() * 0.32f), radius * 0.5f);
}

namespace
{
    /** Text squeezed into `area` on one line (narrowed to 55 %, then cut short). */
    void lcdText (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area, juce::Justification j)
    {
        g.drawFittedText (text, area.toNearestInt(), j, 1, 0.55f);
    }

    /** The LCD's type: the screen font with capitals `cap` px tall. */
    juce::Font lcdFont (float cap, WingWeight weight)
    {
        return wingFontPx (juce::jmax (4.0f, cap), weight);
    }

    /** The name's rows, as on the console: a name that fits stays on one row;
        a longer one breaks onto a new row at a space (up to `maxRows`), the
        last row taking the rest. */
    juce::StringArray nameRows (const juce::String& name, const juce::Font& font, float width, int maxRows)
    {
        juce::StringArray rows;
        if (maxRows <= 1 || juce::GlyphArrangement::getStringWidth (font, name) <= width || ! name.containsChar (' '))
        {
            rows.add (name);
            return rows;
        }
        auto words = juce::StringArray::fromTokens (name, " ", "");
        words.removeEmptyStrings();
        juce::String row;
        for (int i = 0; i < words.size(); ++i)
        {
            const auto candidate = row.isEmpty() ? words[i] : row + " " + words[i];
            if (row.isNotEmpty() && juce::GlyphArrangement::getStringWidth (font, candidate) > width)
            {
                rows.add (row);
                if (rows.size() == maxRows - 1)
                {
                    words.removeRange (0, i);
                    rows.add (words.joinIntoString (" "));
                    return rows;
                }
                row = words[i];
            }
            else
            {
                row = candidate;
            }
        }
        rows.add (row);
        return rows;
    }

    /** One row of the name: centred when it fits, else from the left and cut
        off at the right edge (typing goes on to the right, as on the console). */
    void nameRow (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area)
    {
        const bool fits = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), text) <= area.getWidth();
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (area.toNearestInt());
        g.drawText (text, area, fits ? juce::Justification::centred : juce::Justification::centredLeft, false);
    }
} // namespace

void drawScribble (juce::Graphics& g, juce::Rectangle<float> lcd, const ScribbleInfo& info)
{
    // A negative LCD: light text on the backlight, which is one colour on
    // every strip (only the light bar above shows the strip's colour).
    // SCRIBBLE sets the backlight (the text, lit by it, follows), SCR CONT
    // how far the text stands out from the background.
    const float backlight = juce::jlimit (0.2f, 1.6f, info.backlight);
    const auto bg = surfaceColours().scribbleLcd.withMultipliedBrightness (backlight);
    g.setColour (bg);
    g.fillRect (lcd);
    if (info.isEmpty())
        return;
    const float contrast = juce::jlimit (0.0f, 1.0f, 0.55f + 0.75f * (info.contrast - 0.4f));
    const auto ink = bg.interpolatedWith (surfaceColours().scribbleInk, 0.45f + 0.55f * contrast)
                         .withMultipliedBrightness (juce::jlimit (0.55f, 1.0f, 0.55f + 0.45f * backlight));

    const float H = lcd.getHeight(), W = lcd.getWidth();
    const float pad = juce::jlimit (2.0f, 6.0f, W * 0.04f);
    auto r = lcd;

    // Top row: the number, and the tag on an inverted box ("5" | "CRD5" for
    // inputs, "BUS" | "1" otherwise), under a rule across the strip.
    if (info.number.isNotEmpty() || info.tag.isNotEmpty() || info.sendTag.isNotEmpty())
    {
        auto row = r.removeFromTop (juce::jmax (9.0f, std::round (H * 0.2f)));
        const float cap = juce::jmax (5.0f, row.getHeight() * 0.56f);
        const auto font = lcdFont (cap, cap < 7.0f ? WingWeight::SemiBold : WingWeight::Medium); // bolder when tiny
        g.setFont (font);
        auto inner = row.reduced (0.0f, 1.0f);

        auto box = [&] (const juce::String& text, bool left, bool filled)
        {
            const float textW = juce::GlyphArrangement::getStringWidth (font, text);
            const float w = juce::jmin (textW + pad * 1.4f, inner.getWidth() * (left ? 0.62f : 0.72f));
            auto b = left ? inner.removeFromLeft (w) : inner.removeFromRight (w);
            if (filled)
            {
                g.setColour (ink);
                g.fillRect (b);
                g.setColour (bg);
            }
            else
            {
                // A send (SOF): an outlined box.
                g.setColour (ink);
                g.drawRect (b, 1.0f);
            }
            lcdText (g, text, b.reduced (pad * 0.5f, 0.0f), juce::Justification::centred);
        };

        // An SOF send sits at the right, in place of the source tag (inputs)
        // or the number.
        if (info.tag.isNotEmpty() && info.tagFirst)
            box (info.tag, true, true);
        if (info.sendTag.isNotEmpty())
            box (info.sendTag, false, false);
        else if (info.tag.isNotEmpty() && ! info.tagFirst)
            box (info.tag, false, true);

        if (info.sendTag.isEmpty() || ! info.tagFirst)
        {
            g.setColour (ink);
            lcdText (g, info.number, inner.reduced (pad, 0.0f),
                     info.tagFirst ? juce::Justification::centredRight : juce::Justification::centredLeft);
        }
        g.setColour (ink.withMultipliedAlpha (0.85f));
        g.fillRect (juce::Rectangle<float> (row.getX(), std::round (row.getBottom()) - 1.0f, row.getWidth(), 1.0f));
    }

    r = r.reduced (pad * 0.5f, 0.0f);

    // Bottom: the footer under its rule, then the name, centred (on an
    // inverted bar for a CC named "|...").
    if (info.footer.isNotEmpty())
    {
        auto foot = r.removeFromBottom (juce::jmax (8.0f, std::round (H * 0.18f)));
        g.setColour (ink.withMultipliedAlpha (0.85f));
        g.fillRect (juce::Rectangle<float> (lcd.getX(), std::round (foot.getY()), lcd.getWidth(), 1.0f));
        g.setColour (ink);
        g.setFont (lcdFont (foot.getHeight() * 0.56f, WingWeight::Medium));
        lcdText (g, info.footer, foot.withTrimmedTop (1.0f), juce::Justification::centred);
    }
    // With no icon (and nothing else) in the centre, the name takes the whole
    // area in larger type, as on the console.
    const bool hasIcon = (info.strip != nullptr && info.strip->getIcon() > 0) || info.icon.has_value();
    const bool centreFree = info.value.isEmpty() && ! hasIcon && info.line2.isEmpty() && info.line3.isEmpty();
    if (info.name.isNotEmpty() && centreFree && ! info.nameInverted)
    {
        // Large bold capitals, about six to the strip's width (a seventh is
        // cut off), two rows broken at a space as the console does.
        auto area = r.reduced (0.0f, juce::jmax (1.0f, r.getHeight() * 0.04f));
        const float probe = 20.0f;
        const float widthCap = probe * area.getWidth() / juce::GlyphArrangement::getStringWidth (lcdFont (probe, WingWeight::SemiBold), "CHANNE");
        const float rowCap = juce::jmin (widthCap, area.getHeight() / 2.0f / 1.3f);
        g.setFont (lcdFont (rowCap, WingWeight::SemiBold));
        const auto rows = nameRows (info.name, g.getCurrentFont(), area.getWidth(), 2);
        g.setColour (ink);
        auto block = area.withSizeKeepingCentre (area.getWidth(), rowCap * 1.3f * (float) rows.size());
        for (const auto& row : rows)
            nameRow (g, row, block.removeFromTop (rowCap * 1.3f));
        return;
    }
    if (info.name.isNotEmpty())
    {
        auto bar = r.removeFromBottom (juce::jmax (10.0f, std::round (H * 0.24f)));
        const float nameCap = juce::jmax (5.5f, bar.getHeight() * 0.5f);
        g.setFont (lcdFont (nameCap, nameCap < 7.0f ? WingWeight::SemiBold : WingWeight::Medium));
        if (info.nameInverted)
        {
            g.setColour (ink);
            g.fillRect (bar.reduced (0.0f, 1.0f));
            g.setColour (bg);
        }
        else
        {
            g.setColour (ink);
        }
        nameRow (g, info.name, bar.reduced (1.0f, 0.0f));
    }

    // Centre: a value in large type (SHOW FADER VALUE, a knob's value) with
    // its caption, else the icon, else up to two small lines.
    auto centre = r.reduced (0.0f, juce::jmax (1.0f, r.getHeight() * 0.06f));
    if (centre.getHeight() < 4.0f)
        return;
    g.setColour (ink);
    if (info.value.isNotEmpty())
    {
        // The value and its caption as one block, centred; sized by the
        // LCD, so every strip's value has the same size.
        const bool hasCaption = info.caption.isNotEmpty();
        const float captionCap = hasCaption ? juce::jmin (H * 0.1f, centre.getHeight() * 0.22f) : 0.0f;
        float valueCap = juce::jmin (H * 0.24f, (centre.getHeight() - captionCap * 1.7f) / 1.45f);
        // As large as fits the strip's width (a value with its unit is long).
        if (const float textW = juce::GlyphArrangement::getStringWidth (lcdFont (valueCap, WingWeight::Medium), info.value);
            textW > centre.getWidth() * 0.92f)
            valueCap *= centre.getWidth() * 0.92f / textW;
        auto block = centre.withSizeKeepingCentre (centre.getWidth(), valueCap * 1.45f + captionCap * 1.7f);
        g.setFont (lcdFont (valueCap, WingWeight::Medium));
        lcdText (g, info.value, block.removeFromTop (valueCap * 1.45f), juce::Justification::centred);
        if (hasCaption)
        {
            g.setFont (lcdFont (captionCap, WingWeight::Medium));
            lcdText (g, info.caption, block, juce::Justification::centred);
        }
        return;
    }
    const float s = juce::jmin (centre.getHeight() * 0.82f, centre.getWidth() * 0.5f);
    const auto iconArea = centre.withSizeKeepingCentre (s, s);
    if (const int wingIcon = info.strip != nullptr ? (int) info.strip->getIcon() : info.wingIcon; wingIcon > 0)
    {
        drawWingIcon (g, wingIcon, iconArea, ink);
        return;
    }
    if (info.icon.has_value())
    {
        drawIcon (g, *info.icon, iconArea.reduced (s * 0.08f), ink);
        return;
    }
    // A patched channel with neither name nor icon shows its source's glyph
    // (the XLR connector for a local input), as on the console.
    if (info.strip != nullptr && info.strip->isInput() && info.name.isEmpty() && info.line2.isEmpty() && info.line3.isEmpty()
        && stripHasSource (*info.strip))
    {
        drawIcon (g, sourceIcon (info.strip->getActiveInputLeft()), iconArea.reduced (s * 0.08f), ink);
        return;
    }
    if (info.line2.isNotEmpty() || info.line3.isNotEmpty())
    {
        const bool two = info.line2.isNotEmpty() && info.line3.isNotEmpty();
        const float lineH = juce::jmin (centre.getHeight() / (two ? 2.0f : 1.0f), H * 0.24f);
        auto lines = centre.withSizeKeepingCentre (centre.getWidth(), lineH * (two ? 2.0f : 1.0f));
        g.setFont (lcdFont (lineH * 0.56f, WingWeight::Medium));
        for (const auto* line : { &info.line2, &info.line3 })
            if (line->isNotEmpty())
                lcdText (g, *line, lines.removeFromTop (lineH), juce::Justification::centred);
    }
}

void drawScribbleFrame (juce::Graphics& g, juce::Rectangle<float> lcd, bool first, bool last, float backlight)
{
    // The rim (top, bottom and the display's ends), pixel aligned so it stays
    // a crisp line at every size, and the rule to the next strip.
    const auto rim = surfaceColours().scribbleRim.withMultipliedBrightness (juce::jlimit (0.4f, 1.3f, backlight));
    const auto r = lcd.toNearestInt().toFloat();
    g.setColour (rim);
    g.fillRect (r.withHeight (1.0f));
    g.fillRect (r.withTrimmedTop (r.getHeight() - 1.0f));
    if (first)
        g.fillRect (r.withWidth (1.0f));
    if (last)
        g.fillRect (r.withTrimmedLeft (r.getWidth() - 1.0f));
    else
    {
        g.setColour (surfaceColours().scribbleRule.withMultipliedBrightness (juce::jlimit (0.5f, 1.0f, 0.5f + 0.5f * backlight)).withAlpha (0.8f));
        g.fillRect (r.getRight() - 1.0f, r.getY() + 1.0f, 1.0f, r.getHeight() - 2.0f);
    }
}

juce::String scribbleName (const Strip& s)
{
    const auto name = s.getName().trim();
    return name.removeCharacters (" ").equalsIgnoreCase (s.getLabel().removeCharacters (" ")) ? juce::String() : name;
}

juce::String scribbleSourceTag (const Strip& s, bool showSource)
{
    const bool stereo = s.params.stereo.load();
    if (! showSource)
        return stereo ? "ST" : "MONO";
    const int left = s.getActiveInputLeft();
    if (left < 0)
        return "OFF";
    const auto l = inputPortName (left).removeCharacters (" ");
    if (! stereo)
        return l;
    const int right = s.getActiveInputRight() >= 0 ? s.getActiveInputRight() : left + 1;
    const auto r = inputPortName (right).removeCharacters (" ");
    // "IN7/8" when both ports are in the same group, else both names.
    const auto groupL = l.trimCharactersAtEnd ("0123456789"), groupR = r.trimCharactersAtEnd ("0123456789");
    return groupL == groupR ? l + "/" + r.substring (groupR.length()) : l + "/" + r;
}

void setScribbleStrip (ScribbleInfo& info, const Strip& s, bool showSource)
{
    if (s.isInput())
    {
        // The number ("A1" for aux channels), the source tag at the right.
        info.number = (s.getKind() == StripKind::Aux ? "A" : "") + juce::String (s.getNumber());
        info.tag = scribbleSourceTag (s, showSource);
        info.tagFirst = false;
    }
    else
    {
        info.tag = label (s.getKind());
        info.number = juce::String (s.getNumber());
        info.tagFirst = true;
    }
    info.name = scribbleName (s);
    info.strip = &s;
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
        if (strip != nullptr && ctx.settings.getBoolValue (uiprefs::autoSelect[juce::jlimit (0, 2, (int) section)], false)
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
    target = t;
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
    band (scribbleArea, 0, juce::Colours::black);
    band (meterArea, 4, juce::Colour (0xff0c0d0e));

    // Colour light bar (COLOR LED brightness) and scribble strip; one LCD
    // runs across the module, with a fine rule between the strips.
    const juce::Colour colour = strip != nullptr ? stripDisplayColour (*strip)
                                : (dca >= 0 ? wingColour (shownColour) : (fxp ? kFxColour : (midi ? Colours::midiCc : surfaceColours().ledOff)));
    drawLightBar (g, barArea.toFloat(), colour.withMultipliedBrightness (uiprefs::brightness (ctx.settings, "colorLed", 25)),
                  target.has_value() && shownLight);
    auto info = target ? scribbleInfo() : ScribbleInfo {};
    info.backlight = uiprefs::brightness (ctx.settings, "scribble", 60);
    drawScribble (g, lcdArea.toFloat(), info);
    drawScribbleFrame (g, lcdArea.toFloat(), moduleFirst, moduleLast, info.backlight);

    // Meter block: DYN LED, two 6-segment columns (input level), GATE LED.
    if (strip != nullptr)
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

    if (showValue && target)
    {
        g.setColour (send ? Colours::solo : Colours::textDim);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (shownValue, valueArea, juce::Justification::centred);
    }
}

void FaderStrip::resized()
{
    auto b = getLocalBounds().reduced (0, 3);
    const int w = b.getWidth();

    // The display window: the thin colour light bar over the LCD, which runs
    // edge to edge across the module (inset at the module's ends).
    // The LCD is wider than tall, as on the console (about 1.4 : 1).
    const int lcdH = juce::jlimit (38, 120, juce::roundToInt ((float) w * 0.72f));
    const int barH = juce::jlimit (3, 8, w / 12);
    // The black window has room above the LCD, the light bars in its middle.
    const int barRow = juce::jlimit (barH + 12, barH + 36, juce::roundToInt ((float) w * 0.4f));
    scribbleArea = b.removeFromTop (barRow + lcdH + 5);
    barArea = juce::Rectangle<int> (juce::roundToInt ((float) w * 0.78f), barH).withCentre ({ w / 2, scribbleArea.getY() + barRow / 2 });
    lcdArea = scribbleArea.withTrimmedTop (barRow).withTrimmedBottom (5)
                  .withTrimmedLeft (moduleFirst ? 9 : 0).withTrimmedRight (moduleLast ? 9 : 0);
    b.removeFromTop (juce::jlimit (8, 36, w * 3 / 10));

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
    meterArea = b.removeFromTop (juce::jlimit (56, 100, juce::roundToInt ((float) keyH * 3.3f)));
    b.removeFromTop (9);
    key (muteKey);
    b.removeFromTop (juce::jlimit (6, 16, w / 8));

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
