#pragma once

#include "ui/Knobs.h"
#include "ui/MeterComponent.h"
#include "ui/UiContext.h"
#include "ui/WingWidgets.h"

#include <optional>

namespace livedaw::ui
{

/** What a scribble strip LCD shows, laid out as the 3.x firmware draws it:
    - a top row under a rule: an input's number and its source on an
      inverted tag ("5" | "IN5", "IN7/8"), or the kind on the tag and the
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
    bool nameInverted = false;
    float backlight = 1.0f, contrast = 1.0f; // SETUP > GENERAL: SCRIBBLE and SCR CONT
    /** The strip's colour: the console lights its LCD in it (white print).
        Unset: the finish's plain LCD (SurfacePalette::scribbleLcd). */
    std::optional<juce::Colour> backlightColour;

    bool isEmpty() const noexcept
    {
        return number.isEmpty() && tag.isEmpty() && sendTag.isEmpty() && name.isEmpty() && value.isEmpty() && line2.isEmpty()
               && line3.isEmpty() && footer.isEmpty() && strip == nullptr && wingIcon == 0 && ! icon.has_value();
    }
};

/** Draws a scribble strip LCD into `lcd`. */
void drawScribble (juce::Graphics&, juce::Rectangle<float> lcd, const ScribbleInfo&);

/** Draws the LCD's frame: its lighter rim along the given edges of one strip's
    LCD (`first` / `last`: the left / right ends of the display), and the rule
    between this strip and the next (unless `last`). */
void drawScribbleFrame (juce::Graphics&, juce::Rectangle<float> lcd, bool first, bool last, float backlight);

/** A strip's name as its scribble strip shows it: empty while it has the
    default name ("CH 3"), which the top row already says. */
juce::String scribbleName (const Strip&);
/** The source tag: "IN5", "IN7/8" (the ALT source while active), "OFF"
    without a source; with SHOW SOURCE ON SCRIBBLE off "MONO" or "ST". */
juce::String scribbleSourceTag (const Strip&, bool showSource);
/** Fills the top row, the name and the icon for a strip. */
void setScribbleStrip (ScribbleInfo&, const Strip&, bool showSource);

/** Draws the colour light bar above a scribble strip (dark when off). */
void drawLightBar (juce::Graphics&, juce::Rectangle<float> bar, juce::Colour, bool on);

/** One fader strip of a WING fader section, top to bottom: the colour light
    bar and scribble strip, SELECT, SOLO, the meter block (DYN LED, a stereo
    6-segment input meter, GATE LED), MUTE and the fader with its printed scale.
    It controls a strip, a DCA, a send level (Sends on Fader between the
    selected strip and this one; a send fader of a layer or spill), an FX
    slot's parameter (FX SPILL) or a MIDI CC sent out of EXTERNAL MIDI CONTROL
    (a user layer's MIDI CC fader). */
class FaderStrip final : public juce::Component, private juce::Timer
{
public:
    FaderStrip (UiContext&, LayerSet::Section);

    /** What the strip controls (nullopt = an empty position). */
    void setTarget (std::optional<state::FaderRef>);
    /** Re-resolves the target (after structure, selection or SOF changes). */
    void refresh();

    /** The meter block's LED rows (DYN, CLIP .. -60, GATE), for the section
        to print the scale between strips. */
    juce::Rectangle<int> getMeterArea() const noexcept { return meterArea; }
    /** The colour bar and LCD, for the section's frame around four strips. */
    juce::Rectangle<int> getScribbleArea() const noexcept { return scribbleArea; }
    /** The strip this fader controls (nullptr for a DCA or an empty position). */
    Strip* getStrip() const noexcept { return strip; }

    /** Where the strip sits in its module of four: the LCD and meter panels
        are rounded at the module's outer edges. */
    void setModuleEdges (bool first, bool last);

    /** The meter block's rows: DYN, the six segments, GATE. */
    static constexpr int kMeterRows = 8;
    static constexpr int kMeterSegments = 6;
    static constexpr float kMeterThresholds[kMeterSegments] = { -0.1f, -6.0f, -12.0f, -18.0f, -30.0f, -60.0f };
    static constexpr const char* kMeterLabels[kMeterSegments] = { "CLIP", "-6", "-12", "-18", "-30", "-60" };

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    /** A send shown on this fader under Sends on Fader. */
    struct SendTarget
    {
        std::atomic<float>* level = nullptr;
        std::atomic<bool>* on = nullptr;
        juce::String tag; // "> BUS 3", "> MAIN 1", "< CH 5"
    };

    /** An FX slot's parameter on this fader: the fader travel is the
        parameter's knob travel, MUTE sets its default (0 dB for EQ gains). */
    struct ParamTarget
    {
        std::atomic<float>* value = nullptr;
        int slot = 0, model = 0, index = 0;
    };

    /** A MIDI CC on this fader: the travel is 0-127, sent on every change. */
    struct MidiTarget
    {
        int channel = 0, cc = 0; // 0-15, 0-127
    };
    /** The CC's value, as last sent or received. */
    int midiValue() const noexcept;

    void timerCallback() override;
    /** The fader position (dB travel) for the current target's value, and back. */
    double faderPosition() const;
    void writeFader (double position);
    bool atDefault() const;
    void bindControls();
    void pressSelect();
    /** Moves the fader towards its parameter at the FADER SPEED. */
    void glide (double target);
    std::atomic<float>* faderParam() const noexcept;
    ScribbleInfo scribbleInfo() const;

    UiContext& ctx;
    const LayerSet::Section section;
    std::optional<state::FaderRef> target;
    Strip* strip = nullptr;
    int dca = -1;
    std::optional<SendTarget> send;
    std::optional<ParamTarget> fxp;
    std::optional<MidiTarget> midi;

    juce::Slider fader;
    SurfaceKey selectKey { "SELECT" }, soloKey { "SOLO" }, muteKey { "MUTE" };
    juce::Rectangle<int> scribbleArea, barArea, lcdArea, meterArea, valueArea;
    bool showValue = false, moduleFirst = false, moduleLast = false;

    // What's drawn, to repaint only on changes.
    juce::String shownName, shownValue, shownSource;
    std::array<int, 2> shownLit {};
    bool shownGate = false, shownDyn = false, shownLight = false, shownFlash = false;
    int shownColour = 0;
    bool retargeted = true;          // the next value change is a new target, not a move
    juce::uint32 valueFlashUntil = 0; // SHOW FADER VALUE ON SCRIBBLE: the value replaces the icon until then
};

} // namespace livedaw::ui
