#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace livedaw::ui
{

namespace Colours
{
    const juce::Colour background { 0xff1b1d21 };
    const juce::Colour panel { 0xff25282d };
    const juce::Colour panelLight { 0xff30343a };
    const juce::Colour outline { 0xff3c4148 };
    const juce::Colour text { 0xffd8dce2 };
    const juce::Colour textDim { 0xff8a919b };
    const juce::Colour accent { 0xff4fa3ff };
    const juce::Colour mute { 0xffe0483e };
    const juce::Colour solo { 0xffe8c13a };
    const juce::Colour active { 0xff45c46b };
    const juce::Colour meterGreen { 0xff3ccf5b };
    const juce::Colour meterYellow { 0xffe8d23a };
    const juce::Colour meterRed { 0xffe8413a };
} // namespace Colours

/** Dark console-style look and feel. */
class ConsoleLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    ConsoleLookAndFeel();

    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPos, float minSliderPos, float maxSliderPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPosProportional, float rotaryStartAngle,
                           float rotaryEndAngle, juce::Slider&) override;

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                               bool isMouseOverButton, bool isButtonDown) override;

    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    juce::Font getLabelFont (juce::Label&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
};

} // namespace livedaw::ui
