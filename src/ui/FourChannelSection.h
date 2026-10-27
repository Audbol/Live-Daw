#pragma once

#include "ui/CustomControlsPage.h"
#include "ui/Knobs.h"
#include "ui/UiContext.h"
#include "ui/WingWidgets.h"

namespace livedaw::ui
{

/** The WING's 4-Channel section (top right, manual 4.6, fig. 16, table 4.8): a
    second operator's quick access to the four strips on the right fader
    section, without selecting them. Eight mode keys (GAIN / 48 V, GATE, COMP,
    FILTER, F1, F2, F3, PAN / MAIN), a four-way LCD with the strips' scribble
    strips, and four encoders each over a key:

    | mode   | encoder                          | key            |
    |--------|----------------------------------|----------------|
    | GAIN   | input trim (no preamps here)     | source's 48 V  |
    | GATE   | gate threshold                   | gate on        |
    | COMP   | compressor threshold             | compressor on  |
    | FILTER | low cut (press again: high cut)  | low cut on     |
    | F1-F3  | send level to the key's bus      | send on        |
    | PAN    | pan (press again: width)         | MAIN 1 on      |

    The 48 V keys switch only when held for 2 s, as the console's. Holding
    F1-F3 picks the bus it sends to. With USE F1-F3 AS CUSTOM CONTROLS
    (SETUP > SURFACE) they mirror the custom control layers 1-3 instead (WING
    manual 5.5 p. 48: "F1 opens the 4 knobs and top row of 4 buttons of the
    first layer"): the encoders and upper keys of the CC section's layers,
    and a key held lets its encoder set what it switches (FX PAR, 4.8). */
class FourChannelSection final : public juce::Component, private juce::Timer
{
public:
    /** `stripAt (i)`: the strip on the right fader section's fader i (0-3), or nullptr. */
    FourChannelSection (UiContext&, std::function<Strip* (int)> stripAt);
    ~FourChannelSection() override;

    /** The right section's faders or the selection changed. */
    void refresh();

    void paint (juce::Graphics&) override;
    void resized() override;

    enum Mode
    {
        Gain,
        Gate,
        Comp,
        Filter,
        F1,
        F2,
        F3,
        Pan,
        kNumModes
    };

private:
    void timerCallback() override;
    void setMode (int mode);
    void bind();
    int busOf (int fKey) const;
    void chooseBus (int fKey);
    juce::String modeCaption() const;
    /** USE F1-F3 AS CUSTOM CONTROLS in an F mode: the control on encoder i of
        custom control layer 1-3, else nullptr. */
    const cc::Control* customControl (int i) const;

    UiContext& ctx;
    std::function<Strip* (int)> stripAt;
    int mode = Gain;
    bool alternate = false; // FILTER: high cut, PAN: width
    juce::OwnedArray<SurfaceKey> modeKeys, channelKeys;
    /** A custom control key held (its encoder sets the key's parameter), or -1. */
    int heldKey = -1;
    DisplayKnob knobs[4];
    std::array<Strip*, 4> strips {};
    juce::Rectangle<int> panel, keyPanel, lcd, knobPanel, heading;
    juce::String shownState;
    int shownCcRevision = -1;
};

} // namespace livedaw::ui
