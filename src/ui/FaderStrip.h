#pragma once

#include "ui/Knobs.h"
#include "ui/MeterComponent.h"
#include "ui/Scribble.h"
#include "ui/UiContext.h"
#include "ui/WingWidgets.h"

#include <optional>

namespace livedaw::ui
{

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
    /** The shortest fader travel: shorter strips fold their meter away first. */
    static constexpr int kMinFaderTravel = 100;
    /** About the height of the narrowest strip with its meter folded away. */
    static constexpr int kMinCompactHeight = 250;
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
    /** Whether the strip controls anything (else it shows nothing: blankScribble()). */
    bool hasControl() const noexcept;
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
