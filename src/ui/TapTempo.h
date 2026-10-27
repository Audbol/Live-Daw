#pragma once

#include "engine/Models.h"

#include <cmath>
#include <optional>
#include <utility>

namespace livedaw::ui
{

/** Tap tempo on the blank key under the knob right of the display (manual
    4.1: "acting as a tap tempo button when editing delay effects"; 5.5
    p. 48: it "always blinks when it can be used for Tap Tempo"). EFFECTS
    and the INS 1 / INS 2 screens of HOME use it for the FX they show. */
class TapTempo
{
public:
    /** The delay time parameter of FX model `fxModel`, or -1 (no delay time):
        a TIME in ms (ST-DL, TAP-DL, TAPE-DL, DEL/REV) or the dial of the oil
        can (TIME 0-10) and the BBD (DELAY 0-100). Not the pitch devices'
        DELAY, which isn't a tempo. */
    static int timeParam (int fxModel)
    {
        const auto& spec = models::model (models::Fx, fxModel);
        if (spec.token == "PITCH" || spec.token == "D-PITCH")
            return -1;
        if (const auto dial = dialRange (spec.token); dial.first > 0.0f)
            return spec.find (spec.token == "OILCAN" ? "time" : "dly");
        for (size_t i = 0; i < spec.params.size(); ++i)
        {
            const auto& p = spec.params[i];
            if (p.unit == "ms" && p.isContinuous() && (p.id == "time" || p.id == "dly" || p.label.equalsIgnoreCase ("TIME")))
                return (int) i;
        }
        return -1;
    }

    /** The value of `fxModel`'s time parameter `param` for a delay of `ms`
        (the dials are logarithmic over the delay range, as the devices read them). */
    static float valueForMs (int fxModel, int param, float ms)
    {
        const auto& spec = models::model (models::Fx, fxModel);
        const auto& p = spec.params[(size_t) param];
        if (const auto [lo, hi] = dialRange (spec.token); lo > 0.0f)
            return juce::jlimit (p.min, p.max, p.max * std::log (juce::jlimit (lo, hi, ms) / lo) / std::log (hi / lo));
        return juce::jlimit (p.min, p.max, ms);
    }

    /** The delay in ms of `fxModel`'s time parameter `param` at `value`. */
    static float msOf (int fxModel, int param, float value)
    {
        const auto& spec = models::model (models::Fx, fxModel);
        const auto& p = spec.params[(size_t) param];
        if (const auto [lo, hi] = dialRange (spec.token); lo > 0.0f)
            return lo * std::pow (hi / lo, juce::jlimit (0.0f, 1.0f, value / p.max));
        return value;
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
    /** The delay range (ms) of a device whose time is a dial (livedaw-dsp
        oilcan.rs / bbd.rs TIME_MS), {0, 0} for the others. */
    static std::pair<float, float> dialRange (const juce::String& token)
    {
        if (token == "OILCAN")
            return { 60.0f, 480.0f };
        if (token == "BBD-DL")
            return { 20.0f, 600.0f };
        return { 0.0f, 0.0f };
    }

    double lastTap = -1.0e9;
};

} // namespace livedaw::ui
