#include "dsp/NoiseGate.h"
#include "dsp/DspUtils.h"

#include <algorithm>
#include <cmath>

namespace livedaw::dsp
{

void NoiseGate::prepare (double newSampleRate) noexcept
{
    sampleRate = newSampleRate;
    updateCoefficients();
    reset();
}

void NoiseGate::reset() noexcept
{
    envelope = 0.0f;
    gain = settings.enabled ? rangeGain : 1.0f;
    holdCounter = 0;
    open = false;
    lastMinGain = 1.0f;
}

void NoiseGate::setSettings (const Settings& s) noexcept
{
    const bool wasEnabled = settings.enabled;
    settings = s;
    updateCoefficients();

    if (wasEnabled != settings.enabled)
        gain = 1.0f; // fade in from unity when switching on; the release takes it down
}

void NoiseGate::updateCoefficients() noexcept
{
    openThreshold = dbToGain (settings.thresholdDb);
    closeThreshold = dbToGain (settings.thresholdDb - std::max (0.0f, settings.hysteresisDb));
    rangeGain = dbToGain (std::min (0.0f, settings.rangeDb));
    attackCoeff = timeConstantCoeff (std::max (0.01f, settings.attackMs), sampleRate);
    releaseCoeff = timeConstantCoeff (std::max (1.0f, settings.releaseMs), sampleRate);
    detectorRelease = timeConstantCoeff (10.0f, sampleRate);
    holdSamples = static_cast<int> (std::max (0.0f, settings.holdMs) * 0.001 * sampleRate);
}

void NoiseGate::process (float* const* channels, int numChannels, int numSamples) noexcept
{
    if (! settings.enabled)
    {
        lastMinGain = 1.0f;
        return;
    }

    float minGain = 1.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        float level = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
            level = std::max (level, std::abs (channels[ch][i]));

        envelope = level > envelope ? level : level + detectorRelease * (envelope - level);

        if (envelope >= openThreshold)
        {
            open = true;
            holdCounter = holdSamples;
        }
        else if (envelope < closeThreshold)
        {
            if (holdCounter > 0)
                --holdCounter;
            else
                open = false;
        }

        const float targetGain = open ? 1.0f : rangeGain;
        const float coeff = targetGain > gain ? attackCoeff : releaseCoeff;
        gain = targetGain + coeff * (gain - targetGain);

        for (int ch = 0; ch < numChannels; ++ch)
            channels[ch][i] *= gain;

        minGain = std::min (minGain, gain);
    }

    lastMinGain = minGain;
}

} // namespace livedaw::dsp
