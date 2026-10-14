#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>

namespace livedaw::ui
{

/** Vertical peak meter (1 or 2 channels) with peak hold and clip indicator. */
class MeterComponent final : public juce::Component, private juce::Timer
{
public:
    /** `readAndReset(ch)` returns the linear peak since the last call. */
    MeterComponent (std::function<float (int)> readAndReset, std::function<int()> numChannels);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override; // clears clip indicators

    static float dbToProportion (float db) noexcept;

private:
    void timerCallback() override;

    std::function<float (int)> reader;
    std::function<int()> channelCount;
    std::array<float, 2> levelDb { -100.0f, -100.0f }, holdDb { -100.0f, -100.0f };
    std::array<int, 2> holdFrames {};
    std::array<bool, 2> clipped {};
};

} // namespace livedaw::ui
