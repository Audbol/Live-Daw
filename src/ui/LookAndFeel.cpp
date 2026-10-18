#include "ui/LookAndFeel.h"

#include "BinaryData.h"

namespace livedaw::ui
{

namespace
{
    constexpr const char* kScreenTypefaceName = "WingScreen";

    float screenHeightPx = 500.0f;
    float capRatio = 0.583f; // cap height / font height of the screen face (measured)

    float measureCapRatio (juce::Typeface::Ptr face)
    {
        if (face == nullptr)
            return capRatio;
        // The ink of a capital H (glyph boxes span the whole line height).
        juce::GlyphArrangement glyphs;
        glyphs.addLineOfText (juce::Font (juce::FontOptions (face).withHeight (100.0f)), "H", 0.0f, 0.0f);
        juce::Path outline;
        glyphs.createPath (outline);
        const auto box = outline.getBounds();
        return box.getHeight() > 10.0f ? box.getHeight() / 100.0f : capRatio;
    }
} // namespace

void setWingScreenHeight (float px) noexcept { screenHeightPx = juce::jmax (100.0f, px); }
float getWingScreenHeight() noexcept { return screenHeightPx; }

juce::Font wingFontPx (float capHeightPx, WingWeight weight)
{
    const float height = juce::jmax (4.0f, capHeightPx / capRatio);
    return juce::Font (juce::FontOptions (kScreenTypefaceName, height, weight == WingWeight::SemiBold ? juce::Font::bold : juce::Font::plain)
                           .withKerningFactor (0.025f));
}

juce::Font wingFont (float capFraction, WingWeight weight)
{
    return wingFontPx (capFraction * screenHeightPx, weight);
}

juce::Font silkFont (float height, bool bold)
{
    return juce::Font (juce::FontOptions (kSilkTypefaceName, height, bold ? juce::Font::bold : juce::Font::plain));
}

namespace
{
    /** The fader cap's height for a fader `sliderHeight` px tall. */
    float surfaceFaderCapHeight (float sliderHeight) { return juce::jlimit (22.0f, 56.0f, sliderHeight * 0.14f); }

    /** A fader of the WING's fader sections: a black slot between two printed
        tick ladders, two frames over the +5 .. -5 zone, the scale (10 .. -60,
        -oo) to the right, and the tall silver cap with its centre groove. */
    void drawSurfaceFader (juce::Graphics& g, juce::Rectangle<float> bounds, float sliderPos, juce::Slider& slider)
    {
        const float w = bounds.getWidth();
        const bool numerals = w > 34.0f;
        const float slotX = bounds.getX() + w * (numerals ? 0.42f : 0.5f);
        const float capW = juce::jmin (w * (numerals ? 0.5f : 0.62f), 44.0f);
        const float capH = surfaceFaderCapHeight (bounds.getHeight());
        const float top = (float) slider.getPositionOfValue (slider.getMaximum());
        const float bottom = (float) slider.getPositionOfValue (slider.getMinimum());
        auto yOf = [&slider] (double db) { return (float) slider.getPositionOfValue (db); };

        // Tick ladders either side of the slot.
        const float inner = juce::jmax (3.0f, w * 0.055f), outer = juce::jmin (capW * 0.5f, w * 0.25f);
        const float pitch = juce::jmax (3.0f, (bottom - top) / 64.0f);
        g.setColour (Colours::silkDim.withAlpha (0.55f));
        for (float y = top; y <= bottom + 0.5f; y += pitch)
        {
            g.fillRect (slotX - outer, y, outer - inner, 1.0f);
            g.fillRect (slotX + inner, y, outer - inner, 1.0f);
        }

        // The two frames over the +5 .. 0 .. -5 zone.
        const auto zone = [&] (double from, double to)
        {
            return juce::Rectangle<float> (slotX - outer - 1.0f, yOf (from) + 2.0f, (outer + 1.0f) * 2.0f, yOf (to) - yOf (from) - 4.0f);
        };
        for (auto frame : { zone (5.0, 0.0), zone (0.0, -5.0) })
        {
            if (frame.getHeight() < 6.0f)
                continue;
            g.setColour (Colours::panel);
            g.fillRoundedRectangle (frame, 3.0f);
            g.setColour (Colours::silkDim.withAlpha (0.8f));
            g.drawRoundedRectangle (frame.reduced (0.5f), 3.0f, 1.0f);
        }

        // The slot.
        const auto slot = juce::Rectangle<float> (slotX - 2.5f, top - capH * 0.25f, 5.0f, bottom - top + capH * 0.5f);
        g.setColour (juce::Colours::black);
        g.fillRoundedRectangle (slot, 2.5f);

        if (numerals)
        {
            g.setFont (silkFont (juce::jlimit (8.0f, 11.5f, w * 0.17f)));
            g.setColour (Colours::silk.withAlpha (0.85f));
            const float textX = slotX + outer + 2.0f;
            for (double db : { 10.0, 5.0, 0.0, -5.0, -10.0, -20.0, -30.0, -40.0, -50.0, -60.0, -100.0 })
            {
                const auto text = db <= -90.0 ? juce::String::fromUTF8 ("-\xe2\x88\x9e") : juce::String ((int) db);
                g.drawText (text, juce::Rectangle<float> (textX, yOf (db) - 6.0f, bounds.getRight() - textX, 12.0f), juce::Justification::centredRight);
            }
        }

        // The cap: a shadow, silver body in two halves, a dark groove with a
        // highlight; the groove takes the fader's colour when it has one (sends, FX).
        auto cap = juce::Rectangle<float> (capW, capH).withCentre ({ slotX, sliderPos });
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillRoundedRectangle (cap.translated (2.0f, 3.0f).expanded (1.0f), 3.0f);
        const bool enabled = slider.isEnabled();
        const auto light = juce::Colour (enabled ? 0xffd9dbdf : 0xff9a9ca0), dark = juce::Colour (enabled ? 0xff7d8086 : 0xff5c5e62);
        auto upper = cap.withHeight (capH * 0.5f), lower = cap.withTrimmedTop (capH * 0.5f);
        g.setGradientFill (juce::ColourGradient (light, 0.0f, upper.getY(), dark, 0.0f, upper.getBottom(), false));
        g.fillRoundedRectangle (upper, 2.5f);
        g.setGradientFill (juce::ColourGradient (light.darker (0.08f), 0.0f, lower.getY(), dark.darker (0.15f), 0.0f, lower.getBottom(), false));
        g.fillRoundedRectangle (lower, 2.5f);
        // Darker side edges.
        g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.28f), cap.getX(), 0.0f,
                                                 juce::Colours::transparentBlack, cap.getX() + capW * 0.22f, 0.0f, false));
        g.fillRect (cap.withWidth (capW * 0.22f));
        g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.28f), cap.getRight(), 0.0f,
                                                 juce::Colours::transparentBlack, cap.getRight() - capW * 0.22f, 0.0f, false));
        g.fillRect (cap.withLeft (cap.getRight() - capW * 0.22f));
        g.setColour (juce::Colours::black.withAlpha (0.7f));
        g.drawRoundedRectangle (cap, 2.5f, 1.0f);
        const auto line = slider.findColour (juce::Slider::thumbColourId);
        const bool coloured = line != Colours::text;
        g.setColour (coloured ? line : juce::Colour (0xff2a2c30));
        g.fillRect (cap.getX() + 1.0f, cap.getCentreY() - (coloured ? 1.25f : 0.75f), capW - 2.0f, coloured ? 2.5f : 1.5f);
        g.setColour (juce::Colours::white.withAlpha (0.35f));
        g.fillRect (cap.getX() + 1.0f, cap.getCentreY() + 1.0f, capW - 2.0f, 1.0f);
    }
} // namespace

ConsoleLookAndFeel::ConsoleLookAndFeel()
{
    screenMedium = juce::Typeface::createSystemTypefaceFor (BinaryData::BarlowSemiCondensedMedium_ttf, BinaryData::BarlowSemiCondensedMedium_ttfSize);
    screenSemiBold = juce::Typeface::createSystemTypefaceFor (BinaryData::BarlowSemiCondensedSemiBold_ttf, BinaryData::BarlowSemiCondensedSemiBold_ttfSize);
    silkMedium = juce::Typeface::createSystemTypefaceFor (BinaryData::BarlowCondensedMedium_ttf, BinaryData::BarlowCondensedMedium_ttfSize);
    silkSemiBold = juce::Typeface::createSystemTypefaceFor (BinaryData::BarlowCondensedSemiBold_ttf, BinaryData::BarlowCondensedSemiBold_ttfSize);
    capRatio = measureCapRatio (screenMedium);

    setColourScheme ({ Colours::background, Colours::panel, Colours::panelLight,
                       Colours::outline, Colours::text, Colours::panelLight,
                       Colours::text, Colours::gold, Colours::text });

    setColour (juce::ResizableWindow::backgroundColourId, Colours::background);
    setColour (juce::TextButton::buttonColourId, Colours::screenKey);
    setColour (juce::TextButton::buttonOnColourId, Colours::gold);
    setColour (juce::TextButton::textColourOffId, Colours::text);
    setColour (juce::TextButton::textColourOnId, juce::Colours::black);
    setColour (juce::ToggleButton::tickColourId, Colours::gold);
    setColour (juce::ToggleButton::textColourId, Colours::text);
    setColour (juce::Slider::thumbColourId, Colours::text);
    setColour (juce::Slider::trackColourId, Colours::valueBlue);
    setColour (juce::Slider::backgroundColourId, Colours::screenWell);
    setColour (juce::Slider::rotarySliderFillColourId, Colours::width);
    setColour (juce::Slider::rotarySliderOutlineColourId, Colours::screenLine);
    setColour (juce::Slider::textBoxTextColourId, Colours::text);
    setColour (juce::Slider::textBoxBackgroundColourId, Colours::screenWell);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Label::textColourId, Colours::text);
    setColour (juce::ComboBox::backgroundColourId, Colours::screenWell);
    setColour (juce::ComboBox::textColourId, Colours::text);
    setColour (juce::ComboBox::outlineColourId, juce::Colours::transparentBlack);
    setColour (juce::ComboBox::arrowColourId, Colours::text);
    setColour (juce::ComboBox::buttonColourId, Colours::screenKey);
    setColour (juce::PopupMenu::backgroundColourId, Colours::screenWell);
    setColour (juce::PopupMenu::textColourId, Colours::text);
    setColour (juce::PopupMenu::headerTextColourId, Colours::caption);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, Colours::screenKey);
    setColour (juce::PopupMenu::highlightedTextColourId, Colours::text);
    setColour (juce::TextEditor::backgroundColourId, juce::Colours::black);
    setColour (juce::TextEditor::outlineColourId, Colours::screenLine);
    setColour (juce::TextEditor::focusedOutlineColourId, Colours::gold);
    setColour (juce::TextEditor::textColourId, Colours::text);
    setColour (juce::TextEditor::highlightColourId, Colours::gold.withAlpha (0.45f));
    setColour (juce::CaretComponent::caretColourId, Colours::gold);
    setColour (juce::ListBox::backgroundColourId, Colours::screen);
    setColour (juce::TabbedComponent::backgroundColourId, Colours::screen);
    setColour (juce::TabbedComponent::outlineColourId, juce::Colours::transparentBlack);
    setColour (juce::ScrollBar::thumbColourId, Colours::textDim);
    setColour (juce::ScrollBar::trackColourId, juce::Colours::transparentBlack);
    setColour (juce::AlertWindow::backgroundColourId, Colours::screenPanel);
    setColour (juce::AlertWindow::textColourId, Colours::text);
    setColour (juce::AlertWindow::outlineColourId, Colours::screenLine);
    setColour (juce::TooltipWindow::backgroundColourId, Colours::screenPanel);
    setColour (juce::TooltipWindow::textColourId, Colours::text);
    setColour (juce::TooltipWindow::outlineColourId, Colours::screenLine);
    setColour (juce::DocumentWindow::textColourId, Colours::text);
}

juce::Typeface::Ptr ConsoleLookAndFeel::getTypefaceForFont (const juce::Font& f)
{
    const auto name = f.getTypefaceName();
    if (name == kSilkTypefaceName && silkMedium != nullptr)
        return f.isBold() ? silkSemiBold : silkMedium;
    if (screenMedium != nullptr
        && (name == kScreenTypefaceName || name == juce::Font::getDefaultSansSerifFontName() || name == "<Sans-Serif>"))
        return f.isBold() ? screenSemiBold : screenMedium;
    return LookAndFeel_V4::getTypefaceForFont (f);
}

int ConsoleLookAndFeel::getSliderThumbRadius (juce::Slider& slider)
{
    // The slider indents its travel by this much, so the cap at either end stays inside.
    if (slider.getProperties().getWithDefault ("surfaceFader", false))
        return juce::roundToInt (surfaceFaderCapHeight ((float) slider.getHeight()) * 0.5f) + 2;
    return LookAndFeel_V4::getSliderThumbRadius (slider);
}

void ConsoleLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                                           float sliderPos, float minSliderPos, float maxSliderPos,
                                           juce::Slider::SliderStyle style, juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat();

    if (style == juce::Slider::LinearHorizontal)
    {
        // The WING's screen slider: a thin grey line, the value filled blue from
        // the left, and a small rounded white thumb.
        const auto track = juce::Rectangle<float> (bounds.getX(), bounds.getCentreY() - 1.0f, bounds.getWidth(), 2.0f);
        g.setColour (Colours::screenLine);
        g.fillRect (track);
        g.setColour (slider.findColour (juce::Slider::trackColourId));
        g.fillRect (track.expanded (0.0f, 1.0f).withRight (sliderPos));
        g.setColour (Colours::text);
        g.fillRoundedRectangle (juce::Rectangle<float> (6.0f, juce::jmin (12.0f, bounds.getHeight())).withCentre ({ sliderPos, bounds.getCentreY() }), 1.5f);
        return;
    }

    if (style != juce::Slider::LinearVertical)
    {
        LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
        return;
    }

    if (slider.getProperties().getWithDefault ("surfaceFader", false))
    {
        drawSurfaceFader (g, bounds, sliderPos, slider);
        return;
    }

    // Surface fader: dark slot with the printed scale (10 .. -60, -oo) to its
    // right when there's room, and a long grey cap with a white line.
    const bool numerals = width > 34;
    const float slotX = numerals ? bounds.getX() + bounds.getWidth() * 0.4f : bounds.getCentreX();
    const auto slot = juce::Rectangle<float> (slotX - 2.5f, bounds.getY(), 5.0f, bounds.getHeight());
    g.setColour (juce::Colours::black);
    g.fillRoundedRectangle (slot, 2.5f);

    const float capW = numerals ? juce::jmin (bounds.getWidth() * 0.5f, 34.0f) : juce::jmin (bounds.getWidth() * 0.62f, 38.0f);
    const float textX = slotX + capW * 0.5f + 2.0f;
    g.setFont (silkFont (juce::jlimit (8.0f, 11.0f, bounds.getWidth() * 0.16f)));
    for (double db : { 10.0, 5.0, 0.0, -5.0, -10.0, -20.0, -30.0, -40.0, -50.0, -60.0, -100.0 })
    {
        const auto ty = static_cast<float> (slider.getPositionOfValue (db));
        const float w = db == 0.0 ? 9.0f : 5.0f;
        g.setColour (Colours::silkDim.withAlpha (0.5f));
        if (! numerals)
            g.fillRect (slot.getX() - w - 2.0f, ty, w, 1.0f);
        g.fillRect (slot.getRight() + 2.0f, ty, w, 1.0f);
        if (numerals)
        {
            const auto text = db <= -90.0 ? juce::String::fromUTF8 ("\xe2\x88\x9e") : juce::String ((int) db);
            g.setColour (Colours::silkDim);
            g.drawText (text, juce::Rectangle<float> (textX, ty - 6.0f, bounds.getRight() - textX, 12.0f), juce::Justification::centredRight);
        }
    }

    const float capH = juce::jlimit (18.0f, 34.0f, bounds.getHeight() * 0.09f);
    const auto cap = juce::Rectangle<float> (capW, capH).withCentre ({ slotX, sliderPos });
    juce::ColourGradient grad (juce::Colour (0xff5d6168), cap.getX(), cap.getY(), juce::Colour (0xff2c2f34), cap.getX(), cap.getBottom(), false);
    g.setGradientFill (grad);
    g.fillRoundedRectangle (cap, 2.5f);
    g.setColour (juce::Colours::black.withAlpha (0.8f));
    g.drawRoundedRectangle (cap, 2.5f, 1.0f);
    g.setColour (slider.findColour (juce::Slider::thumbColourId));
    g.fillRect (cap.getX() + 2.0f, cap.getCentreY() - 0.75f, cap.getWidth() - 4.0f, 1.5f);
}

void ConsoleLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                           float pos, float startAngle, float endAngle, juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (2.0f);
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();

    if (slider.getProperties().getWithDefault ("hardware", false))
    {
        // A surface knob: a black cap on a skirt with a soft shadow. Encoders
        // are plain (endless, their value is on a screen); pots have a white
        // pointer line; the scroll wheel is a dished knob with a finger dimple.
        const int cap = slider.getProperties().getWithDefault ("cap", 0);
        const float angle = startAngle + pos * (endAngle - startAngle);
        const auto body = juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (centre);
        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.fillEllipse (body.translated (radius * 0.06f, radius * 0.12f).expanded (radius * 0.03f));
        g.setColour (juce::Colour (0xff0e0f10));
        g.fillEllipse (body);

        if (cap == 2)
        {
            // Wheel: a rim, the dished face and the dimple (which turns with it).
            g.setColour (juce::Colour (0xff2c2e32));
            g.drawEllipse (body.reduced (1.0f), juce::jmax (1.0f, radius * 0.025f));
            const auto face = body.reduced (radius * 0.07f);
            juce::ColourGradient dish (juce::Colour (0xff111214), centre.x - radius * 0.2f, centre.y - radius * 0.25f,
                                       juce::Colour (0xff232528), centre.x, face.getBottom(), true);
            g.setGradientFill (dish);
            g.fillEllipse (face);
            g.setColour (juce::Colours::black.withAlpha (0.6f));
            g.drawEllipse (face, 1.2f);
            const float d = radius * 0.28f;
            const auto dimple = juce::Rectangle<float> (d, d).withCentre (centre.getPointOnCircumference (radius * 0.6f, angle + juce::MathConstants<float>::pi));
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xff4a4c50), dimple.getX() + d * 0.3f, dimple.getY() + d * 0.25f,
                                                     juce::Colour (0xff0b0b0c), dimple.getRight(), dimple.getBottom(), true));
            g.fillEllipse (dimple);
            return;
        }

        const auto top = body.reduced (radius * 0.12f);
        juce::ColourGradient dome (juce::Colour (0xff3a3d42), top.getX() + top.getWidth() * 0.35f, top.getY() + top.getHeight() * 0.3f,
                                   juce::Colour (0xff131416), top.getRight(), top.getBottom(), true);
        g.setGradientFill (dome);
        g.fillEllipse (top);
        g.setColour (juce::Colours::white.withAlpha (0.05f));
        g.drawEllipse (top.reduced (0.5f), 1.0f);
        if (cap == 1)
        {
            g.setColour (slider.isEnabled() ? Colours::text : Colours::silkDim.withAlpha (0.4f));
            g.drawLine (juce::Line<float> (centre.getPointOnCircumference (radius * 0.45f, angle), centre.getPointOnCircumference (radius * 0.86f, angle)),
                        juce::jmax (1.5f, radius * 0.09f));
        }
        return;
    }

    // The WING's knob: a thick ring (the value arc, green by default, on a grey
    // track) around a black centre.
    const float thickness = juce::jmax (3.0f, radius * 0.3f);
    const float arcRadius = radius - thickness * 0.5f;
    const float angle = startAngle + pos * (endAngle - startAngle);
    const bool bipolar = slider.getProperties().getWithDefault ("bipolar", false);
    const float originAngle = bipolar ? (startAngle + endAngle) * 0.5f : startAngle;
    const juce::PathStrokeType stroke (thickness, juce::PathStrokeType::curved, juce::PathStrokeType::butt);

    juce::Path track;
    track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, startAngle, endAngle, true);
    g.setColour (slider.findColour (juce::Slider::rotarySliderOutlineColourId));
    g.strokePath (track, stroke);

    juce::Path value;
    value.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, juce::jmin (originAngle, angle), juce::jmax (originAngle, angle), true);
    g.setColour (slider.isEnabled() ? slider.findColour (juce::Slider::rotarySliderFillColourId) : Colours::chipOff);
    g.strokePath (value, stroke);

    g.setColour (juce::Colours::black);
    g.fillEllipse (juce::Rectangle<float> ((radius - thickness) * 2.0f, (radius - thickness) * 2.0f).withCentre (centre));

    juce::Path pointer;
    pointer.addRectangle (-1.25f, -(radius - thickness * 0.2f), 2.5f, thickness * 1.2f);
    g.setColour (Colours::text);
    g.fillPath (pointer, juce::AffineTransform::rotation (angle).translated (centre));
}

void ConsoleLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour& background,
                                               bool isMouseOver, bool isDown)
{
    auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
    auto colour = background;
    if (! button.isEnabled())
        colour = colour.withMultipliedAlpha (0.45f);
    else if (isDown)
        colour = colour.brighter (0.25f);
    else if (isMouseOver)
        colour = colour.brighter (0.1f);

    g.setColour (colour);
    g.fillRoundedRectangle (bounds, 2.0f);
}

void ConsoleLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box)
{
    // A WING dropdown field: a dark well with the value, and a separate key
    // with a down arrow at its right.
    const float alpha = box.isEnabled() ? 1.0f : 0.45f;
    auto r = juce::Rectangle<int> (width, height).toFloat();
    auto key = r.removeFromRight (juce::jmin (r.getHeight(), r.getWidth() * 0.3f));
    r.removeFromRight (2.0f);
    g.setColour (box.findColour (juce::ComboBox::backgroundColourId).withMultipliedAlpha (alpha));
    g.fillRect (r);
    g.setColour (box.findColour (juce::ComboBox::buttonColourId).withMultipliedAlpha (alpha));
    g.fillRect (key);

    const float s = juce::jmin (7.0f, key.getHeight() * 0.22f);
    const auto c = key.getCentre();
    juce::Path tri;
    tri.addTriangle (c.x - s, c.y - s * 0.55f, c.x + s, c.y - s * 0.55f, c.x, c.y + s * 0.65f);
    g.setColour (box.findColour (juce::ComboBox::arrowColourId).withMultipliedAlpha (alpha));
    g.fillPath (tri);
}

void ConsoleLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    const int key = juce::jmin (box.getHeight(), box.getWidth() * 3 / 10);
    label.setBounds (6, 1, box.getWidth() - key - 10, box.getHeight() - 2);
    label.setFont (getComboBoxFont (box));
}

void ConsoleLookAndFeel::drawTabButton (juce::TabBarButton& button, juce::Graphics& g, bool isMouseOver, bool)
{
    // WING tabs: #202020 with grey text; the current one #303030 with gold text
    // and a gold line along its top.
    auto r = button.getActiveArea().toFloat().reduced (1.0f, 0.0f);
    const bool front = button.isFrontTab();
    g.setColour (front ? Colours::screenTile : (isMouseOver ? Colours::screenHeader.brighter (0.06f) : Colours::screenHeader));
    g.fillRect (r);
    if (front)
    {
        g.setColour (Colours::gold);
        g.fillRect (r.withHeight (juce::jmax (2.0f, r.getHeight() * 0.07f)));
    }
    g.setColour (front ? Colours::gold : Colours::textDim);
    g.setFont (wingFontPx (juce::jlimit (8.0f, 14.0f, r.getHeight() * (front ? 0.36f : 0.31f))));
    g.drawFittedText (button.getButtonText().toUpperCase(), r.reduced (6.0f, 0.0f).toNearestInt(), juce::Justification::centred, 1);
}

int ConsoleLookAndFeel::getTabButtonBestWidth (juce::TabBarButton& button, int tabDepth)
{
    return juce::jmax (110, juce::roundToInt (juce::GlyphArrangement::getStringWidth (wingFontPx ((float) tabDepth * 0.36f),
                                                                                    button.getButtonText().toUpperCase()))
                                + 40);
}

juce::Font ConsoleLookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    return wingFontPx (juce::jlimit (7.0f, 11.0f, (float) buttonHeight * 0.32f));
}

juce::Font ConsoleLookAndFeel::getComboBoxFont (juce::ComboBox& box)
{
    return wingFontPx (juce::jlimit (7.0f, 12.0f, (float) box.getHeight() * 0.36f));
}

juce::Font ConsoleLookAndFeel::getLabelFont (juce::Label& label)
{
    return label.getFont();
}

juce::Font ConsoleLookAndFeel::getPopupMenuFont()
{
    return wingFont (TypeScale::body * 1.1f);
}

void ConsoleLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int width, int height)
{
    // A WING list: #101010 rows on black, in a grey frame.
    g.fillAll (juce::Colours::black);
    g.setColour (Colours::textDim);
    g.drawRect (0, 0, width, height, 1);
}

void ConsoleLookAndFeel::drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator, bool isActive,
                                            bool isHighlighted, bool isTicked, bool hasSubMenu, const juce::String& text,
                                            const juce::String& shortcutKeyText, const juce::Drawable* icon,
                                            const juce::Colour* textColour)
{
    if (isSeparator)
    {
        g.setColour (Colours::screenLine);
        g.fillRect (area.withSizeKeepingCentre (area.getWidth() - 8, 1));
        return;
    }

    // Rows are separated by a black line; the current choice is filled gold
    // with black text (no tick), a hovered row is lighter.
    auto r = area.withTrimmedBottom (2);
    const bool current = isTicked && isActive;
    g.setColour (current ? Colours::gold : (isHighlighted && isActive ? Colours::screenKey : Colours::screenWell));
    g.fillRect (r);

    auto ink = current ? juce::Colours::black : (textColour != nullptr ? *textColour : Colours::text);
    if (! isActive)
        ink = Colours::chipOff;
    r = r.reduced (10, 0);
    if (icon != nullptr)
    {
        auto iconArea = r.removeFromLeft (r.getHeight()).reduced (4).toFloat();
        icon->drawWithin (g, iconArea, juce::RectanglePlacement::centred | juce::RectanglePlacement::onlyReduceInSize, 1.0f);
        r.removeFromLeft (6);
    }
    if (hasSubMenu)
    {
        const auto arrow = r.removeFromRight (r.getHeight() / 2).toFloat();
        const float s = juce::jmin (6.0f, arrow.getHeight() * 0.2f);
        const auto c = arrow.getCentre();
        juce::Path p;
        p.startNewSubPath (c.x - s * 0.5f, c.y - s);
        p.lineTo (c.x + s * 0.5f, c.y);
        p.lineTo (c.x - s * 0.5f, c.y + s);
        g.setColour (ink);
        g.strokePath (p, juce::PathStrokeType (1.6f));
    }
    g.setFont (getPopupMenuFont());
    if (shortcutKeyText.isNotEmpty())
    {
        g.setColour (current ? ink : Colours::textDim);
        g.drawText (shortcutKeyText, r, juce::Justification::centredRight, true);
    }
    g.setColour (ink);
    g.drawFittedText (text, r, juce::Justification::centredLeft, 1, 0.8f);
}

void ConsoleLookAndFeel::drawPopupMenuSectionHeader (juce::Graphics& g, const juce::Rectangle<int>& area, const juce::String& sectionName)
{
    g.setColour (Colours::screenHeader);
    g.fillRect (area.withTrimmedBottom (2));
    g.setColour (Colours::caption);
    g.setFont (wingFont (TypeScale::caption));
    g.drawFittedText (sectionName.toUpperCase(), area.reduced (10, 0), juce::Justification::centredLeft, 1);
}

void ConsoleLookAndFeel::getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator, int standardMenuItemHeight,
                                                    int& idealWidth, int& idealHeight)
{
    if (isSeparator)
    {
        idealWidth = 50;
        idealHeight = 7;
        return;
    }
    const auto font = getPopupMenuFont();
    // WING list rows are tall: 60 px of 800.
    idealHeight = standardMenuItemHeight > 0 ? standardMenuItemHeight
                                             : juce::roundToInt (juce::jmax (font.getHeight() * 1.9f, getWingScreenHeight() * 0.06f));
    idealWidth = juce::roundToInt (juce::GlyphArrangement::getStringWidth (font, text)) + idealHeight * 2;
}

void drawSectionBox (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title, bool chevron)
{
    auto r = area.reduced (3).toFloat();
    if (title.isNotEmpty())
    {
        auto header = r.removeFromTop ((float) panelHeaderHeight());
        g.setColour (Colours::screenHeader);
        g.fillRect (header);
        g.setColour (Colours::caption);
        g.setFont (wingFontPx (juce::jmin (TypeScale::header * getWingScreenHeight(), header.getHeight() * 0.42f)));
        // Keep the title centred, clear of the chevron only when there is one.
        // Narrow headers (FILTER in a small window) give the title more of the chevron's margin.
        g.drawFittedText (title, header.reduced (chevron ? juce::jmin (28.0f, header.getWidth() * 0.2f) : 4.0f, 0.0f).toNearestInt(),
                          juce::Justification::centred, 1);
        if (chevron)
        {
            const auto c = juce::Point<float> (header.getRight() - 14.0f, header.getCentreY());
            juce::Path p;
            p.startNewSubPath (c.x - 4.0f, c.y - 8.0f);
            p.lineTo (c.x + 4.0f, c.y);
            p.lineTo (c.x - 4.0f, c.y + 8.0f);
            g.strokePath (p, juce::PathStrokeType (2.5f));
        }
        r.removeFromTop (4.0f);
    }
    g.setColour (Colours::screenPanel);
    g.fillRect (r);
}

juce::Rectangle<int> sectionContent (juce::Rectangle<int> area, bool hasTitle)
{
    auto r = area.reduced (3);
    if (hasTitle)
        r.removeFromTop (panelHeaderHeight() + 4);
    return r.reduced (8, 6);
}

void drawChip (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& text, juce::Colour fill, juce::Colour textColour)
{
    g.setColour (fill);
    g.fillRoundedRectangle (r, 2.0f);
    g.setColour (textColour);
    g.setFont (wingFontPx (juce::jlimit (5.0f, 12.0f, r.getHeight() * 0.56f)));
    g.drawFittedText (text, r.toNearestInt(), juce::Justification::centred, 1, 0.75f);
}

void drawSilk (juce::Graphics& g, juce::Rectangle<float> area, const juce::String& text, float fontSize,
               juce::Justification justification, juce::Colour colour)
{
    const auto lines = juce::StringArray::fromLines (text);
    g.setColour (colour);
    g.setFont (silkFont (fontSize));
    g.drawFittedText (text, area.toNearestInt(), justification, lines.size(), 0.8f);
}

} // namespace livedaw::ui
