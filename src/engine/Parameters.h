#pragma once

// Strip parameters are owned by the Rust engine; these are its C++ views
// (see rust/crates/livedaw-ffi/include/livedaw_engine.h). The UI writes the
// atomics directly; the audio thread snapshots them once per block.

#include "livedaw_engine.h"

namespace ld
{
/** Field-wise comparison (found by ADL). */
inline bool operator== (const EqSnapshot& a, const EqSnapshot& b) noexcept
{
    if (a.enabled != b.enabled)
        return false;
    for (int i = 0; i < kNumEqBands; ++i)
    {
        const auto &x = a.bands[i], &y = b.bands[i];
        if (x.type != y.type || x.frequency != y.frequency || x.gainDb != y.gainDb || x.q != y.q || x.enabled != y.enabled)
            return false;
    }
    return true;
}
} // namespace ld

namespace livedaw
{

/** Upper bound on bus slots (send storage grows on demand; CPU is the real limit). */
constexpr int kMaxBuses = ld::kMaxBuses;

/** Number of insert slots per strip for VST/AU/LV2 plugins. */
constexpr int kNumInsertSlots = ld::kNumInsertSlots;

constexpr int kNumEqBands = ld::kNumEqBands;

using GateParams = ld::GateParams;
using CompParams = ld::CompParams;
using EqBandParams = ld::EqBandParams;
using EqParams = ld::EqParams;
using StripParams = ld::StripParams;
using SendParams = ld::SendParams;

/** EQ band types (values match livedaw_dsp::BandType). */
enum class EqBandType : int
{
    Bell = 0,
    LowShelf = 1,
    HighShelf = 2,
    LowCut = 3,
    HighCut = 4
};

inline bool isCut (EqBandType t) noexcept { return t == EqBandType::LowCut || t == EqBandType::HighCut; }

/** Plain-value copy of an EQ's settings (for drawing and change detection). */
inline ld::EqSnapshot snapshotEq (const EqParams& eq) noexcept
{
    ld::EqSnapshot s {};
    s.enabled = eq.enabled.load() ? 1 : 0;
    for (int i = 0; i < kNumEqBands; ++i)
    {
        const auto& b = eq.bands[i];
        s.bands[i] = { b.type.load(), b.frequency.load(), b.gainDb.load(), b.q.load(), b.enabled.load() ? 1 : 0 };
    }
    return s;
}

/** Combined EQ response in dB at `freq`: the same maths the engine runs (computed in Rust). */
inline double eqResponseDb (const ld::EqSnapshot& s, double freq, double sampleRate) noexcept
{
    return ld::ld_eq_response_db (&s, freq, sampleRate);
}

} // namespace livedaw
