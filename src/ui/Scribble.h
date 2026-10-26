#pragma once

#include "ui/Icons.h"
#include "ui/LookAndFeel.h"
#include "ui/UiContext.h"

#include <optional>
#include <utility>

namespace livedaw::ui
{

/** What a scribble strip LCD shows, laid out as the 3.x firmware draws it:
    - a top row under a rule: an input's number and its source on an
      inverted tag ("5" | "LCL5", "LCL7/8"), or the kind on the tag and the
      number at the right ("BUS" | "1"); under Sends on Fader an outlined tag
      names the send ("> BUS 3") in place of the source (or the number);
    - the centre: the strip's WING icon, or a value in large type with a
      caption (SHOW FADER VALUE ON SCRIBBLE, FX SPILL, the 4-Channel
      section's knob), or up to two small lines;
    - the name, centred (inverted for custom controls named "|..."), and an
      optional footer line under a rule (a key's state). */
struct ScribbleInfo
{
    juce::String number, tag, sendTag;
    bool tagFirst = false;        // the tag at the left, the number at the right
    std::optional<Icon> icon;     // drawn when `strip` has no WING icon
    const Strip* strip = nullptr; // draws its WING icon (none when unset)
    int wingIcon = 0;             // a WING icon without a strip (a DCA's)
    juce::String value, caption;  // "-3.5" in large type in place of the icon
    juce::String line2, line3;    // small centre lines (no icon or value)
    juce::String name, footer;
    bool footerRow = false;       // the footer's row under its rule even when empty (a key that does nothing here)
    bool nameInverted = false;
    float backlight = 1.0f, contrast = 1.0f; // SETUP > GENERAL: SCRIBBLE and SCR CONT

    bool isEmpty() const noexcept
    {
        return number.isEmpty() && tag.isEmpty() && sendTag.isEmpty() && name.isEmpty() && value.isEmpty() && line2.isEmpty()
               && line3.isEmpty() && footer.isEmpty() && strip == nullptr && wingIcon == 0 && ! icon.has_value();
    }
};

/** What a strip with nothing to control shows (a blank layer slot, a strip
    past the end of a layer, a target that's gone, a DAW strip without a
    channel): nothing at all, the LCD unlit, as on the console. */
inline ScribbleInfo blankScribble()
{
    ScribbleInfo info;
    info.backlight = 0.0f;
    return info;
}

/** Draws a scribble strip LCD into `lcd`. An unlit LCD (backlight 0, see
    blankScribble()) shows no text, whatever `info` holds. */
void drawScribble (juce::Graphics&, juce::Rectangle<float> lcd, const ScribbleInfo&);

/** A scribble LCD's backlight and ink at SCRIBBLE `backlight` and SCR CONT
    `contrast` (as in ScribbleInfo). */
std::pair<juce::Colour, juce::Colour> scribbleColours (float backlight, float contrast);

/** Draws the LCD's frame: its lighter rim along the given edges of one strip's
    LCD (`first` / `last`: the left / right ends of the display), and the rule
    between this strip and the next (unless `last`). */
void drawScribbleFrame (juce::Graphics&, juce::Rectangle<float> lcd, bool first, bool last, float backlight);

/** A strip's name as its scribble strip shows it: empty while it has the
    default name ("CH 3"), which the top row already says. */
juce::String scribbleName (const Strip&);
/** The source tag: "LCL5", "LCL7/8", "B1" (the ALT source while active), "OFF"
    without a source; with SHOW SOURCE ON SCRIBBLE off "MONO" or "ST". */
juce::String scribbleSourceTag (const Strip&, bool showSource);
/** Fills the top row, the name and the icon for a strip. */
void setScribbleStrip (ScribbleInfo&, const Strip&, bool showSource);

/** The colour a WING colour lights a light bar in: the screens' DARK BLUE
    (colour 1, the inputs' and mains' default) lights its LEDs plain blue. */
juce::Colour lightBarColour (juce::Colour screenColour);

/** Draws the colour light bar above a scribble strip (dark when off). */
void drawLightBar (juce::Graphics&, juce::Rectangle<float> bar, juce::Colour, bool on);

} // namespace livedaw::ui
