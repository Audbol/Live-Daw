#include "ui/MeterComponent.h"
#include "dsp/DspUtils.h"
#include "ui/LookAndFeel.h"

namespace livedaw::ui
{

MeterComponent::MeterComponent (std::function<float (int)> read, std::function<int()> numChannels)
    : reader (std::move (read)), channelCount (std::move (numChannels))
{
    startTimerHz (30);
}

float MeterComponent::dbToProportion (float db) noexcept
{
    // -60 dB .. +6 dB, with more resolution near the top.
    const float p = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 66.0f);
    return std::pow (p, 1.6f);
}

void MeterComponent::timerCallback()
{
    const int n = juce::jlimit (1, 2, channelCount());

    for (int ch = 0; ch < 2; ++ch)
    {
        const float peak = ch < n ? reader (ch) : 0.0f;
        const float db = dsp::gainToDb (peak);

        // Fast attack, ~20 dB/s fall.
        levelDb[(size_t) ch] = db > levelDb[(size_t) ch] ? db : juce::jmax (db, levelDb[(size_t) ch] - 0.7f);

        if (db >= holdDb[(size_t) ch])
        {
            holdDb[(size_t) ch] = db;
            holdFrames[(size_t) ch] = 45;
        }
        else if (--holdFrames[(size_t) ch] <= 0)
        {
            holdDb[(size_t) ch] = juce::jmax (-100.0f, holdDb[(size_t) ch] - 1.0f);
        }

        if (peak >= 1.0f)
            clipped[(size_t) ch] = true;
    }

    // Only repaint when what's drawn changes (quantised to 0.25 dB): idle meters cost nothing.
    juce::int64 signature = n;
    for (int ch = 0; ch < 2; ++ch)
        signature = signature * 1000003 + juce::roundToInt (levelDb[(size_t) ch] * 4.0f) * 7919
                    + juce::roundToInt (holdDb[(size_t) ch] * 4.0f) * 31 + (clipped[(size_t) ch] ? 1 : 0);

    if (signature != lastSignature)
    {
        lastSignature = signature;
        repaint();
    }
}

void MeterComponent::mouseDown (const juce::MouseEvent&)
{
    clipped = { false, false };
    repaint();
}

void MeterComponent::paint (juce::Graphics& g)
{
    const int n = juce::jlimit (1, 2, channelCount());
    auto bounds = getLocalBounds().toFloat();
    const float gap = 2.0f;

    if (showScale && bounds.getWidth() > 30.0f)
    {
        auto scale = bounds.removeFromRight (18.0f);
        const float top = scale.getY() + 6.0f, height = scale.getHeight() - 6.0f;
        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (9.0f));
        for (float db : { 0.0f, -6.0f, -12.0f, -18.0f, -30.0f, -40.0f, -60.0f })
        {
            const float y = top + height * (1.0f - dbToProportion (db));
            g.drawText (juce::String ((int) -db), juce::Rectangle<float> (scale.getX(), y - 5.0f, scale.getWidth(), 10.0f),
                        juce::Justification::centred);
        }
    }
    const float barWidth = (bounds.getWidth() - gap * static_cast<float> (n - 1)) / static_cast<float> (n);

    for (int ch = 0; ch < n; ++ch)
    {
        auto bar = juce::Rectangle<float> (bounds.getX() + static_cast<float> (ch) * (barWidth + gap), bounds.getY(),
                                           barWidth, bounds.getHeight());
        auto clip = bar.removeFromTop (5.0f);
        bar.removeFromTop (1.0f);

        g.setColour (clipped[(size_t) ch] ? Colours::meterRed : juce::Colours::black.withAlpha (0.5f));
        g.fillRect (clip);

        g.setColour (juce::Colours::black.withAlpha (0.5f));
        g.fillRect (bar);

        const float h = bar.getHeight() * dbToProportion (levelDb[(size_t) ch]);
        auto lit = bar.withTop (bar.getBottom() - h);

        juce::ColourGradient grad (Colours::meterRed, bar.getX(), bar.getY(),
                                   Colours::meterGreen, bar.getX(), bar.getBottom(), false);
        grad.addColour (1.0 - dbToProportion (0.0f), Colours::meterRed);
        grad.addColour (1.0 - dbToProportion (-6.0f), Colours::meterYellow);
        grad.addColour (1.0 - dbToProportion (-18.0f), Colours::meterGreen);
        g.setGradientFill (grad);
        g.fillRect (lit);

        if (holdDb[(size_t) ch] > -60.0f)
        {
            const float y = bar.getBottom() - bar.getHeight() * dbToProportion (holdDb[(size_t) ch]);
            g.setColour (Colours::text);
            g.fillRect (bar.getX(), y, bar.getWidth(), 1.5f);
        }
    }
}

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
            g.setColour (i >= numSegments - lit ? colour (i) : Colours::ledOff);
            g.fillRect (segment (area, numSegments, i));
        }
    }

    void drawColumnFromTop (juce::Graphics& g, juce::Rectangle<float> area, int numSegments, int lit, juce::Colour colour)
    {
        for (int i = 0; i < numSegments; ++i)
        {
            g.setColour (i < lit ? colour : Colours::ledOff);
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
