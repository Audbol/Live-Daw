#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace livedaw::ui
{

/** The WING's dark surface, black touch screen, cyan highlights and yellow
    processing blocks. */
namespace Colours
{
    const juce::Colour background { 0xff141517 };   // surface
    const juce::Colour panel { 0xff1d1f22 };
    const juce::Colour panelLight { 0xff2a2d31 };
    const juce::Colour outline { 0xff3a3e44 };
    const juce::Colour screen { 0xff07080a };       // main display background
    const juce::Colour screenPanel { 0xff14171b };
    const juce::Colour screenLine { 0xff262b31 };
    const juce::Colour text { 0xffe3e6eb };
    const juce::Colour textDim { 0xff8c939d };
    const juce::Colour accent { 0xff33c2e6 };       // WING cyan
    const juce::Colour block { 0xffe9b73a };        // processing blocks (on)
    const juce::Colour blockOff { 0xff4d4532 };
    const juce::Colour select { 0xff4aa8ff };       // SEL buttons
    const juce::Colour mute { 0xffe5413a };
    const juce::Colour solo { 0xffe9c23a };
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
    juce::Font getPopupMenuFont() override;
};

/** Draws a titled box on a page. */
void drawSectionBox (juce::Graphics&, juce::Rectangle<int> area, const juce::String& title, bool onScreen = true);

} // namespace livedaw::ui
