#include "ui/MeterComponent.h"
#include "dsp/DspUtils.h"
#include "ui/LookAndFeel.h"

namespace livedaw::ui
{

MeterComponent::MeterComponent (std::function<float (int)> readAndReset, std::function<int()> numChannels)
    : reader (std::move (readAndReset)), channelCount (std::move (numChannels))
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

    repaint();
}

void MeterComponent::mouseDown (const juce::MouseEvent&)
{
    clipped = { false, false };
}

void MeterComponent::paint (juce::Graphics& g)
{
    const int n = juce::jlimit (1, 2, channelCount());
    auto bounds = getLocalBounds().toFloat();
    const float gap = 2.0f;
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

} // namespace livedaw::ui
