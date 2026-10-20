#pragma once

#include "engine/Analysis.h"
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
    lines labelled at the top with a ring handle near the bottom. The key
    filter and crossover views (GATE KEY FILTER, COMP XOVER) fill the area
    under the filter's response (green for a key filter, blue for the
    crossover) on a +10 .. -30 dB scale, with the FREQ line to drag.

    The RTA of the strip (the engine's analyser) shows behind the curves as
    one bar per semitone with a peak-hold mark, as RTA SETTINGS / EQ/RTA
    SETTINGS set it up: SIZE/MODE (over part of the graph, or in a strip of
    its own under the EQ on the EQ screen), RANGE, FIXED GAIN, AUTOGAIN and
    COLOR. The graph that is showing drives the analyser. */
class ResponseGraph final : public juce::Component, private juce::Timer
{
public:
    /** The block shown (the block numbers of ld_strip_response). */
    enum Block
    {
        Eq = 1,
        Filter = 3,
        GateKey = 5,
        DynamicsKey = 6,
        Xover = 7
    };

    ResponseGraph();
    ~ResponseGraph() override;

    /** Which RTA view the graph shows. Auto: FILTER for the filter, the EQ
        screen's with the ruler at the top, HOME's (the overview) otherwise. */
    enum class RtaView
    {
        Auto,
        None,
        Home,
        Eq,
        Filter
    };
    void setRtaView (RtaView v)
    {
        rtaView = v;
        repaint();
    }

    /** The RTA's colour for COLOR `index` (RD25 ... BL75). */
    static juce::Colour rtaColour (int index);
    /** The RTA view (ld::RtaView) of the graph shown last (whatever its SIZE/MODE). */
    static int shownRtaView() noexcept { return lastShownView; }

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

    /** The overview panel's graph: the bare grid with the ruler, no labels, the
        bands as small crosses (the selected one blue) [I1, Fig. 21]. */
    void setOverview (bool shouldBe)
    {
        overview = shouldBe;
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
    /** On the FILTER graph: resets the LC / HC (or all-pass) line to its default. */
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    static constexpr float kRangeDb = 18.0f;

private:
    struct Band
    {
        juce::String id;
        int f = -1, g = -1, q = -1;
    };

    /** How the RTA shows: its view (ld::RtaView, -1 none), over the curve or
        in a strip of its own, and how much of the graph's height it takes. */
    struct RtaLayout
    {
        int view = -1;
        bool split = false;
        float fraction = 0.0f;
    };
    RtaLayout rtaLayout() const;
    /** This graph's RTA view (ld::RtaView), or -1. */
    int rtaViewId() const;
    void updateRta();
    void paintRta (juce::Graphics&, juce::Rectangle<float> area, const RtaLayout&);
    /** The EQ curve's area: the plot, less the RTA's own strip. */
    juce::Rectangle<float> curvePlot() const;

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
    /** A key filter or crossover view. */
    bool isKeyView() const noexcept { return block == GateKey || block == DynamicsKey || block == Xover; }
    /** The frequency the key / crossover view's FREQ line sets. */
    std::atomic<float>* keyFreq() const;
    /** The levels at the top and bottom of the curve's area. */
    float topDb() const;
    float bottomDb() const;

    Strip* strip = nullptr;
    Block block = Eq;
    Ruler ruler = Ruler::Bottom;
    /** eqResponse: the other block drawn alongside (FILTER: the channel EQ
        behind it; EQ: the low/high cut filters over it). */
    std::vector<double> freqs, response, eqResponse;
    /** The line and band positions drawn last. */
    std::vector<float> shownMarks;
    std::vector<Band> bands;
    int builtModel = -1, selectedBand = -1, dragBand = -1, dragLine = -1;

    RtaView rtaView = RtaView::Auto;
    bool overview = false;
    Analysis::Bands rta;
    bool rtaShown = false;
    float autoGainDb = 0.0f;
    /** The graph that drives the analyser now (one at a time). */
    static ResponseGraph* rtaOwner;
    static int lastShownView;
};

} // namespace livedaw::ui
