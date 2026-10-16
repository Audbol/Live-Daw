#include "ui/LookAndFeel.h"

namespace livedaw::ui
{

ConsoleLookAndFeel::ConsoleLookAndFeel()
{
    setColourScheme ({ Colours::background, Colours::panel, Colours::panelLight,
                       Colours::outline, Colours::text, Colours::panelLight,
                       Colours::text, Colours::accent, Colours::text });

    setColour (juce::ResizableWindow::backgroundColourId, Colours::background);
    setColour (juce::TextButton::buttonColourId, Colours::panelLight);
    setColour (juce::TextButton::buttonOnColourId, Colours::accent);
    setColour (juce::TextButton::textColourOffId, Colours::text);
    setColour (juce::TextButton::textColourOnId, juce::Colours::black);
    setColour (juce::ToggleButton::tickColourId, Colours::accent);
    setColour (juce::Slider::thumbColourId, Colours::text);
    setColour (juce::Slider::trackColourId, Colours::accent);
    setColour (juce::Slider::backgroundColourId, Colours::outline);
    setColour (juce::Slider::rotarySliderFillColourId, Colours::accent);
    setColour (juce::Slider::rotarySliderOutlineColourId, Colours::outline);
    setColour (juce::Slider::textBoxTextColourId, Colours::text);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Label::textColourId, Colours::text);
    setColour (juce::ComboBox::backgroundColourId, Colours::panelLight);
    setColour (juce::ComboBox::outlineColourId, Colours::outline);
    setColour (juce::ComboBox::arrowColourId, Colours::accent);
    setColour (juce::PopupMenu::backgroundColourId, Colours::panel);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, Colours::accent.withAlpha (0.4f));
    setColour (juce::TextEditor::backgroundColourId, juce::Colours::black.withAlpha (0.3f));
    setColour (juce::TextEditor::outlineColourId, Colours::outline);
    setColour (juce::TextEditor::focusedOutlineColourId, Colours::accent);
    setColour (juce::TextEditor::textColourId, Colours::text);
    setColour (juce::ListBox::backgroundColourId, juce::Colours::black.withAlpha (0.3f));
    setColour (juce::TabbedButtonBar::tabTextColourId, Colours::textDim);
    setColour (juce::TabbedButtonBar::frontTextColourId, Colours::accent);
    setColour (juce::TabbedComponent::outlineColourId, juce::Colours::transparentBlack);
    setColour (juce::ScrollBar::thumbColourId, Colours::outline);
}

void ConsoleLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                                           float sliderPos, float minSliderPos, float maxSliderPos,
                                           juce::Slider::SliderStyle style, juce::Slider& slider)
{
    if (style != juce::Slider::LinearVertical)
    {
        LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
        return;
    }

    // WING-style fader: dark slot, scale ticks, a long grey cap with a white line.
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat();
    const auto slot = juce::Rectangle<float> (bounds.getCentreX() - 2.5f, bounds.getY(), 5.0f, bounds.getHeight());
    g.setColour (juce::Colours::black);
    g.fillRoundedRectangle (slot, 2.5f);

    g.setColour (Colours::textDim.withAlpha (0.6f));
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
    g.setColour (juce::Colours::white);
    g.fillRect (cap.getX() + 2.0f, cap.getCentreY() - 0.75f, cap.getWidth() - 4.0f, 1.5f);
}

void ConsoleLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                           float pos, float startAngle, float endAngle, juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (2.0f);
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const float angle = startAngle + pos * (endAngle - startAngle);
    const bool bipolar = slider.getProperties().getWithDefault ("bipolar", slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0);
    const float originAngle = bipolar ? (startAngle + endAngle) * 0.5f : startAngle;

    juce::Path track;
    track.addCentredArc (centre.x, centre.y, radius - 2.0f, radius - 2.0f, 0.0f, startAngle, endAngle, true);
    g.setColour (Colours::outline);
    g.strokePath (track, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    juce::Path value;
    value.addCentredArc (centre.x, centre.y, radius - 2.0f, radius - 2.0f, 0.0f,
                         juce::jmin (originAngle, angle), juce::jmax (originAngle, angle), true);
    g.setColour (slider.isEnabled() ? slider.findColour (juce::Slider::rotarySliderFillColourId) : Colours::textDim);
    g.strokePath (value, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    const auto knob = juce::Rectangle<float> (radius * 1.35f, radius * 1.35f).withCentre (centre);
    juce::ColourGradient grad (juce::Colour (0xff3d4147), knob.getX(), knob.getY(), juce::Colour (0xff1f2125), knob.getX(), knob.getBottom(), false);
    g.setGradientFill (grad);
    g.fillEllipse (knob);

    juce::Path pointer;
    pointer.addRectangle (-1.0f, -radius * 0.66f, 2.0f, radius * 0.42f);
    g.setColour (Colours::text);
    g.fillPath (pointer, juce::AffineTransform::rotation (angle).translated (centre));
}

void ConsoleLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour& background,
                                               bool isMouseOver, bool isDown)
{
    auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
    auto colour = background;

    if (isDown)
        colour = colour.brighter (0.2f);
    else if (isMouseOver)
        colour = colour.brighter (0.08f);

    g.setColour (colour);
    g.fillRoundedRectangle (bounds, 3.0f);
    g.setColour (button.getToggleState() ? colour.brighter (0.3f) : Colours::outline);
    g.drawRoundedRectangle (bounds, 3.0f, 1.0f);
}

juce::Font ConsoleLookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    return juce::Font (juce::FontOptions (juce::jmin (13.0f, buttonHeight * 0.55f), juce::Font::bold));
}

juce::Font ConsoleLookAndFeel::getComboBoxFont (juce::ComboBox& box)
{
    return juce::Font (juce::FontOptions (juce::jmin (13.0f, box.getHeight() * 0.55f)));
}

juce::Font ConsoleLookAndFeel::getLabelFont (juce::Label& label)
{
    return label.getFont();
}

juce::Font ConsoleLookAndFeel::getPopupMenuFont()
{
    return juce::Font (juce::FontOptions (14.0f));
}

void drawSectionBox (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title, bool onScreen)
{
    g.setColour (onScreen ? Colours::screenPanel : Colours::panel);
    g.fillRoundedRectangle (area.toFloat().reduced (2.0f), 4.0f);
    if (title.isNotEmpty())
    {
        g.setColour (Colours::accent.withAlpha (0.85f));
        g.setFont (juce::FontOptions (11.5f, juce::Font::bold));
        g.drawText (title, area.reduced (10, 5).removeFromTop (14), juce::Justification::centredLeft);
    }
}

} // namespace livedaw::ui
