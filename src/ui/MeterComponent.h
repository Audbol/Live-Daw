#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace livedaw::ui
{

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

    /** The surface LED colours: red at clip, orange from -18 dB, else green. */
    juce::Colour meterColour (float thresholdDb) noexcept;
} // namespace leds

} // namespace livedaw::ui
