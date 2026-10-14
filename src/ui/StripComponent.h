#pragma once

#include "engine/MixerEngine.h"
#include "ui/Controls.h"
#include "ui/MeterComponent.h"

#include <functional>

namespace livedaw::ui
{

/** A narrow console channel strip: name, patch, pan, mute/solo/select, fader and meter. */
class StripComponent final : public juce::Component, private juce::Timer
{
public:
    static constexpr int kWidth = 88;

    StripComponent (Strip& strip, MixerEngine& engine);

    Strip& getStrip() noexcept { return strip; }

    void setSelected (bool shouldBeSelected);

    std::function<void (Strip&)> onSelect;
    std::function<void (Strip&)> onRemove;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    void timerCallback() override;
    void showRoutingMenu();
    void showContextMenu();
    juce::String routingText() const;

    Strip& strip;
    MixerEngine& engine;
    bool selected = false;

    juce::Label nameLabel;
    juce::TextButton routeButton;
    ParamSlider pan;
    ParamButton mute, solo;
    juce::TextButton selectButton { "SEL" };
    ParamSlider fader;
    MeterComponent meter;
    juce::Label levelLabel;

    float shownGateGain = 1.0f, shownReduction = 0.0f;
    juce::Rectangle<int> indicatorArea;
};

} // namespace livedaw::ui
