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
    /** `read(ch)` returns the current linear peak of channel ch. */
    MeterComponent (std::function<float (int)> read, std::function<int()> numChannels);

    /** Draws the dB scale next to the bars. */
    void setShowScale (bool s) { showScale = s; repaint(); }

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
    juce::int64 lastSignature = -1;
    bool showScale = false;
};

} // namespace livedaw::ui
