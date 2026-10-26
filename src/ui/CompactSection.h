#pragma once

#include "ui/CustomControlsPage.h"
#include "ui/FaderStrip.h"
#include "ui/SurfaceLayout.h"

namespace livedaw::ui
{

namespace compact = layout::compact;

/** The WING COMPACT's key section, right of its fader section: BUS, DCA,
    MAIN MATRIX and USER choose what the 16 keys do (/$ctl/cfg/cscmode), VIEW,
    the small LCD naming the keys and what fader 13 controls, the 16 keys and
    fader 13 (the main fader, /$ctl/cfg/mfdr; MAIN 1 by default), which has
    no scribble strip, only its level meter.

    The keys [a WING COMPACT's owner]:
    - BUS: BUS 1-16; a press spills the bus (the sources sending to it) onto
      the faders and puts the bus on fader 13.
    - MAIN MATRIX: MAIN 1-4 on keys 1-4, MATRIX 1-8 on keys 9-16, keys 5-8
      empty; a press puts the main or matrix on fader 13.
    - DCA: DCA 1-16; a press spills the DCA's members onto the faders.
    - USER: custom control keys: 1-8 the USER layer's (CC EDIT > USER), 9-16
      those of the custom control layer shown (LAYER 1 ...), each control's
      upper key then its lower one. */
class CompactSection final : public juce::Component, private juce::Timer
{
public:
    explicit CompactSection (UiContext&);

    /** The fader strips' width (fader 13 and the rows line up with them). */
    void setStripWidth (int);
    /** Structure, selection or settings changed: re-resolves fader 13 and the keys. */
    void refresh();

    /** Fader 13's assignment, from the app settings. */
    compact::MainFader getMainFader() const;
    /** The strip on fader 13 (nullptr for a DCA or OFF). */
    Strip* getMainFaderStrip() const { return fader13.getStrip(); }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    compact::KeyMode keyMode() const;
    void setKeyMode (compact::KeyMode);
    /** Key `key`'s custom control function in the current mode (a spill, a USER key). */
    cc::Function keyFunction (int key) const;
    /** What key `key` puts on fader 13, if the console has it. */
    std::optional<compact::MainFader> keyMainFader (int key) const;
    /** The strip a fader 13 assignment controls (nullptr for a DCA, OFF or one the console lacks). */
    Strip* mainFaderStrip (const compact::MainFader&) const;
    juce::String keyName (int key) const;
    juce::String lcdState() const;

    UiContext& ctx;
    int stripW = 70;
    std::unique_ptr<SurfaceKey> modeKeys[4];
    ViewKey viewKey;
    juce::OwnedArray<SurfaceKey> keys;
    FaderStrip fader13;
    juce::Rectangle<int> lcd;
    juce::String shownLcd;
    compact::MainFader shownMainFader;
    bool viewOpened = false;
};

} // namespace livedaw::ui
