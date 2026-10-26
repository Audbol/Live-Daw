#include "ui/Scribble.h"

namespace livedaw::ui
{

juce::Colour lightBarColour (juce::Colour screenColour)
{
    // Measured off photos of a WING's light bars at the factory colour.
    const auto darkBlue = wingColour (1);
    const juce::Colour ledBlue (0xff1468ff);
    if (screenColour == darkBlue)
        return ledBlue;
    if (screenColour == darkBlue.withMultipliedBrightness (0.5f))
        return ledBlue.withMultipliedBrightness (0.5f);
    return screenColour;
}

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

std::pair<juce::Colour, juce::Colour> scribbleColours (float backlightSetting, float contrastSetting)
{
    // A negative LCD: light text on the backlight, which is one colour on
    // every strip (only the light bar above shows the strip's colour).
    // SCRIBBLE sets the backlight (the text, lit by it, follows), SCR CONT
    // how far the text stands out from the background.
    const float backlight = juce::jlimit (0.2f, 1.6f, backlightSetting);
    const auto bg = surfaceColours().scribbleLcd.withMultipliedBrightness (backlight);
    const float contrast = juce::jlimit (0.0f, 1.0f, 0.55f + 0.75f * (contrastSetting - 0.4f));
    const auto ink = bg.interpolatedWith (surfaceColours().scribbleInk, 0.45f + 0.55f * contrast)
                         .withMultipliedBrightness (juce::jlimit (0.55f, 1.0f, 0.55f + 0.45f * backlight));
    return { bg, ink };
}

void drawScribble (juce::Graphics& g, juce::Rectangle<float> lcd, const ScribbleInfo& info)
{
    const auto colours = scribbleColours (info.backlight, info.contrast);
    const auto bg = colours.first, ink = colours.second;
    g.setColour (bg);
    g.fillRect (lcd);
    if (info.isEmpty() || info.backlight <= 0.0f) // an unlit LCD shows nothing
        return;

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
    if (info.footer.isNotEmpty() || info.footerRow)
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
        return {}; // as on the WING: no tag at all (Sidecar showed "OFF")
    const auto l = inputPortName (left).removeCharacters (" ");
    if (! stereo)
        return l;
    const int right = s.getActiveInputRight() >= 0 ? s.getActiveInputRight() : left + 1;
    const auto r = inputPortName (right).removeCharacters (" ");
    // "LCL7/8" when both ports are in the same group, else both names.
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

} // namespace livedaw::ui
