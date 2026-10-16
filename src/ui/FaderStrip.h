#pragma once

#include "ui/Knobs.h"
#include "ui/MeterComponent.h"
#include "ui/UiContext.h"
#include "ui/WingWidgets.h"

#include <optional>

namespace livedaw::ui
{

/** What a scribble strip LCD shows (manual 4.4, fig. 14): the number large
    ("CH29"), the source on two small lines ("IN 01" / "IN 02", or "mono"), the
    icon, and the name on an inverted bar. */
struct ScribbleInfo
{
    juce::String number, line2, line3, name;
    std::optional<Icon> icon;
    juce::Colour line2Colour = Colours::textDim;
};

/** Draws a scribble strip LCD into `lcd`. */
void drawScribble (juce::Graphics&, juce::Rectangle<float> lcd, const ScribbleInfo&);

/** Draws the colour light bar above a scribble strip (dark when off). */
void drawLightBar (juce::Graphics&, juce::Rectangle<float> bar, juce::Colour, bool on);

/** One fader strip of a WING fader section, top to bottom: the colour light
    bar and scribble strip, SELECT, SOLO, the meter block (DYN LED, a stereo
    6-segment input meter, GATE LED), MUTE and the fader with its printed scale.
    It controls a strip, a DCA, or (Sends on Fader) a send level between the
    selected strip and this one: a bus, main or matrix send. */
class FaderStrip final : public juce::Component, private juce::Timer
{
public:
    explicit FaderStrip (UiContext&);

    /** What the strip controls (nullopt = an empty position). */
    void setTarget (std::optional<state::FaderRef>);
    /** Re-resolves the target (after structure, selection or SOF changes). */
    void refresh();

    /** The meter block's LED rows (DYN, CLIP .. -60, GATE), for the section
        to print the scale between strips. */
    juce::Rectangle<int> getMeterArea() const noexcept { return meterArea; }
    /** The colour bar and LCD, for the section's frame around four strips. */
    juce::Rectangle<int> getScribbleArea() const noexcept { return scribbleArea; }

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

    void timerCallback() override;
    void bindControls();
    void pressSelect();
    std::atomic<float>* faderParam() const noexcept;
    ScribbleInfo scribbleInfo() const;

    UiContext& ctx;
    std::optional<state::FaderRef> target;
    Strip* strip = nullptr;
    int dca = -1;
    std::optional<SendTarget> send;

    juce::Slider fader;
    SurfaceKey selectKey { "SELECT" }, soloKey { "SOLO" }, muteKey { "MUTE" };
    juce::Rectangle<int> scribbleArea, barArea, lcdArea, meterArea, valueArea;
    bool showValue = false, moduleFirst = false, moduleLast = false;

    // What's drawn, to repaint only on changes.
    juce::String shownName, shownValue, shownSource;
    std::array<int, 2> shownLit {};
    bool shownGate = false, shownDyn = false, shownLight = false;
    int shownColour = 0;
};

} // namespace livedaw::ui
