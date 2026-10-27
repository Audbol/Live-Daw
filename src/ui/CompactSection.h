#pragma once

#include "ui/CustomControlsPage.h"
#include "ui/FaderStrip.h"
#include "ui/SurfaceLayout.h"

namespace livedaw::ui
{

namespace compact = layout::compact;

/** The WING COMPACT's key section, right of its fader section (the Main
    Fader Section, WING manual 4.2, Fig. 11): BUS, DCA, MAIN MATRIX and USER
    choose what the 16 keys do (/$ctl/cfg/cscmode), VIEW, the small LCD
    naming the keys and what fader 13 controls, the 16 keys and fader 13 (the
    main fader, /$ctl/cfg/mfdr; MAIN 1 by default), which has no scribble
    strip, only its level meter.

    A key selects its bus, main or matrix: fader 13 controls it until the key
    is pressed again (or another one is), then goes back to MAIN FADER's
    source; KEEP MAIN FADER (/$ctl/cfg/mfdrkeep) leaves fader 13 alone.
    - BUS: BUS 1-16; a press shows the channels' sends to the bus on the
      faders (SOF), held spills the bus (the sources sending to it) [WING
      manual 4.2, a WING COMPACT's owner]; BUS SPILL swaps them.
    - MAIN MATRIX: MAIN 1-4 on keys 1-4, the custom control set M/M on keys
      5-8, MATRIX 1-8 on keys 9-16. A MAIN key pressed shows the channels'
      sends to the main on the faders (SOF), held spills the main (the
      strips feeding it); MAIN SPILL swaps them. A MATRIX key does what
      MATRIX SPILL says: on (the factory setting), a press spills the matrix
      (the strips sending to it) and a hold shows its sends; off, the other
      way round (layout::compact::mainMatrixActions).
    - DCA: DCA 1-16; a press spills the DCA (its members onto the faders)
      and puts it on fader 13, both (a WING COMPACT's owner; pressed again,
      both end). With USE DCA BUTTONS 1-16 AS CC (/$ctl/cfg/dcacc) the keys
      are the custom control sets D1-D4. A DCA key pressed, then fader 13's SELECT held:
      the fader section's SELECT keys put their strips in the DCA or take
      them out (lit on its members) until SELECT is let go (manual 4.2 p. 16).
    A custom control key with a parameter (FX PAR) gives it to the knob right
    of the display (manual 4.8 p. 25: "hold down the custom control and use
    the knob to the right of the main display").
    - USER: the custom control sets U1-U4 (keys 1-4 U1 ... 13-16 U4).
    In DAW mode (DAW LAYER on a custom control key, WING manual 4.8) the 16
    keys and the four mode keys do nothing until the user assigns them (a
    WING COMPACT's owner): they play the sets DAW KEYS 1-4 .. 13-16 and DAW
    MODE KEYS, empty at first, edited in CC EDIT like the other sets. A key
    whose DAW control is empty but whose control in the mode's own set is
    DAW LAYER still leaves DAW mode, so the key that turned it on turns it off.
    VIEW opens the FADERS screen; held, CUSTOM CONTROLS on the mode's set
    (WING manual Table 4.4). */
class CompactSection final : public juce::Component, private juce::Timer
{
public:
    explicit CompactSection (UiContext&);

    /** The fader strips' width (fader 13 and the rows line up with them). */
    void setStripWidth (int);
    /** Structure, selection or settings changed: re-resolves fader 13 and the keys. */
    void refresh();

    /** Fader 13's own assignment (MAIN FADER), from the app settings. */
    compact::MainFader getMainFader() const;
    /** What fader 13 controls now: the selected key's bus, main or matrix, or MAIN FADER's source. */
    compact::MainFader shownMainFader() const;
    /** The strip on fader 13 (nullptr for a DCA or OFF). */
    Strip* getMainFaderStrip() const { return fader13.getStrip(); }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    compact::KeyMode keyMode() const;
    void setKeyMode (compact::KeyMode);
    void pressKey (int key, bool held);
    /** A BUS, MAIN or MATRIX key's spill or sends. */
    cc::Function actionFunction (int key, compact::KeyAction) const;
    /** Key `key`'s custom control in the current mode, if it plays one. */
    std::optional<compact::KeyControl> keyControl (int key) const;
    /** Key `key`'s function in the current mode (a spill, a custom control key). */
    cc::Function keyFunction (int key) const;
    /** What key `key` puts on fader 13, if the console has it. */
    std::optional<compact::MainFader> keyMainFader (int key) const;
    /** The strip a fader 13 assignment controls (nullptr for a DCA, OFF or one the console lacks). */
    Strip* mainFaderStrip (const compact::MainFader&) const;
    bool keyLit (int key) const;
    /** DAW remote control is on: the keys and mode keys play the DAW sets. */
    bool dawKeys() const;
    /** The function of a custom control key set's control. */
    cc::Function setFunction (const compact::KeyControl&) const;
    /** A mode key's function in DAW mode (the set DAW MODE KEYS). */
    cc::Function modeKeyFunction (int modeKey) const;
    /** A key or mode key going down (true) or up in DAW mode: its function
        acts while held (DAW BUTTONS push); returns whether it took the press. */
    bool dawDownUp (int index, bool down, bool modeKey);
    juce::String keyName (int key) const;
    /** The colour of the custom control on key `key` (transparent: none). */
    juce::Colour keyColour (int key) const;
    juce::String lcdState() const;

    UiContext& ctx;
    int stripW = 70;
    std::unique_ptr<SurfaceKey> modeKeys[4];
    ViewKey viewKey;
    juce::OwnedArray<SurfaceKey> keys;
    FaderStrip fader13;
    juce::Rectangle<int> lcd;
    juce::String shownLcd;
    compact::MainFader shownFader;
    /** The key selected in the current mode (-1: none). */
    int selectedKey = -1;
    /** The DCA key last pressed in DCA mode (the DCA fader 13's SELECT assigns to), or -1. */
    int assignDca = -1;
    /** DAW mode: what each key (0-15) and mode key (16-19) pressed, released
        when it goes up, and whether the down took the press (onPress and
        onHold then do nothing). */
    std::array<cc::Function, compact::kNumKeys + 4> dawHeld {};
    std::array<bool, compact::kNumKeys + 4> dawTook {};
    bool viewOpened = false;
};

} // namespace livedaw::ui
