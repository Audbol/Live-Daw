#include "dsp/ParametricEq.h"
#include "dsp/DspUtils.h"

#include <algorithm>
#include <cmath>

namespace livedaw::dsp
{

void ParametricEq::prepare (double newSampleRate) noexcept
{
    sampleRate = newSampleRate;
    // ~20 ms glide, evaluated once per sub-block.
    smoothingCoeff = static_cast<float> (std::exp (-static_cast<double> (kSubBlock) / (0.02 * sampleRate)));
    reset();
}

void ParametricEq::reset() noexcept
{
    for (auto& f : filters)
        f.reset();

    needsSnap = true;
}

BiquadCoeffs ParametricEq::designBand (const Band& band, double sr) noexcept
{
    if (! band.enabled)
        return BiquadCoeffs::identity();

    switch (band.type)
    {
        case BandType::LowShelf:  return BiquadCoeffs::lowShelf (sr, band.frequency, band.q, band.gainDb);
        case BandType::HighShelf: return BiquadCoeffs::highShelf (sr, band.frequency, band.q, band.gainDb);
        case BandType::LowCut:    return BiquadCoeffs::highPass (sr, band.frequency, band.q);
        case BandType::HighCut:   return BiquadCoeffs::lowPass (sr, band.frequency, band.q);
        case BandType::Bell:
        default:                  return BiquadCoeffs::peak (sr, band.frequency, band.q, band.gainDb);
    }
}

double ParametricEq::responseDb (const Settings& settings, double freq, double sr) noexcept
{
    if (! settings.enabled)
        return 0.0;

    double mag = 1.0;

    for (const auto& band : settings.bands)
        if (band.enabled)
            mag *= designBand (band, sr).magnitudeAt (freq, sr);

    return 20.0 * std::log10 (std::max (mag, 1.0e-6));
}

void ParametricEq::updateCoefficients (bool snap) noexcept
{
    const float k = snap ? 0.0f : smoothingCoeff;

    for (int b = 0; b < kNumBands; ++b)
    {
        const auto& t = target.bands[(size_t) b];
        auto& c = current[(size_t) b];

        const float targetFreq = std::max (10.0f, t.frequency);
        const float targetQ = std::max (0.025f, t.q);

        // Smooth frequency and Q in the log domain so sweeps sound even.
        c.freq = std::exp (std::log (targetFreq) + k * (std::log (std::max (10.0f, c.freq)) - std::log (targetFreq)));
        c.q = std::exp (std::log (targetQ) + k * (std::log (std::max (0.025f, c.q)) - std::log (targetQ)));
        c.gainDb = t.gainDb + k * (c.gainDb - t.gainDb);

        // Snap once close enough, so a settled band stops being redesigned.
        if (std::abs (c.freq - targetFreq) < 1.0e-4f * targetFreq) c.freq = targetFreq;
        if (std::abs (c.q - targetQ) < 1.0e-4f) c.q = targetQ;
        if (std::abs (c.gainDb - t.gainDb) < 1.0e-3f) c.gainDb = t.gainDb;

        Band smoothed = t;
        smoothed.frequency = c.freq;
        smoothed.q = c.q;
        smoothed.gainDb = c.gainDb;

        auto& last = designed[(size_t) b];
        if (! snap && last.type == smoothed.type && last.enabled == smoothed.enabled
            && last.frequency == smoothed.frequency && last.q == smoothed.q && last.gainDb == smoothed.gainDb)
            continue;

        filters[(size_t) b].setCoefficients (designBand (smoothed, sampleRate));
        last = smoothed;
    }
}

void ParametricEq::process (float* const* channels, int numChannels, int numSamples) noexcept
{
    if (! target.enabled)
    {
        needsSnap = true;
        return;
    }

    numChannels = std::min (numChannels, Biquad::kMaxChannels);

    for (int start = 0; start < numSamples; start += kSubBlock)
    {
        const int n = std::min (kSubBlock, numSamples - start);

        updateCoefficients (needsSnap);
        needsSnap = false;

        for (int b = 0; b < kNumBands; ++b)
        {
            const auto& band = target.bands[(size_t) b];

            if (! band.enabled)
                continue;

            // A flat bell or shelf is an identity filter; skip it to save CPU.
            const bool isCut = band.type == BandType::LowCut || band.type == BandType::HighCut;
            if (! isCut && std::abs (current[(size_t) b].gainDb) < 1.0e-3f && std::abs (band.gainDb) < 1.0e-3f)
            {
                filters[(size_t) b].reset();
                continue;
            }

            for (int ch = 0; ch < numChannels; ++ch)
                filters[(size_t) b].process (channels[ch] + start, ch, n);
        }
    }
}

} // namespace livedaw::dsp
