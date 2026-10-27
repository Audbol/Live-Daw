#pragma once

#include "engine/Models.h"

#include <cmath>
#include <optional>

namespace livedaw::ui
{

/** Tap tempo on the blank key under the knob right of the display (manual
    4.1: "acting as a tap tempo button when editing delay effects"; 5.5
    p. 48: it "always blinks when it can be used for Tap Tempo"). EFFECTS
    and the INS 1 / INS 2 screens of HOME use it for the FX they show. */
class TapTempo
{
public:
    /** The TIME (ms) parameter of FX model `fxModel`, or -1 (no delay time). */
    static int timeParam (int fxModel)
    {
        const auto& spec = models::model (models::Fx, fxModel);
        for (size_t i = 0; i < spec.params.size(); ++i)
        {
            const auto& p = spec.params[i];
            if (p.unit == "ms" && p.isContinuous() && (p.id == "time" || p.id == "dly" || p.label.equalsIgnoreCase ("TIME")))
                return (int) i;
        }
        return -1;
    }

    /** A tap at `nowMs`: the time since the last tap when it is a beat (0.1
        to 3 s), else nothing (the first tap of a new count). */
    std::optional<float> tap (double nowMs)
    {
        const double interval = nowMs - lastTap;
        lastTap = nowMs;
        if (interval < 100.0 || interval > 3000.0)
            return std::nullopt;
        return (float) interval;
    }

    /** The key's LED blinking at the tempo of `periodMs`: lit the first half
        of each beat, counted from the last tap (so it blinks with the taps). */
    bool ledLit (double nowMs, float periodMs) const
    {
        if (periodMs <= 0.0f)
            return true;
        const double phase = std::fmod (std::abs (nowMs - lastTap), (double) periodMs);
        return phase < (double) periodMs * 0.5;
    }

private:
    double lastTap = -1.0e9;
};

} // namespace livedaw::ui
