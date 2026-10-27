#include "ui/MeterComponent.h"
#include "dsp/DspUtils.h"
#include "ui/LookAndFeel.h"

namespace livedaw::ui
{

//==============================================================================
namespace leds
{
    int countLit (const float* thresholds, int numSegments, float value) noexcept
    {
        int lit = 0;
        for (int i = numSegments; --i >= 0;)
        {
            if (value < thresholds[i])
                break;
            ++lit;
        }
        return lit;
    }

    static juce::Rectangle<float> segment (juce::Rectangle<float> area, int numSegments, int i)
    {
        const float pitch = area.getHeight() / (float) juce::jmax (1, numSegments);
        const float h = juce::jmax (1.5f, juce::jmin (pitch * 0.62f, area.getWidth() * 0.9f));
        return { area.getX(), area.getY() + pitch * (float) i + (pitch - h) * 0.5f, area.getWidth(), h };
    }

    void drawColumn (juce::Graphics& g, juce::Rectangle<float> area, int numSegments, int lit,
                     const std::function<juce::Colour (int)>& colour)
    {
        for (int i = 0; i < numSegments; ++i)
        {
            g.setColour (i >= numSegments - lit ? colour (i) : surfaceColours().ledOff);
            g.fillRect (segment (area, numSegments, i));
        }
    }

    void drawColumnFromTop (juce::Graphics& g, juce::Rectangle<float> area, int numSegments, int lit, juce::Colour colour)
    {
        for (int i = 0; i < numSegments; ++i)
        {
            g.setColour (i < lit ? colour : surfaceColours().ledOff);
            g.fillRect (segment (area, numSegments, i));
        }
    }

    juce::Colour meterColour (float thresholdDb) noexcept
    {
        if (thresholdDb > -0.5f)
            return Colours::meterRed;
        // The WING's LED meters: -6 to -18 orange, below that green.
        return thresholdDb >= -18.0f ? Colours::meterOrange : Colours::meterGreen;
    }
} // namespace leds

} // namespace livedaw::ui
