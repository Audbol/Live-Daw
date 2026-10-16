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

/** The surface's LED meters: a column of square LEDs, top to bottom. */
namespace leds
{
    /** How many of `thresholds` (top to bottom, descending) `value` reaches. */
    int countLit (const float* thresholds, int numSegments, float value) noexcept;

    /** Draws `numSegments` LEDs into `area`, the bottom `lit` of them lit in
        `colour (segment)` (segment 0 = top). */
    void drawColumn (juce::Graphics&, juce::Rectangle<float> area, int numSegments, int lit,
                     const std::function<juce::Colour (int)>& colour);

    /** Draws a column lit from the top (gain reduction). */
    void drawColumnFromTop (juce::Graphics&, juce::Rectangle<float> area, int numSegments, int lit, juce::Colour colour);

    /** The usual meter colours: red at clip, yellow from -6 dB, else green. */
    juce::Colour meterColour (float thresholdDb) noexcept;
} // namespace leds

} // namespace livedaw::ui
