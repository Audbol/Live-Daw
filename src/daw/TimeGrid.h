#pragma once

#include <juce_core/juce_core.h>

#include <vector>

namespace livedaw::daw
{

/** What edits snap to. GRID follows the zoom (bar or beat subdivisions that
    stay a usable distance apart on screen); the others are fixed. */
enum class SnapMode
{
    Off,
    Grid,
    Bars,
    Beats,
    Seconds
};

const char* snapModeName (SnapMode);

/** Musical and clock time on a timeline counted in samples. */
struct TimeGrid
{
    double sampleRate = 48000.0;
    double tempo = 120.0;     // beats per minute
    int beatsPerBar = 4;
    int beatUnit = 4;         // the time signature's denominator

    double samplesPerBeat() const noexcept { return sampleRate * 60.0 / tempo * 4.0 / (double) beatUnit; }
    double samplesPerBar() const noexcept { return samplesPerBeat() * (double) beatsPerBar; }

    /** The spacing of snap points, in samples (0 for Off). For Grid and
        Seconds the step adapts so that points are at least `minPixels` apart
        at `samplesPerPixel`. */
    double step (SnapMode, double samplesPerPixel, double minPixels = 10.0) const;

    /** The nearest snap point to `position` (unchanged for Off). */
    juce::int64 snap (juce::int64 position, SnapMode, double samplesPerPixel) const;

    /** Ruler divisions for a zoom: a major step (labelled) and a minor step,
        in samples, musical when `bars` is true, else clock time. */
    void rulerSteps (bool bars, double samplesPerPixel, double& major, double& minor) const;

    /** "1:23.456" (m:ss.mmm; h:mm:ss.mmm past an hour) */
    juce::String formatTime (juce::int64 samples) const;
    /** "12|3|240" (bar|beat|tick of 480 per beat, 1-based) */
    juce::String formatBars (juce::int64 samples) const;
    /** "12" or "12.3" for ruler labels */
    juce::String formatBarLabel (juce::int64 samples, double majorStep) const;
    juce::String formatTimeLabel (juce::int64 samples, double majorStep) const;
};

/** Magnetic snapping to edges (clip edges, markers, the playhead): the
    nearest edge within `tolerance` samples, or `position` if none is. */
juce::int64 snapToEdges (juce::int64 position, const std::vector<juce::int64>& edges, juce::int64 tolerance, bool* snapped = nullptr);

} // namespace livedaw::daw
