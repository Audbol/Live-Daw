#pragma once

#include "ui/Knobs.h"
#include "ui/MeterComponent.h"
#include "ui/UiContext.h"

#include <optional>

namespace livedaw::ui
{

/** One fader strip of a WING fader section: colour bar and scribble strip
    (number, name, icon), SELECT, SOLO, MUTE, a stereo meter with the GATE and
    DYN LEDs, and the fader. It controls a strip, a DCA, or (Sends on Fader)
    a send level between the selected strip and this one. */
class FaderStrip final : public juce::Component, private juce::Timer
{
public:
    explicit FaderStrip (UiContext&);

    /** What the strip controls (nullopt = an empty position). */
    void setTarget (std::optional<state::FaderRef>);
    /** Re-resolves the target (after structure, selection or SOF changes). */
    void refresh();

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    void timerCallback() override;
    void bindControls();
    std::atomic<float>* faderParam() const noexcept;

    UiContext& ctx;
    std::optional<state::FaderRef> target;
    Strip* strip = nullptr;
    int dca = -1;

    // Sends on Fader: the send from `sendSource` to bus slot `sendSlot`.
    Strip* sendSource = nullptr;
    int sendSlot = -1;

    juce::Slider fader;
    juce::TextButton selButton { "SEL" }, soloButton { "SOLO" }, muteButton { "MUTE" };
    MeterComponent meter;
    juce::Rectangle<int> scribbleArea, ledArea, valueArea;
    juce::String shownName, shownValue;
    bool shownSelected = false, shownGate = false, shownDyn = false;
};

} // namespace livedaw::ui
