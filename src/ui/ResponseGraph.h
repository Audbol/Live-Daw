#pragma once

#include "ui/UiContext.h"

#include <vector>

namespace livedaw::ui
{

/** Frequency response of a strip's EQ or FILTER, computed by the engine's own
    filters (ld_strip_response), drawn like the WING (firmware 3.1): a black
    graph with a grey log grid, the EQ curve in amber over a dark amber fill, a
    piano-key ruler (along the bottom, or at the top on the EQ screen) and the
    frequencies as 20 ... 1K ... 20K. With the WING EQ (STD) loaded, each band
    is a grey line from its label at the top with an open ring at its gain
    (the selected band's line blue, its ring filled black): drag to set
    frequency and gain, mouse wheel for Q. The FILTER view draws the filter in
    blue over the EQ curve, LC / HC (and the all-pass AP, yellow) as vertical
    lines labelled at the top with a ring handle near the bottom. */
class ResponseGraph final : public juce::Component, private juce::Timer
{
public:
    enum Block
    {
        Eq = 1,
        Filter = 3
    };

    ResponseGraph();

    void setStrip (Strip*, Block);

    /** Where the piano-key ruler goes. */
    enum class Ruler
    {
        Bottom,
        Top,
        None
    };
    void setRuler (Ruler r)
    {
        ruler = r;
        repaint();
    }

    /** The piano-key ruler: grey white keys, the black keys as dark notches. */
    static void paintPianoRuler (juce::Graphics&, juce::Rectangle<float> area, std::function<float (float hz)> xForHz);

    /** Band index (into the STD bands) last touched, or -1. */
    int getSelectedBand() const noexcept { return selectedBand; }
    void setSelectedBand (int band)
    {
        selectedBand = band;
        repaint();
    }
    std::function<void (int band)> onBandSelected;

    /** STD EQ band ids of a strip ("l", "1", ... "h"). */
    static juce::StringArray stdBands (const Strip&);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    static constexpr float kRangeDb = 18.0f;

private:
    struct Band
    {
        juce::String id;
        int f = -1, g = -1, q = -1;
    };

    void timerCallback() override;
    void updateBands();
    float xForFreq (float f) const;
    float freqForX (float x) const;
    float yForDb (float db) const;
    float dbForY (float y) const;
    juce::Rectangle<float> plot() const;
    bool showLabels() const;
    int bandAt (juce::Point<float>) const;
    int cutLineAt (juce::Point<float>) const;
    juce::Path curveOf (const std::vector<double>&) const;

    Strip* strip = nullptr;
    Block block = Eq;
    Ruler ruler = Ruler::Bottom;
    std::vector<double> freqs, response, eqResponse;
    std::vector<Band> bands;
    int builtModel = -1, selectedBand = -1, dragBand = -1, dragLine = -1;
};

} // namespace livedaw::ui
