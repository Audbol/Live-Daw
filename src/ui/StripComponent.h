#pragma once

#include "ui/Controls.h"
#include "ui/MeterComponent.h"
#include "ui/StripHandles.h"
#include "ui/UiContext.h"

#include <functional>

namespace livedaw::ui
{

/** A console strip on the mixer page: name, patch and width, insert slots, gate,
    mini EQ and compressor handles, pan, mute/solo/select, fader and meter. */
class StripComponent final : public juce::Component, private juce::Timer
{
public:
    static constexpr int kWidth = 90;

    StripComponent (Strip& strip, UiContext& ctx);

    Strip& getStrip() noexcept { return strip; }

    void setSelected (bool shouldBeSelected);
    void setNumInsertRows (int rows);

    std::function<void (Strip&)> onRemove;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    void timerCallback() override;
    void showRoutingMenu();
    void showContextMenu();
    void toggleWidth();
    juce::String routingText() const;

    Strip& strip;
    UiContext& ctx;
    bool selected = false;
    int insertRows = 4;

    juce::Label nameLabel;
    juce::TextButton routeButton, widthButton;
    InsertSlotsHandle inserts;
    GateHandle gate;
    EqHandle eq;
    CompHandle comp;
    ParamSlider pan;
    ParamButton mute, solo;
    juce::TextButton selectButton { "SEL" };
    ParamSlider fader;
    MeterComponent meter;
    juce::Label levelLabel;
};

} // namespace livedaw::ui
