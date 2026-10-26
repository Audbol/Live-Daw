#pragma once

#include "ui/CustomControlsPage.h"
#include "ui/FaderStrip.h"
#include "ui/SurfaceLayout.h"

namespace livedaw::ui
{

namespace compact = layout::compact;

/** The WING COMPACT's key section, right of its fader section: BUS, DCA,
    MAIN MATRIX and USER choose what the 16 keys do (/$ctl/cfg/cscmode), VIEW,
    the small LCD naming the keys, the 16 keys and fader 13 (the main fader,
    /$ctl/cfg/mfdr; MAIN 1 unless the LCD's title is touched to pick another).

    The keys (WING remote protocols: busspill, mainspill, dcaspill at their
    defaults):
    - BUS: BUS 1-16; a press puts the bus's sends on the faders (Sends on
      Fader to it), holding it spills the sources sending to it.
    - MAIN MATRIX: MAIN 1-4 on keys 1-4, MATRIX 1-8 on keys 5-12; a press puts
      their sends on the faders.
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
    void mouseUp (const juce::MouseEvent&) override;

private:
    void timerCallback() override;
    compact::KeyMode keyMode() const;
    void setKeyMode (compact::KeyMode);
    /** Key `key`'s functions in the current mode: on a press and on a hold. */
    std::pair<cc::Function, cc::Function> keyFunctions (int key) const;
    juce::String keyName (int key) const;
    void showMainFaderMenu();
    juce::String lcdState() const;

    UiContext& ctx;
    int stripW = 70;
    std::unique_ptr<SurfaceKey> modeKeys[4];
    ViewKey viewKey;
    juce::OwnedArray<SurfaceKey> keys;
    FaderStrip fader13;
    juce::Rectangle<int> lcd;
    juce::String shownLcd;
    bool viewOpened = false;
};

} // namespace livedaw::ui
