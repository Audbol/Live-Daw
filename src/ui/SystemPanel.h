#pragma once

#include "ui/Controls.h"
#include "ui/UiContext.h"

namespace livedaw::ui
{

/** Stability monitor: output protection, fault counters, device health and the
    event log. */
class SystemPanel final : public juce::Component,
                          private juce::Timer,
                          private juce::ChangeListener,
                          private juce::ListBoxModel
{
public:
    SystemPanel (UiContext&, juce::AudioDeviceManager&);
    ~SystemPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;

    UiContext& ctx;
    juce::AudioDeviceManager& deviceManager;

    ParamButton protection;
    ParamSlider ceiling;
    juce::Label ceilingLabel, stats;
    juce::TextButton clearFaultsButton { "Clear plugin faults" };
    juce::ListBox logList;

    juce::Rectangle<int> leftArea, rightArea;
};

} // namespace livedaw::ui
