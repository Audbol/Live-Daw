#include "ui/LookAndFeel.h"

#include "BinaryData.h"

namespace livedaw::ui
{

ConsoleLookAndFeel::ConsoleLookAndFeel()
{
    regular = juce::Typeface::createSystemTypefaceFor (BinaryData::BarlowCondensedMedium_ttf, BinaryData::BarlowCondensedMedium_ttfSize);
    bold = juce::Typeface::createSystemTypefaceFor (BinaryData::BarlowCondensedSemiBold_ttf, BinaryData::BarlowCondensedSemiBold_ttfSize);

    setColourScheme ({ Colours::background, Colours::panel, Colours::panelLight,
                       Colours::outline, Colours::text, Colours::panelLight,
                       Colours::text, Colours::amber, Colours::text });

    setColour (juce::ResizableWindow::backgroundColourId, Colours::background);
    setColour (juce::TextButton::buttonColourId, Colours::screenHeader);
    setColour (juce::TextButton::buttonOnColourId, Colours::yellow);
    setColour (juce::TextButton::textColourOffId, Colours::text);
    setColour (juce::TextButton::textColourOnId, juce::Colours::black);
    setColour (juce::ToggleButton::tickColourId, Colours::yellow);
    setColour (juce::ToggleButton::textColourId, Colours::text);
    setColour (juce::Slider::thumbColourId, Colours::text);
    setColour (juce::Slider::trackColourId, Colours::cyan);
    setColour (juce::Slider::backgroundColourId, Colours::screenDark);
    setColour (juce::Slider::rotarySliderFillColourId, Colours::lime);
    setColour (juce::Slider::rotarySliderOutlineColourId, Colours::screenDark);
    setColour (juce::Slider::textBoxTextColourId, Colours::text);
    setColour (juce::Slider::textBoxBackgroundColourId, Colours::screenDark);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Label::textColourId, Colours::text);
    setColour (juce::ComboBox::backgroundColourId, Colours::screenHeader);
    setColour (juce::ComboBox::textColourId, Colours::text);
    setColour (juce::ComboBox::outlineColourId, juce::Colours::transparentBlack);
    setColour (juce::ComboBox::arrowColourId, Colours::text);
    setColour (juce::PopupMenu::backgroundColourId, juce::Colour (0xff2a2a2a));
    setColour (juce::PopupMenu::textColourId, Colours::text);
    setColour (juce::PopupMenu::headerTextColourId, Colours::cyan);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, Colours::amber);
    setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::black);
    setColour (juce::TextEditor::backgroundColourId, Colours::screenDark);
    setColour (juce::TextEditor::outlineColourId, Colours::screenHeader);
    setColour (juce::TextEditor::focusedOutlineColourId, Colours::amber);
    setColour (juce::TextEditor::textColourId, Colours::text);
    setColour (juce::TextEditor::highlightColourId, Colours::amber.withAlpha (0.5f));
    setColour (juce::CaretComponent::caretColourId, Colours::yellow);
    setColour (juce::ListBox::backgroundColourId, Colours::screen);
    setColour (juce::TabbedComponent::backgroundColourId, Colours::screen);
    setColour (juce::TabbedComponent::outlineColourId, juce::Colours::transparentBlack);
    setColour (juce::ScrollBar::thumbColourId, Colours::screenTile);
    setColour (juce::ScrollBar::trackColourId, juce::Colours::transparentBlack);
    setColour (juce::AlertWindow::backgroundColourId, Colours::screenPanel);
    setColour (juce::AlertWindow::textColourId, Colours::text);
    setColour (juce::AlertWindow::outlineColourId, Colours::amber);
    setColour (juce::TooltipWindow::backgroundColourId, juce::Colour (0xff2a2a2a));
    setColour (juce::TooltipWindow::textColourId, Colours::text);
    setColour (juce::TooltipWindow::outlineColourId, Colours::screenTile);
    setColour (juce::DocumentWindow::textColourId, Colours::text);
}

juce::Typeface::Ptr ConsoleLookAndFeel::getTypefaceForFont (const juce::Font& f)
{
    const auto name = f.getTypefaceName();
    if (regular != nullptr && (name == juce::Font::getDefaultSansSerifFontName() || name == "<Sans-Serif>"))
        return f.isBold() ? bold : regular;
    return LookAndFeel_V4::getTypefaceForFont (f);
}

void ConsoleLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                                           float sliderPos, float minSliderPos, float maxSliderPos,
                                           juce::Slider::SliderStyle style, juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat();

    if (style == juce::Slider::LinearHorizontal)
    {
        // A thin track with a light blue fill and a white thumb, as on the WING's screen.
        const auto track = juce::Rectangle<float> (bounds.getX(), bounds.getCentreY() - 2.0f, bounds.getWidth(), 4.0f);
        g.setColour (Colours::screenDark);
        g.fillRect (track);
        g.setColour (slider.findColour (juce::Slider::trackColourId));
        g.fillRect (track.withRight (sliderPos));
        g.setColour (Colours::text);
        g.fillRect (juce::Rectangle<float> (6.0f, juce::jmin (18.0f, bounds.getHeight())).withCentre ({ sliderPos, bounds.getCentreY() }));
        return;
    }

    if (style != juce::Slider::LinearVertical)
    {
        LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
        return;
    }

    // Surface fader: dark slot, scale ticks, a long grey cap with a white line.
    const auto slot = juce::Rectangle<float> (bounds.getCentreX() - 2.5f, bounds.getY(), 5.0f, bounds.getHeight());
    g.setColour (juce::Colours::black);
    g.fillRoundedRectangle (slot, 2.5f);

    g.setColour (Colours::textDim.withAlpha (0.5f));
    for (double db : { 10.0, 5.0, 0.0, -5.0, -10.0, -20.0, -30.0, -40.0, -50.0, -60.0 })
    {
        const auto ty = static_cast<float> (slider.getPositionOfValue (db));
        const float w = db == 0.0 ? 9.0f : 5.0f;
        g.fillRect (slot.getX() - w - 2.0f, ty, w, 1.0f);
        g.fillRect (slot.getRight() + 2.0f, ty, w, 1.0f);
    }

    const float capH = juce::jlimit (18.0f, 34.0f, bounds.getHeight() * 0.09f);
    const auto cap = juce::Rectangle<float> (juce::jmin (bounds.getWidth() * 0.62f, 38.0f), capH).withCentre ({ bounds.getCentreX(), sliderPos });
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
    // The WING's knob: a thick ring (lime value arc on a dark track) around a black centre.
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (2.0f);
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const float thickness = juce::jmax (3.0f, radius * 0.3f);
    const float arcRadius = radius - thickness * 0.5f;
    const float angle = startAngle + pos * (endAngle - startAngle);
    const bool bipolar = slider.getProperties().getWithDefault ("bipolar", false);
    const float originAngle = bipolar ? (startAngle + endAngle) * 0.5f : startAngle;
    const juce::PathStrokeType stroke (thickness, juce::PathStrokeType::curved, juce::PathStrokeType::butt);

    juce::Path track;
    track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, startAngle, endAngle, true);
    g.setColour (juce::Colour (0xff262626));
    g.strokePath (track, stroke);

    juce::Path value;
    value.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, juce::jmin (originAngle, angle), juce::jmax (originAngle, angle), true);
    g.setColour (slider.isEnabled() ? slider.findColour (juce::Slider::rotarySliderFillColourId) : Colours::screenTile);
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
    auto r = juce::Rectangle<int> (width, height).toFloat();
    g.setColour (box.findColour (juce::ComboBox::backgroundColourId).withMultipliedAlpha (box.isEnabled() ? 1.0f : 0.5f));
    g.fillRoundedRectangle (r, 2.0f);

    const float s = juce::jmin (8.0f, (float) height * 0.3f);
    const auto c = juce::Point<float> ((float) width - s * 1.6f, (float) height * 0.5f);
    juce::Path chevron;
    chevron.startNewSubPath (c.x - s * 0.6f, c.y - s * 0.3f);
    chevron.lineTo (c.x, c.y + s * 0.35f);
    chevron.lineTo (c.x + s * 0.6f, c.y - s * 0.3f);
    g.setColour (box.findColour (juce::ComboBox::arrowColourId).withAlpha (box.isEnabled() ? 1.0f : 0.4f));
    g.strokePath (chevron, juce::PathStrokeType (2.0f));
}

void ConsoleLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (6, 1, box.getWidth() - 30, box.getHeight() - 2);
    label.setFont (getComboBoxFont (box));
}

void ConsoleLookAndFeel::drawTabButton (juce::TabBarButton& button, juce::Graphics& g, bool isMouseOver, bool)
{
    // WING tabs: grey keys; the current one darker with yellow text and a yellow top line.
    auto r = button.getActiveArea().toFloat().reduced (1.5f, 0.0f);
    const bool front = button.isFrontTab();
    g.setColour (front ? Colours::screenPanel : (isMouseOver ? Colours::screenHeader.brighter (0.08f) : Colours::screenHeader));
    g.fillRect (r);
    if (front)
    {
        g.setColour (Colours::yellow);
        g.fillRect (r.withHeight (3.0f));
    }
    g.setColour (front ? Colours::yellow : Colours::text);
    g.setFont (juce::FontOptions (juce::jlimit (12.0f, 20.0f, r.getHeight() * 0.58f), front ? juce::Font::bold : juce::Font::plain));
    g.drawFittedText (button.getButtonText().toUpperCase(), r.reduced (6.0f, 0.0f).toNearestInt(), juce::Justification::centred, 1);
}

int ConsoleLookAndFeel::getTabButtonBestWidth (juce::TabBarButton& button, int tabDepth)
{
    return juce::jmax (110, juce::roundToInt (juce::GlyphArrangement::getStringWidth (juce::Font (juce::FontOptions ((float) tabDepth * 0.58f)),
                                                                                    button.getButtonText()))
                                + 40);
}

juce::Font ConsoleLookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    return juce::Font (juce::FontOptions (juce::jlimit (11.0f, 18.0f, buttonHeight * 0.58f), juce::Font::bold));
}

juce::Font ConsoleLookAndFeel::getComboBoxFont (juce::ComboBox& box)
{
    return juce::Font (juce::FontOptions (juce::jlimit (11.0f, 18.0f, box.getHeight() * 0.62f)));
}

juce::Font ConsoleLookAndFeel::getLabelFont (juce::Label& label)
{
    return label.getFont();
}

juce::Font ConsoleLookAndFeel::getPopupMenuFont()
{
    return juce::Font (juce::FontOptions (17.0f));
}

void drawSectionBox (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title, bool chevron)
{
    auto r = area.reduced (3).toFloat();
    if (title.isNotEmpty())
    {
        auto header = r.removeFromTop ((float) kPanelHeaderHeight);
        g.setColour (Colours::screenHeader);
        g.fillRect (header);
        g.setColour (Colours::text);
        g.setFont (juce::FontOptions (19.0f, juce::Font::bold));
        g.drawFittedText (title, header.reduced (28.0f, 0.0f).toNearestInt(), juce::Justification::centred, 1);
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
        r.removeFromTop (kPanelHeaderHeight + 4);
    return r.reduced (8, 6);
}

void drawChip (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& text, juce::Colour fill, juce::Colour textColour)
{
    g.setColour (fill);
    g.fillRoundedRectangle (r, 2.0f);
    g.setColour (textColour);
    g.setFont (juce::FontOptions (juce::jlimit (9.0f, 18.0f, r.getHeight() * 0.86f), juce::Font::bold));
    g.drawFittedText (text, r.toNearestInt(), juce::Justification::centred, 1);
}

} // namespace livedaw::ui
