#include "dsp/Compressor.h"
#include "dsp/DspUtils.h"

#include <algorithm>
#include <cmath>

namespace livedaw::dsp
{

void Compressor::prepare (double newSampleRate) noexcept
{
    sampleRate = newSampleRate;
    setSettings (settings);
    reset();
}

void Compressor::reset() noexcept
{
    reductionDb = 0.0f;
    lastMaxReduction = 0.0f;
}

void Compressor::setSettings (const Settings& s) noexcept
{
    settings = s;
    attackCoeff = timeConstantCoeff (std::max (0.05f, settings.attackMs), sampleRate);
    releaseCoeff = timeConstantCoeff (std::max (1.0f, settings.releaseMs), sampleRate);
}

float Compressor::computeGainReductionDb (const Settings& s, float inputDb) noexcept
{
    const float ratio = std::max (1.0f, s.ratio);
    const float slope = 1.0f - 1.0f / ratio;
    const float knee = std::max (0.0f, s.kneeDb);
    const float over = inputDb - s.thresholdDb;

    if (knee > 0.0f && 2.0f * std::abs (over) <= knee)
    {
        const float x = over + knee * 0.5f;
        return slope * x * x / (2.0f * knee);
    }

    return over > 0.0f ? slope * over : 0.0f;
}

void Compressor::process (float* const* channels, int numChannels, int numSamples) noexcept
{
    if (! settings.enabled)
    {
        reductionDb = 0.0f;
        lastMaxReduction = 0.0f;
        return;
    }

    float maxReduction = 0.0f;
    const float makeup = settings.makeupDb;

    for (int i = 0; i < numSamples; ++i)
    {
        float level = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
            level = std::max (level, std::abs (channels[ch][i]));

        const float inputDb = 20.0f * std::log10 (std::max (level, 1.0e-6f));
        const float target = computeGainReductionDb (settings, inputDb);
        const float coeff = target > reductionDb ? attackCoeff : releaseCoeff;
        reductionDb = target + coeff * (reductionDb - target);

        const float g = std::pow (10.0f, (makeup - reductionDb) * 0.05f);

        for (int ch = 0; ch < numChannels; ++ch)
            channels[ch][i] *= g;

        maxReduction = std::max (maxReduction, reductionDb);
    }

    lastMaxReduction = maxReduction;
}

} // namespace livedaw::dsp
