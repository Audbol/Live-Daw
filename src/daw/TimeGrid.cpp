#include "daw/TimeGrid.h"

namespace livedaw::daw
{

const char* snapModeName (SnapMode m)
{
    switch (m)
    {
        case SnapMode::Off:     return "OFF";
        case SnapMode::Grid:    return "GRID";
        case SnapMode::Bars:    return "BARS";
        case SnapMode::Beats:   return "BEATS";
        case SnapMode::Seconds: return "SECONDS";
    }
    return "OFF";
}

namespace
{
    /** The first step of `candidates` (in samples) at least `minSamples` long, or the last. */
    double firstAtLeast (std::initializer_list<double> candidates, double unit, double minSamples)
    {
        double s = 0.0;
        for (auto c : candidates)
        {
            s = c * unit;
            if (s >= minSamples)
                return s;
        }
        return s;
    }
} // namespace

double TimeGrid::step (SnapMode mode, double samplesPerPixel, double minPixels) const
{
    const double minSamples = samplesPerPixel * minPixels;
    switch (mode)
    {
        case SnapMode::Off:   return 0.0;
        case SnapMode::Bars:  return samplesPerBar();
        case SnapMode::Beats: return samplesPerBeat();
        case SnapMode::Grid:
        {
            if (minSamples <= samplesPerBeat())
                return firstAtLeast ({ 1.0 / 16.0, 1.0 / 8.0, 1.0 / 4.0, 1.0 / 2.0, 1.0 }, samplesPerBeat(), minSamples);
            return firstAtLeast ({ 1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0 }, samplesPerBar(), minSamples);
        }
        case SnapMode::Seconds:
            return firstAtLeast ({ 0.001, 0.01, 0.1, 0.5, 1.0, 5.0, 10.0, 30.0, 60.0, 300.0 }, sampleRate, minSamples);
    }
    return 0.0;
}

juce::int64 TimeGrid::snap (juce::int64 position, SnapMode mode, double samplesPerPixel) const
{
    const double s = step (mode, samplesPerPixel);
    if (s <= 0.0)
        return position;
    return juce::jmax<juce::int64> (0, (juce::int64) std::llround (std::round ((double) position / s) * s));
}

void TimeGrid::rulerSteps (bool bars, double samplesPerPixel, double& major, double& minor) const
{
    // Labels at least ~70 px apart, ticks at least ~8 px.
    const double labelSamples = samplesPerPixel * 70.0, tickSamples = samplesPerPixel * 8.0;
    if (bars)
    {
        if (labelSamples <= samplesPerBeat())
            major = firstAtLeast ({ 1.0 / 4.0, 1.0 / 2.0, 1.0 }, samplesPerBeat(), labelSamples);
        else if (labelSamples <= samplesPerBar())
            major = samplesPerBar();
        else
            major = firstAtLeast ({ 1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0, 128.0 }, samplesPerBar(), labelSamples);
        // Several bars tick every bar, a bar ticks its beats, beats their quarters.
        minor = major;
        if (major > samplesPerBar() * 1.001)
        {
            if (samplesPerBar() >= tickSamples)
                minor = samplesPerBar();
        }
        else if (major > samplesPerBar() * 0.999)
        {
            if (samplesPerBeat() >= tickSamples)
                minor = samplesPerBeat();
        }
        else
        {
            for (double d : { 4.0, 2.0 })
                if (major / d >= tickSamples)
                {
                    minor = major / d;
                    break;
                }
        }
    }
    else
    {
        major = firstAtLeast ({ 0.01, 0.05, 0.1, 0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0, 600.0, 1800.0, 3600.0 }, sampleRate, labelSamples);
        minor = major;
        for (double d : { 2.0, 5.0, 10.0 })
            if (major / d >= tickSamples)
                minor = major / d;
    }
}

juce::String TimeGrid::formatTime (juce::int64 samples) const
{
    const bool negative = samples < 0;
    const auto ms = (juce::int64) std::llround ((double) std::abs (samples) * 1000.0 / sampleRate);
    const auto h = ms / 3600000, m = (ms / 60000) % 60, s = (ms / 1000) % 60, frac = ms % 1000;
    juce::String t = negative ? "-" : "";
    if (h > 0)
        t << juce::String (h) << ":" << juce::String (m).paddedLeft ('0', 2);
    else
        t << juce::String (m);
    t << ":" << juce::String (s).paddedLeft ('0', 2) << "." << juce::String (frac).paddedLeft ('0', 3);
    return t;
}

juce::String TimeGrid::formatBars (juce::int64 samples) const
{
    const double beats = juce::jmax (0.0, (double) samples / samplesPerBeat());
    const auto wholeBeats = (juce::int64) std::floor (beats + 1.0e-9);
    const int ticks = juce::jlimit (0, 479, (int) std::floor ((beats - (double) wholeBeats) * 480.0));
    const auto bar = wholeBeats / beatsPerBar + 1;
    const auto beat = wholeBeats % beatsPerBar + 1;
    return juce::String (bar) + "|" + juce::String (beat) + "|" + juce::String (ticks).paddedLeft ('0', 3);
}

juce::String TimeGrid::formatBarLabel (juce::int64 samples, double majorStep) const
{
    const double beats = (double) samples / samplesPerBeat();
    const auto wholeBeats = (juce::int64) std::floor (beats + 1.0e-6);
    const auto bar = wholeBeats / beatsPerBar + 1;
    if (majorStep >= samplesPerBar() * 0.999)
        return juce::String (bar);
    const auto beat = wholeBeats % beatsPerBar + 1;
    const double frac = beats - (double) wholeBeats;
    if (frac > 1.0e-3)
        return juce::String (bar) + "." + juce::String (beat) + "." + juce::String (juce::roundToInt (frac * 4.0) + 1);
    return juce::String (bar) + "." + juce::String (beat);
}

juce::String TimeGrid::formatTimeLabel (juce::int64 samples, double majorStep) const
{
    const double seconds = (double) samples / sampleRate;
    const auto whole = (juce::int64) std::floor (seconds + 1.0e-6);
    const auto m = whole / 60, s = whole % 60;
    juce::String t = juce::String (m) + ":" + juce::String (s).paddedLeft ('0', 2);
    if (majorStep < sampleRate * 0.999)
    {
        const int digits = majorStep < sampleRate * 0.0999 ? 2 : 1;
        const auto frac = juce::roundToInt ((seconds - (double) whole) * std::pow (10.0, digits));
        t << "." << juce::String (frac).paddedLeft ('0', digits);
    }
    return t;
}

juce::int64 snapToEdges (juce::int64 position, const std::vector<juce::int64>& edges, juce::int64 tolerance, bool* snapped)
{
    juce::int64 best = position, bestDistance = tolerance + 1;
    for (auto e : edges)
    {
        const auto d = std::abs (e - position);
        if (d < bestDistance)
        {
            bestDistance = d;
            best = e;
        }
    }
    if (snapped != nullptr)
        *snapped = bestDistance <= tolerance;
    return bestDistance <= tolerance ? best : position;
}

} // namespace livedaw::daw
