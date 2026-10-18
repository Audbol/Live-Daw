#pragma once

#include "ui/FaderStrip.h"
#include "ui/SurfaceSections.h"

namespace livedaw::ui
{

/** A fader section in DAW mode (WING DAW Documentation: "this will change the
    CC- and the Fader-Section to DAW-Mode"): it lies over the console's fader
    section while DAW mode is on and the CONFIGURATION gives the DAW faders.

    - The strips are the emulated controllers' channels: left section 1-12,
      center 13-20, right 21-24 (MCU 1-8, extender 1 9-16, extender 2 17-24),
      each with the DAW's scribble text, SELECT, SOLO and MUTE (with the
      special keys of the DAW layers), its meter and a motor fader.
    - The layer keys become the controller's keys as the documentation lists
      them: VIEW = NAME/VALUE (HUI: PAN), the five layer keys = TRACK / PAN /
      SEND / EQ / PLUG-IN (HUI: SEND A-E), USER 1 / USER 2 = bank of 8 < / >,
      ◄4 / 4► = channel < / >, SOF FLIP = FLIP (HUI: SEND A).

    It shows and hides itself (it polls DAW mode); the owner only places it
    over its section and passes the section's strip count. */
class DawFaderSection final : public juce::Component, private juce::Timer
{
public:
    DawFaderSection (UiContext&, LayerSet::Section);
    ~DawFaderSection() override;

    /** The console section's number of strips (the DAW strips line up with them). */
    void setNumStrips (int);

    /** The DAW channel of strip `index` (-1: none). */
    int channelOf (int index) const noexcept;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class DawStrip;

    void timerCallback() override;
    void rebuildKeys();
    /** The DAW BUTTONS command of a layer column key (0-6 the layer keys,
        7 / 8 the shift keys, 9 VIEW, 10 SOF FLIP). */
    int commandOf (int key) const;

    UiContext& ctx;
    const LayerSet::Section section;
    juce::OwnedArray<DawStrip> strips;
    ViewKey viewKey;
    juce::OwnedArray<TalkKey> layerKeys; // the seven layer keys, ◄4, 4►, SOF FLIP (center)
    juce::Rectangle<int> column;
    bool wasHui = false;
};

} // namespace livedaw::ui
