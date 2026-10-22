#pragma once

#include "ui/Icons.h"
#include "ui/Knobs.h"
#include "ui/UiContext.h"

#include <optional>
#include <vector>

namespace livedaw::ui
{

/** A key at the right of the status bar. The WING changes these with the screen
    (INIT / COPY / SETTINGS on HOME, MONITORS / CC EDIT on SETUP, GLOBAL on
    LIBRARY, the screen keys of the VIEW screens...). */
struct StatusKey
{
    juce::String text;
    std::optional<Icon> icon;
    bool lit = false;
    std::function<void()> onClick;
    /** The icon and caption colour of an unlit key (LIBRARY's GLOBAL: a green
        open padlock, red while something is global-safe); grey when unset. */
    std::optional<juce::Colour> ink {};
};

/** How the parameter bar shows one of the six knobs. The WING colours each value
    by its type (levels and frequencies blue, width green, distance yellow, tilt
    cyan, trim and pan white) and draws a matching track under it: a fill from the
    left for levels, a fill from the centre for width, only a thumb for pan, trim
    and balance, and a strip of segments for selectors. */
struct KnobStyle
{
    enum class Bar
    {
        Auto,     // chosen from the binding (label, steps, bipolar)
        Fill,     // filled from the left
        Centre,   // filled from the centre, with a centre tick
        Thumb,    // a thumb on a grey line
        Segments  // one lit segment per step (BUS 1-16, MATRIX 1-8)
    };

    Bar bar = Bar::Auto;
    juce::Colour colour {}; // transparent: chosen from the label
    int segments = 0;       // Bar::Segments: how many (0 = the binding's steps)
    /** The on-screen control the knob changes: outlined while the knob is touched. */
    juce::Component::SafePointer<juce::Component> target;
};

/** The parameter bar's value colours (WING firmware 3.1). */
namespace KnobColours
{
    const juce::Colour blue { 0xff4890e0 };      // levels, frequencies, BAL, PAGE
    const juce::Colour green { 0xff40a618 };     // width, EQ Q
    const juce::Colour yellow { 0xffe0d400 };    // distance, AP frequency
    const juce::Colour cyan { 0xff48f0f8 };      // tilt
    const juce::Colour white { 0xfff8f8f8 };     // trim, pan, EQ gain
    const juce::Colour orange { 0xfff8a008 };    // preamp gain, MAXER
    const juce::Colour red { 0xfff75c58 };       // decay, reverb time
    const juce::Colour timeGreen { 0xff1ed664 }; // pre-delay, multipliers, delay times
    const juce::Colour gold { 0xfff0c858 };      // selectors (BUS 13): the value and the lit segment
    const juce::Colour segmentOff { 0xff585858 };// a selector's other segments
    const juce::Colour track { 0xff686868 };     // the grey line under a thumb
} // namespace KnobColours

/** What the status bar's title tile shows for a screen. The WING shows the
    selected channel on most screens, a source or output on ROUTING's SOURCES /
    OUTPUTS, the layer on the VIEW screens and a plain title on MUTE GROUPS or
    CUSTOM CONTROLS. */
struct StatusTitle
{
    enum class Kind
    {
        Channel, // the selected strip (number, icon, name in its colour): the default
        Source,  // grey tile, black text, connector icon
        Layer,   // dark tile, grey text, section letter + layers icon + layer name
        Text     // icon and text
    };

    Kind kind = Kind::Channel;
    juce::String prefix;       // e.g. the section letter or "A1"
    juce::String text;
    std::optional<Icon> icon;
    juce::Colour fill {}, ink {}; // transparent: the kind's default colours
    juce::Colour iconInk {};      // a plain title's icon in its own colour (MUTE GROUPS: red)
};

/** A screen of the main display. */
class DisplayPage : public juce::Component
{
public:
    ~DisplayPage() override = default;

    /** The six knobs under the display for what this page shows (fewer is fine). */
    virtual std::vector<KnobBinding> getKnobs() { return {}; }

    /** How to show each of getKnobs() (fewer, or none: chosen from the bindings). */
    virtual std::vector<KnobStyle> getKnobStyles() { return {}; }

    /** False for screens that don't use the six knobs: the display then hides
        the parameter bar and gives the page its space. */
    virtual bool usesKnobs() const { return true; }

    /** The keys this screen puts at the right of the status bar, left to right
        (the clock always follows). Empty: none, as on the WING's CUSTOM CONTROLS
        and MONITORS screens. */
    virtual std::vector<StatusKey> getStatusKeys() { return {}; }

    /** The blank key under the knob right of the display: "performs various
        functions depending on the active screen, such as acting as a tap tempo
        button when editing delay effects". Its LED is lit while this returns true. */
    virtual bool hasBlankKeyFunction() const { return false; }
    virtual void blankKeyPressed() {}

    /** The status bar's title tile for this screen. */
    virtual StatusTitle getStatusTitle() { return {}; }

    /** The UTILITY key "only lights up where it serves a specific function"
        (manual 4.1): pages with one return true and act in utilityPressed(). */
    virtual bool hasUtilityFunction() const { return false; }
    virtual void utilityPressed() {}

    /** The screen's own key pressed while the screen is up (SETUP pressed twice
        opens CC EDIT, firmware 3.1). Return true when the page handled it;
        false shows the screen again as usual. */
    virtual bool screenKeyPressedAgain() { return false; }

    /** The selected strip or the console structure changed. */
    virtual void refresh() {}

    /** Called by the page when its knob assignment changed. */
    std::function<void()> knobsChanged;

    /** Called by the page when its status keys or `usesKnobs()` changed. */
    std::function<void()> statusKeysChanged;

protected:
    void notifyKnobsChanged()
    {
        if (knobsChanged)
            knobsChanged();
    }

    void notifyStatusKeysChanged()
    {
        if (statusKeysChanged)
            statusKeysChanged();
    }
};

} // namespace livedaw::ui
