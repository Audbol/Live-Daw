#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>

namespace livedaw::dsp
{

constexpr double kPi = 3.14159265358979323846;

/** Anything at or below this level is treated as silence (-inf dB). */
constexpr float kMinusInfDb = -100.0f;

inline float dbToGain (float db) noexcept
{
    return db <= kMinusInfDb ? 0.0f : std::pow (10.0f, db * 0.05f);
}

inline float gainToDb (float gain) noexcept
{
    return gain > 0.0f ? std::max (kMinusInfDb, 20.0f * std::log10 (gain)) : kMinusInfDb;
}

/** One-pole smoothing coefficient for a time constant in milliseconds. */
inline float timeConstantCoeff (float ms, double sampleRate) noexcept
{
    if (ms <= 0.0f || sampleRate <= 0.0)
        return 0.0f;

    return static_cast<float> (std::exp (-1.0 / (0.001 * ms * sampleRate)));
}

/** Lock-free "max since last read" update, used for meters written by the audio thread. */
inline void atomicMax (std::atomic<float>& target, float value) noexcept
{
    auto current = target.load (std::memory_order_relaxed);

    while (value > current
           && ! target.compare_exchange_weak (current, value, std::memory_order_relaxed))
    {
    }
}

/** Multiplies a block by a gain that moves linearly from `startGain` to `endGain`. */
inline void applyGainRamp (float* data, int numSamples, float startGain, float endGain) noexcept
{
    if (startGain == endGain)
    {
        if (startGain == 1.0f)
            return;

        for (int i = 0; i < numSamples; ++i)
            data[i] *= startGain;

        return;
    }

    const float step = (endGain - startGain) / static_cast<float> (std::max (1, numSamples));
    float g = startGain;

    for (int i = 0; i < numSamples; ++i)
    {
        g += step;
        data[i] *= g;
    }
}

/** dst += src * (gain ramping linearly from startGain to endGain). */
inline void addWithGainRamp (float* dst, const float* src, int numSamples, float startGain, float endGain) noexcept
{
    if (startGain == 0.0f && endGain == 0.0f)
        return;

    if (startGain == endGain)
    {
        for (int i = 0; i < numSamples; ++i)
            dst[i] += src[i] * startGain;

        return;
    }

    const float step = (endGain - startGain) / static_cast<float> (std::max (1, numSamples));
    float g = startGain;

    for (int i = 0; i < numSamples; ++i)
    {
        g += step;
        dst[i] += src[i] * g;
    }
}

/** True if the block contains no NaN or infinity. */
inline bool allFinite (const float* data, int numSamples) noexcept
{
    // Multiplying by zero turns any inf/NaN into NaN, and the sum stays NaN.
    float acc = 0.0f;

    for (int i = 0; i < numSamples; ++i)
        acc += data[i] * 0.0f;

    return acc == 0.0f;
}

inline float peakLevel (const float* data, int numSamples) noexcept
{
    float peak = 0.0f;

    for (int i = 0; i < numSamples; ++i)
        peak = std::max (peak, std::abs (data[i]));

    return peak;
}

/** Constant-power (-3 dB centre) pan law for a mono source. pan is -1 (L) .. +1 (R). */
inline void constantPowerPan (float pan, float& left, float& right) noexcept
{
    const auto angle = (std::clamp (pan, -1.0f, 1.0f) + 1.0f) * static_cast<float> (kPi) * 0.25f;
    left = std::cos (angle);
    right = std::sin (angle);
}

/** Balance law for a stereo source: the opposite side is attenuated, centre is unity. */
inline void stereoBalance (float pan, float& left, float& right) noexcept
{
    pan = std::clamp (pan, -1.0f, 1.0f);
    left = std::min (1.0f, 1.0f - pan);
    right = std::min (1.0f, 1.0f + pan);
}

} // namespace livedaw::dsp
