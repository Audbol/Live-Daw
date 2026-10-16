#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace livedaw::ui
{

/** Colours of the WING: a dark control surface, and a black touch screen with
    flat grey panels, the amber channel tab, yellow ON keys, lime knob rings
    and light blue selections. */
namespace Colours
{
    // Control surface (around the screen)
    const juce::Colour background { 0xff17181a };
    const juce::Colour panel { 0xff222326 };
    const juce::Colour panelLight { 0xff313338 };
    const juce::Colour outline { 0xff3e4146 };

    // Main display
    const juce::Colour screen { 0xff000000 };
    const juce::Colour screenPanel { 0xff3a3a3a };   // panel body
    const juce::Colour screenHeader { 0xff4b4b4b };  // panel header bar, keys
    const juce::Colour screenTile { 0xff5a5a5a };    // tiles (channels, sources)
    const juce::Colour screenDark { 0xff1e1e1e };    // wells (graphs, meters)
    const juce::Colour screenLine { 0xff545454 };

    const juce::Colour text { 0xffffffff };
    const juce::Colour textDim { 0xffa8a8a8 };

    const juce::Colour amber { 0xffe9a23b };   // selected channel tab, selected rows
    const juce::Colour yellow { 0xfff5e14a };  // ON keys, selected tab text
    const juce::Colour lime { 0xffc8d72b };    // knob rings, monitor keys
    const juce::Colour cyan { 0xff6cc6f0 };    // selections, fader fill, numbers
    const juce::Colour green { 0xff3ddc84 };   // status chips
    const juce::Colour pink { 0xffe8338a };    // PRE chips

    const juce::Colour accent = cyan;
    const juce::Colour block = amber;          // processing block on
    const juce::Colour blockOff { 0xff4b4b4b };
    const juce::Colour select = cyan;
    const juce::Colour mute { 0xffe53935 };
    const juce::Colour solo = yellow;
    const juce::Colour active = green;
    const juce::Colour meterGreen { 0xff35d05a };
    const juce::Colour meterYellow { 0xffe8d23a };
    const juce::Colour meterRed { 0xffe8413a };
} // namespace Colours

/** The WING look: Barlow Condensed type (a DIN-style condensed face like the
    console's), flat keys, WING tabs, combo boxes and knob rings. */
class ConsoleLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    ConsoleLookAndFeel();

    juce::Typeface::Ptr getTypefaceForFont (const juce::Font&) override;

    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPos, float minSliderPos, float maxSliderPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPosProportional, float rotaryStartAngle,
                           float rotaryEndAngle, juce::Slider&) override;

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                               bool isMouseOverButton, bool isButtonDown) override;

    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown, int buttonX, int buttonY,
                       int buttonW, int buttonH, juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;

    void drawTabButton (juce::TabBarButton&, juce::Graphics&, bool isMouseOver, bool isMouseDown) override;
    void drawTabAreaBehindFrontButton (juce::TabbedButtonBar&, juce::Graphics&, int, int) override {}
    int getTabButtonBestWidth (juce::TabBarButton&, int tabDepth) override;

    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    juce::Font getLabelFont (juce::Label&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    juce::Font getPopupMenuFont() override;

private:
    juce::Typeface::Ptr regular, bold;
};

/** Height of a panel's header bar. */
constexpr int kPanelHeaderHeight = 30;

/** Draws a WING panel: a grey header bar with the title (and a chevron when the
    panel opens a screen) above the panel body. */
void drawSectionBox (juce::Graphics&, juce::Rectangle<int> area, const juce::String& title, bool chevron = false);

/** The content area inside a panel drawn by drawSectionBox. */
juce::Rectangle<int> sectionContent (juce::Rectangle<int> area, bool hasTitle);

/** A small rounded label chip ("48K", "PRE", "SNAP"). */
void drawChip (juce::Graphics&, juce::Rectangle<float> r, const juce::String& text, juce::Colour fill, juce::Colour textColour);

} // namespace livedaw::ui
