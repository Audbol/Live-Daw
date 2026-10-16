#pragma once

// Level conversions for the UI (meters, labels). The audio DSP lives in the
// Rust engine (rust/crates/livedaw-dsp).

#include <algorithm>
#include <cmath>

namespace livedaw::dsp
{

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

} // namespace livedaw::dsp
