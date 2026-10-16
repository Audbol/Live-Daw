#pragma once

#include "ui/Knobs.h"
#include "ui/MeterComponent.h"
#include "ui/UiContext.h"
#include "ui/WingWidgets.h"

namespace livedaw::ui
{

/** The WING's channel strip section (top left, manual 4.5) for the selected
    strip: the CHANNEL STRIP DETAILS LED panel with LEVEL / GR meters, bus sends
    and groups around a small screen; CHANNEL PROCESSING (six encoders over
    INPUT - FILTER - GATE - COMP - INSERT - MAIN); CHANNEL EQUALIZER (SHIFT,
    four band encoders, BLEND/MIX, EQ, GAIN / WIDTH / FREQ, LISTEN) and VIEW.

    A slot key shows its slot on the small screen; pressed again it steps the
    encoder above it through the slot's parameters; held, it switches the slot
    on or off. Touching an encoder shows its slot too. */
class ChannelStripSection final : public juce::Component, private juce::Timer
{
public:
    explicit ChannelStripSection (UiContext&);

    /** The selected strip changed. */
    void refresh();

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    enum Slot
    {
        Input,
        Filter,
        Gate,
        Comp,
        Insert,
        Main,
        kNumSlots,
        EqSlot = kNumSlots
    };

    void timerCallback() override;
    std::vector<KnobBinding> slotParams (int slot) const;
    void bindSlot (int slot);
    void pressSlot (int slot);
    void showSlot (int slot);
    void bindEq();
    bool slotOn (int slot) const;
    void toggleSlot (int slot);
    juce::String panelState() const;
    void paintPanel (juce::Graphics&);
    void paintScreen (juce::Graphics&, juce::Rectangle<float>);

    UiContext& ctx;
    Strip* strip = nullptr;

    DisplayKnob slotKnobs[kNumSlots];
    std::unique_ptr<SurfaceKey> slotKeys[kNumSlots];
    int paramIndex[kNumSlots] {};
    int numParams[kNumSlots] {};
    int boundModels[4] { -2, -2, -2, -2 }; // filter tool, gate, comp, insert FX the slots are bound to
    int shownSlot = Input;

    DisplayKnob eqKnobs[4], eqMix;
    SurfaceKey shiftKey, eqKey { "EQ" }, listenKey { "LISTEN" };
    SurfaceKey gainKey { {}, Icon::GainArrows }, widthKey { {}, Icon::WidthArrows }, freqKey { {}, Icon::FreqMarker };
    ViewKey viewKey;
    int eqElement = 0; // 0 gain, 1 width (Q), 2 freq
    int eqBand = 0;    // the band knob last touched
    bool eqShift = false, eqMixShown = false, viewOpened = false;
    int eqModel = -1;

    juce::Rectangle<int> panelArea, screenArea, processingHeading, eqHeading, legendLow, legendHigh;
    juce::String shownState;
};

/** The USB AUDIO/DATA and MONITORING/TALKBACK panel (top right, manual 4.7):
    VIEW, the USB socket plate, TALK / PHONES / MONITOR LEVEL and the TALK A,
    TALK B, DIM and MONO keys. */
class MonitorSection final : public juce::Component
{
public:
    explicit MonitorSection (UiContext&);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    UiContext& ctx;
    ViewKey viewKey;
    DisplayKnob talkLevel, phonesLevel, monitorLevel;
    SurfaceKey talkA { "A" }, talkB { "B" }, dimKey { "DIM" }, monoKey { {}, Icon::Circle };
    juce::Rectangle<int> wingPanel, usbPlate, monitorTitle, usbTitle;
    bool viewOpened = false;
};

/** The CUSTOM CONTROLS section between the center and right fader sections
    (manual 4.8, fig. 20). Upper part: VIEW, a four-way LCD, the layer display,
    four encoders and eight keys on 16 layers (UP / DOWN), empty until custom
    controls can be assigned. Lower part: the layer keys USER, 2-TRACK TRANSP,
    MULTI TRANSP, MUTE GROUPS (the eight mute groups) and SHOW CNTRL (scene
    back / GO / SCENES), VIEW, a four-way LCD, DAW and eight keys. At the
    bottom: the navigation arrows and the scroll wheel. */
class CustomControlSection final : public juce::Component, private juce::Timer
{
public:
    explicit CustomControlSection (UiContext&);

    /** Scenes changed. */
    void refresh();

    /** Only the lower part (layer keys, LCD and eight keys), for windows too
        narrow to give the section its place between the fader sections. */
    void setCompact (bool);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    enum LowerLayer
    {
        User,
        TwoTrack,
        MultiTrack,
        MuteGroups,
        ShowControl,
        kNumLowerLayers
    };

    void timerCallback() override;
    void setLowerLayer (int);
    juce::String lowerKeyName (int key) const;
    void pressLowerKey (int key);
    bool lowerKeyLit (int key) const;
    void paintLcd (juce::Graphics&, juce::Rectangle<int>, bool colourBars, bool upper);

    UiContext& ctx;
    int upperLayer = 0, lowerLayer = MuteGroups;
    bool viewOpened = false, compact = false;

    ViewKey upperView, lowerView;
    SurfaceKey upKey { "UP" }, downKey { "DOWN" }, dawKey { "DAW" };
    DisplayKnob encoders[4], wheel;
    juce::OwnedArray<SurfaceKey> upperKeys, lowerKeys, layerKeys;
    SurfaceKey navUp, navDown, navLeft, navRight;
    juce::Rectangle<int> upperLcd, lowerLcd, layerDisplay, heading, divider;
    juce::String shownState;
};

} // namespace livedaw::ui
