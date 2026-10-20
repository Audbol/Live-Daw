#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace livedaw::ui
{

/** Colours of the WING (firmware 3.1). The screen is drawn flat, as on the
    console's 16-bit panel: black, #202020 panels, #303030 keys and tiles,
    #101010 wells, #484848 outlines, grey #707070 unlit text and the gold
    accent (selected tab, ON keys, list selection, frames). The control
    surface around the screen keeps its own darker hardware colours. */
namespace Colours
{
    // Control surface (around the screen)
    const juce::Colour background { 0xff17181a };
    const juce::Colour panel { 0xff222326 };
    const juce::Colour panelLight { 0xff313338 };
    const juce::Colour outline { 0xff3e4146 };

    // Main display: chrome
    const juce::Colour screen { 0xff000000 };
    const juce::Colour screenPanel { 0xff202020 };   // panel body
    const juce::Colour screenHeader { 0xff202020 };  // panel header bar, unselected tabs, window title bars
    const juce::Colour screenTile { 0xff303030 };    // keys, tiles, block thumbs, the selected tab
    const juce::Colour screenKey = screenTile;
    const juce::Colour screenDark { 0xff101010 };    // wells: fields, list rows, keyboard keys, fader slots
    const juce::Colour screenWell = screenDark;
    const juce::Colour screenLine { 0xff484848 };    // key and field outlines
    const juce::Colour scrim { 0xbf000000 };         // 75 % black over the page behind a pop-up

    const juce::Colour text { 0xfff8f8f8 };
    const juce::Colour textDim { 0xff707070 };       // unlit or dim text
    const juce::Colour caption { 0xffc0c0c0 };       // captions, window titles
    const juce::Colour chipOff { 0xff585858 };       // unlit chips and selector segments

    // Main display: signal colours
    const juce::Colour gold { 0xfff0c858 };      // THE accent: selected tab, ON keys, list selection, frames, caret
    const juce::Colour yellow = gold;            // (older name) ON keys, selected tab text
    const juce::Colour amber { 0xfff0a830 };     // EQ curve, gain reduction, warnings
    const juce::Colour lime { 0xffc8d72b };      // olive tiles on ROUTING
    const juce::Colour cyan { 0xff6cc6f0 };      // DCA assignments
    const juce::Colour green { 0xff00bc38 };     // status chips, AFL, LIVE, POST
    const juce::Colour pink { 0xffe8338a };
    const juce::Colour choice { 0xff4074c8 };    // the lit choice key, the selected EQ band
    const juce::Colour valueBlue { 0xff4890e0 }; // levels, LC/HC, caps lock, MAIN
    const juce::Colour alt { 0xffb86018 };       // ALT keys and tiles
    const juce::Colour pfl { 0xffe00040 };       // PFL, TAP mode
    const juce::Colour tapRed { 0xfff84040 };    // the TAP POINT node
    const juce::Colour width { 0xff40a618 };     // WIDTH fill, knob rings
    const juce::Colour eqFill { 0xff38280f };    // the area under the EQ curve
    const juce::Colour warning { 0xffe0c040 };   // AM/PM, write-protected card
    const juce::Colour midiCc { 0xff3aa8a0 };    // MIDI CC faders (user layers)

    const juce::Colour accent = gold;
    const juce::Colour block = amber;            // processing block on
    const juce::Colour blockOff = screenTile;
    const juce::Colour select = cyan;
    const juce::Colour mute { 0xffd00018 };
    const juce::Colour solo = gold;
    const juce::Colour active = green;
    const juce::Colour meterGreen { 0xff35d05a };
    const juce::Colour meterYellow { 0xffe8d23a };
    const juce::Colour meterOrange { 0xfff0641e };
    const juce::Colour meterRed { 0xffe8413a };

    // Control surface hardware (the WING's dark panel: same layout and print
    // as the silver one)
    const juce::Colour keyBody { 0xff36383d };      // an unlit key
    const juce::Colour keyEdge { 0xff0d0e10 };
    const juce::Colour keyCap { 0xffc8cbd1 };       // an unlit key's frosted cap
    const juce::Colour keyCapDark { 0xff16171a };   // the black keys (LISTEN, DAW)
    const juce::Colour keyBezel { 0xff08090a };     // the black ring round a key
    const juce::Colour keyPrint { 0xff1b1c1f };     // print on a key cap
    const juce::Colour keySelect { 0xff4fdc64 };    // a lit SELECT key
    const juce::Colour keyLayer { 0xffff9a28 };     // a lit layer or slot key
    const juce::Colour keyWhite { 0xfff4f6fb };     // a key lit white
    const juce::Colour silk { 0xffc4c7cc };         // panel print
    const juce::Colour silkDim { 0xff8a8d92 };      // fainter print (fader scales)
    const juce::Colour layerColumn { 0xff44474c };  // the lighter grey layer key columns
    const juce::Colour plate { 0xff1d1e21 };        // the darker plates (4-CHANNEL)
    const juce::Colour sectionLine { 0xff8b8e94 };  // printed section borders
    const juce::Colour scribbleLcd { 0xff201d4a };  // a scribble strip LCD's backlight (deep indigo)
    const juce::Colour scribbleInk { 0xffeeedff };  // its text, icons and inverted tags
    const juce::Colour scribbleRule { 0xffa6a2dc }; // the rules between an LCD's strips
    const juce::Colour scribbleRim { 0xff4a4690 };  // the lighter rim round the LCD
    const juce::Colour ledOff { 0xff2c2d30 };       // an unlit LED
    const juce::Colour ledYellow { 0xfff5e14a };    // a lit surface key
    const juce::Colour viewGreen { 0xff52d36b };    // lit VIEW keys
} // namespace Colours

//==============================================================================
/** The WING's screen type: Barlow Semi Condensed (a DIN-style face as wide as
    the console's), Medium for labels and values, SemiBold for big numbers and
    titles. Sizes follow the console's type scale, given as cap heights in
    shares of the screen height (WING 1280x800 pixels / 800). */
enum class WingWeight
{
    Medium,
    SemiBold
};

namespace TypeScale
{
    constexpr float tab = 0.025f;           // selected tab text (unselected 0.0212)
    constexpr float header = 0.025f;        // panel header titles
    constexpr float value = 0.025f;         // knob-box values ("0.0 dB")
    constexpr float caption = 0.0175f;      // knob-box captions
    constexpr float body = 0.0175f;         // labels, segmented key text, list rows
    constexpr float paramLabel = 0.0188f;   // parameter-bar labels and values
    constexpr float statusCaption = 0.0125f;// status-key captions ("INIT", "COPY")
    constexpr float statusTitle = 0.036f;   // the channel number and name in the status tab
    constexpr float meterScale = 0.0112f;   // meter scales
} // namespace TypeScale

/** The height in px of the main display's screen, which the type scale refers
    to (MainDisplay sets it; 500 until then). */
void setWingScreenHeight (float px) noexcept;
float getWingScreenHeight() noexcept;

/** A size cap in px tuned for the screen of a 1920 x 1080 window (500 px
    tall), grown in proportion on larger screens (screen only mode), so the
    layouts keep the WING's proportions instead of stopping at the cap. */
inline int screenCap (int px) noexcept
{
    return juce::roundToInt ((float) px * juce::jmax (1.0f, getWingScreenHeight() / 500.0f));
}

inline float screenCap (float px) noexcept
{
    return px * juce::jmax (1.0f, getWingScreenHeight() / 500.0f);
}

/** `wingPixels` of the WING's 1280 x 800 screen, in px of the screen shown. */
inline float wingPx (float wingPixels) noexcept { return wingPixels * getWingScreenHeight() / 800.0f; }

/** A screen font whose capital letters are `capFraction` of the screen height
    (see TypeScale), with the console's slight letter spacing. */
juce::Font wingFont (float capFraction, WingWeight = WingWeight::Medium);

/** A screen font whose capital letters are `capHeightPx` pixels tall. */
juce::Font wingFontPx (float capHeightPx, WingWeight = WingWeight::Medium);

/** wingFont (`capFraction`), no taller than `maxHeightPx` (JUCE font height)
    so it fits small keys and tiles. */
inline juce::Font wingFontFit (float capFraction, float maxHeightPx, WingWeight weight = WingWeight::Medium)
{
    auto f = wingFont (capFraction, weight);
    return f.getHeight() > maxHeightPx ? f.withHeight (juce::jmax (6.0f, maxHeightPx)) : f;
}

/** `f`, made smaller when `text` would not fit `width` px even squashed to
    `minScale` (narrow columns in small windows). */
inline juce::Font fontFittingWidth (juce::Font f, const juce::String& text, float width, float minScale = 0.75f)
{
    const float w = juce::GlyphArrangement::getStringWidth (f, text) * minScale;
    return w > width && w > 0.0f ? f.withHeight (juce::jmax (6.0f, f.getHeight() * width / w)) : f;
}

/** The surface print's narrower face (Barlow Condensed), `height` px tall in
    JUCE terms: for silk print, hardware keys and scales around the screen. */
juce::Font silkFont (float height, bool bold = true);

/** The typeface name that ConsoleLookAndFeel maps to the surface face
    (Barlow Condensed); every other sans-serif font gets the screen face. */
constexpr const char* kSilkTypefaceName = "WingSilk";

/** The WING look: Barlow Semi Condensed on the screen (Barlow Condensed for
    the surface print, see kSilkTypefaceName), flat keys, WING tabs, dropdown
    fields, lists with the selected row filled gold, and knob rings. */
class ConsoleLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    ConsoleLookAndFeel();

    juce::Typeface::Ptr getTypefaceForFont (const juce::Font&) override;

    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPos, float minSliderPos, float maxSliderPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;
    /** Surface faders keep their tall cap inside the slider. */
    int getSliderThumbRadius (juce::Slider&) override;

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

    void drawPopupMenuBackground (juce::Graphics&, int width, int height) override;
    void drawPopupMenuItem (juce::Graphics&, const juce::Rectangle<int>& area, bool isSeparator, bool isActive,
                            bool isHighlighted, bool isTicked, bool hasSubMenu, const juce::String& text,
                            const juce::String& shortcutKeyText, const juce::Drawable* icon,
                            const juce::Colour* textColour) override;
    void drawPopupMenuSectionHeader (juce::Graphics&, const juce::Rectangle<int>& area, const juce::String& sectionName) override;
    void getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator, int standardMenuItemHeight,
                                    int& idealWidth, int& idealHeight) override;
    int getPopupMenuBorderSize() override { return 1; }

private:
    juce::Typeface::Ptr screenMedium, screenSemiBold, silkMedium, silkSemiBold;
};

/** Height of a HOME panel's header bar: 64 of the WING's 800 px screen
    [I1, D5, D9], so it follows the screen size. */
inline int panelHeaderHeight() { return juce::jmax (20, juce::roundToInt (wingPx (64.0f))); }

/** Draws a WING panel: a grey header bar with the title (and a chevron when the
    panel opens a screen) above the panel body. */
void drawSectionBox (juce::Graphics&, juce::Rectangle<int> area, const juce::String& title, bool chevron = false);

/** The content area inside a panel drawn by drawSectionBox. */
juce::Rectangle<int> sectionContent (juce::Rectangle<int> area, bool hasTitle);

/** The whole body of a panel drawn by drawSectionBox (no inset), for content
    that fills it edge to edge (the overview's EQ graph). */
juce::Rectangle<int> sectionBody (juce::Rectangle<int> area, bool hasTitle);

/** The header bar of a panel drawn by drawSectionBox. */
juce::Rectangle<int> sectionHeader (juce::Rectangle<int> area);

/** A small rounded label chip ("48K", "PRE", "SNAP"). */
void drawChip (juce::Graphics&, juce::Rectangle<float> r, const juce::String& text, juce::Colour fill, juce::Colour textColour);

/** Panel print on the control surface (captions, headings): bold caps, one
    line per '\n'. */
void drawSilk (juce::Graphics&, juce::Rectangle<float> area, const juce::String& text, float fontSize,
               juce::Justification = juce::Justification::centred, juce::Colour = Colours::silk);

} // namespace livedaw::ui
