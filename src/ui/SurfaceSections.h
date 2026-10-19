#pragma once

#include "ui/CustomControlsPage.h"
#include "ui/Knobs.h"
#include "ui/MeterComponent.h"
#include "ui/UiContext.h"
#include "ui/WingWidgets.h"

namespace livedaw::ui
{

/** A knob without end stops (the wheel, custom control encoders): `turned`
    gets whole steps, `stepsPerTurn` for the knob's full travel. `position` is
    the shown position; set it back to 0.5 between turns. */
inline KnobBinding endlessBinding (const juce::String& label, std::shared_ptr<float> position, std::function<void (int steps)> turned,
                                   int stepsPerTurn = 48)
{
    KnobBinding b;
    b.label = label;
    auto pending = std::make_shared<float> (0.0f);
    b.get = [position] { return *position; };
    b.set = [position, pending, turned, stepsPerTurn] (float v)
    {
        *pending += (v - *position) * (float) stepsPerTurn;
        *position = v;
        if (const int steps = (int) *pending; steps != 0 && turned)
        {
            *pending -= (float) steps;
            turned (steps);
        }
    };
    return b;
}

/** The WING's channel strip section (right of the display, manual 4.5) for the selected
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
    /** SCREEN FOLLOWS CH STRIP / CH STRIP TOUCH SELECT: the slot on HOME. */
    void followScreen (int slot, bool touch);
    static HomeTab homeTabOf (int slot);
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

/** A TALK key: reports presses and releases, since the talk path's SWITCH
    MODE (AUTO, PUSH, LATCH) needs both. */
class TalkKey final : public SurfaceKey
{
public:
    using SurfaceKey::SurfaceKey;
    std::function<void (bool down)> onTalk;

    void mouseDown (const juce::MouseEvent& e) override
    {
        SurfaceKey::mouseDown (e);
        if (isEnabled() && onTalk != nullptr)
            onTalk (true);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        SurfaceKey::mouseUp (e);
        if (isEnabled() && onTalk != nullptr)
            onTalk (false);
    }
};

/** The USB AUDIO/DATA and MONITORING/TALKBACK panel (top left under the WING
    logo, manual 4.7; top right in narrow windows):
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
    TalkKey talkA { "A" }, talkB { "B" };
    SurfaceKey dimKey { "DIM" }, monoKey { {}, Icon::Circle };
    juce::Rectangle<int> logoArea, wingPanel, usbPlate, monitorTitle, usbTitle;
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
    bool useCursorWheel() const;
    void turnWheel (int steps);
    /** The custom control layer of the upper part (CC EDIT edits the same one). */
    int currentLayer() const;
    void stepLayer (int delta);
    /** Key `key` of a custom control layer: 0-3 the upper keys, 4-7 the lower ones. */
    cc::Function ccFunction (int layer, int key) const;
    /** Binds the encoders and keys to the assignments. */
    void bindCustomControls();
    /** The scene GO recalls next (the wheel on SHOW CNTRL moves it). */
    int nextScene() const;
    /** The custom control layer the lower part shows: USER, or DAW 1-4 on
        the layer keys in DAW mode (-1: none). */
    int lowerCcLayer() const;
    /** DAW mode with USE UPPER CC FOR DAW and a DAW layer: the upper part is
        four V-pots of the DAW (midiui). */
    bool upperIsDaw() const;
    /** The layer keys read DAW 1-4 in DAW mode. */
    void applyDawMode();

    UiContext& ctx;
    int lowerLayer = MuteGroups;
    bool viewOpened = false, compact = false;
    std::shared_ptr<float> wheelPos = std::make_shared<float> (0.5f);
    int sceneCursor = -1; // -1: the scene after the current one
    int shownCcRevision = -1, shownCcLayer = -1;
    Strip* shownSelection = nullptr;
    juce::String shownValues;
    bool shownDaw = false;
    int shownDawRevision = -1;
    juce::StringArray layerLabels;       // the layer keys' own labels (DAW mode relabels them)
    std::array<bool, 5> layerKeysEnabled {};

    ViewKey upperView, lowerView;
    SurfaceKey upKey { "UP" }, downKey { "DOWN" }, dawKey { "DAW" };
    DisplayKnob encoders[4], wheel;
    juce::OwnedArray<SurfaceKey> upperKeys, lowerKeys, layerKeys;
    SurfaceKey navUp, navDown, navLeft, navRight;
    juce::Rectangle<int> upperLcd, lowerLcd, layerDisplay, heading, divider, frame;
    juce::String shownState;
};

} // namespace livedaw::ui
