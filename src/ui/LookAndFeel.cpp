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
    setColour (juce::Slider::thumbColourId, Colours::text);
    setColour (juce::Slider::rotarySliderFillColourId, Colours::accent);
    setColour (juce::Slider::rotarySliderOutlineColourId, Colours::outline);
    setColour (juce::Slider::textBoxTextColourId, Colours::text);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Label::textColourId, Colours::text);
    setColour (juce::ComboBox::backgroundColourId, Colours::panelLight);
    setColour (juce::ComboBox::outlineColourId, Colours::outline);
    setColour (juce::PopupMenu::backgroundColourId, Colours::panel);
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

    // Fader: slot + cap.
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat();
    const auto slot = juce::Rectangle<float> (bounds.getCentreX() - 2.0f, bounds.getY(), 4.0f, bounds.getHeight());
    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.fillRoundedRectangle (slot, 2.0f);

    // Unity (0 dB) tick.
    const auto unityY = static_cast<float> (slider.getPositionOfValue (0.0));
    g.setColour (Colours::textDim);
    g.drawHorizontalLine (juce::roundToInt (unityY), bounds.getX() + 2.0f, bounds.getRight() - 2.0f);

    const auto cap = juce::Rectangle<float> (bounds.getWidth() * 0.8f, 26.0f).withCentre ({ bounds.getCentreX(), sliderPos });
    g.setColour (juce::Colour (0xffc9ced6));
    g.fillRoundedRectangle (cap, 3.0f);
    g.setColour (juce::Colours::black.withAlpha (0.7f));
    g.drawHorizontalLine (juce::roundToInt (cap.getCentreY()), cap.getX() + 2.0f, cap.getRight() - 2.0f);
}

void ConsoleLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                           float pos, float startAngle, float endAngle, juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (2.0f);
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const float angle = startAngle + pos * (endAngle - startAngle);
    const bool bipolar = slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0;
    const float originAngle = bipolar ? startAngle + static_cast<float> (slider.valueToProportionOfLength (0.0)) * (endAngle - startAngle)
                                      : startAngle;

    juce::Path track;
    track.addCentredArc (centre.x, centre.y, radius - 2.0f, radius - 2.0f, 0.0f, startAngle, endAngle, true);
    g.setColour (Colours::outline);
    g.strokePath (track, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    juce::Path value;
    value.addCentredArc (centre.x, centre.y, radius - 2.0f, radius - 2.0f, 0.0f,
                         juce::jmin (originAngle, angle), juce::jmax (originAngle, angle), true);
    g.setColour (slider.isEnabled() ? slider.findColour (juce::Slider::rotarySliderFillColourId) : Colours::textDim);
    g.strokePath (value, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    g.setColour (Colours::panelLight);
    g.fillEllipse (juce::Rectangle<float> (radius * 1.3f, radius * 1.3f).withCentre (centre));

    juce::Path pointer;
    pointer.addRectangle (-1.0f, -radius * 0.65f, 2.0f, radius * 0.45f);
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
    g.setColour (Colours::outline);
    g.drawRoundedRectangle (bounds, 3.0f, 1.0f);
}

juce::Font ConsoleLookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    return juce::Font (juce::FontOptions (juce::jmin (13.0f, buttonHeight * 0.6f)));
}

juce::Font ConsoleLookAndFeel::getComboBoxFont (juce::ComboBox& box)
{
    return juce::Font (juce::FontOptions (juce::jmin (13.0f, box.getHeight() * 0.55f)));
}

juce::Font ConsoleLookAndFeel::getLabelFont (juce::Label& label)
{
    return label.getFont();
}

} // namespace livedaw::ui
