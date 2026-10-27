#pragma once

#include "dsp/DspUtils.h"

#include <algorithm>
#include <iterator>

/** How every level meter moves (one model for the screens, the surface's
    LEDs, the main meter and the WING remote's meter stream) and the WING
    screens' meter scale.

    The manual and the protocol document give no ballistics; WING Edit draws
    the console's meter stream as it comes (no decay or hold of its own), so
    the console's levels arrive with their ballistics. Sidecar's are the
    conventional peak meter's: the full peak at once, a fall of
    kReleaseDbPerSecond, and a peak hold of kHoldSeconds that then falls at the
    same rate. The Rust meter stream (livedaw-wing meters.rs, RELEASE_DB_PER_S)
    falls at the same rate. */
namespace livedaw::ui::meters
{
/** The fall of a level, dB per second. */
constexpr float kReleaseDbPerSecond = 24.0f;
/** How long a peak-hold mark stays before it falls. */
constexpr float kHoldSeconds = 1.0f;
/** Silence. */
constexpr float kFloorDb = dsp::kMinusInfDb;

/** One meter: its level and its peak hold, in dB. */
struct Ballistics
{
    float levelDb = kFloorDb;
    float holdDb = kFloorDb;
    float holdLeft = 0.0f; // seconds the hold mark still stays

    /** Moves on by `dt` seconds with `peak` (linear, the highest sample in that time). */
    void advance (float peak, float dt) noexcept
    {
        dt = std::max (0.0f, dt);
        const float db = dsp::gainToDb (peak);
        levelDb = std::max ({ db, levelDb - kReleaseDbPerSecond * dt, kFloorDb });
        if (db >= holdDb)
        {
            holdDb = db;
            holdLeft = kHoldSeconds;
            return;
        }
        // Past its hold time the mark falls (for the part of dt after it), never below the level.
        const float falling = std::max (0.0f, dt - std::max (0.0f, holdLeft));
        holdLeft -= dt;
        holdDb = std::max (levelDb, holdDb - kReleaseDbPerSecond * falling);
    }

    void reset() noexcept { *this = {}; }
};

/** Where `db` sits on the WING screens' meters (METERS, OVERVIEW, FADERS,
    MONITOR), 0 at -60 dB to 1 at 0 dB (CLIP): six equal steps between the
    marks -60, -48, -36, -24, -12, -6 and 0 (firmware 3.1). */
inline float scalePos (float db) noexcept
{
    static constexpr float marks[] = { -60.0f, -48.0f, -36.0f, -24.0f, -12.0f, -6.0f, 0.0f };
    if (db <= marks[0])
        return 0.0f;
    for (size_t i = 1; i < std::size (marks); ++i)
        if (db <= marks[i])
            return ((float) (i - 1) + (db - marks[i - 1]) / (marks[i] - marks[i - 1])) / 6.0f;
    return 1.0f;
}
} // namespace livedaw::ui::meters
