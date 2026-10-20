#pragma once

#include "engine/Models.h"
#include "engine/Parameters.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <atomic>
#include <functional>
#include <vector>

namespace livedaw::ui
{

/** The dynamics models the console draws as a transfer graph with the
    ENVELOPE panel (WING GATE, DUCKER, COMPRESSOR, EXPANDER); the others show
    their front panel. */
bool hasTransferGraph (int model);

/** Level range of a model's transfer graph in dB (80 for the gate and the
    ducker, whose thresholds reach -80 dB; 60 otherwise). */
float transferRangeDb (int model);

/** The static curve of `model` with `values` at `count` input levels from
    -range to 0 dB (empty if the model has no level curve). */
std::vector<float> transferCurve (int model, const float* values, int count);

/** A loaded slot's parameter values as plain floats. */
std::vector<float> slotValues (const DynSlotParams&);

/** The transfer graph as a thumbnail (HOME, the model list): black, the curve
    orange while the slot is on (grey when off), the threshold as a white dot
    on its line. */
void paintTransferThumb (juce::Graphics&, juce::Rectangle<float>, int model, const float* values, bool on);

/** A model list tile's picture for a native model: the transfer graph left,
    the ENVELOPE shape right (green), as the 3.1 model list draws them. */
void paintDynamicsTile (juce::Graphics&, juce::Rectangle<float>, int model, const float* values, bool on);

//==============================================================================
/** The transfer graph of a WING dynamics model (manual Fig. 92-94, firmware
    3.x): a dark plot with a dot grid every 5 dB and the levels 0 .. -60 (-80
    for the gate) down the left; the static curve (computed by the engine from
    the model's own settings), orange while the slot is on; the threshold as a
    grey line across with the round handle at the right (drag it); the live
    level as a yellow dot on the curve; the gain reduction as a red segment
    meter down the left edge and the slot's output level on the ticks at the
    right. */
class DynTransferGraph final : public juce::Component, private juce::Timer
{
public:
    /** `gr` returns the reduction (dB >= 0), `keyDb` the detector level (dB),
        `outPeak` the output peak since the last call (linear). */
    DynTransferGraph (DynSlotParams&, std::function<float()> gr, std::function<float()> keyDb, std::function<float()> outPeak);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

    /** The plot area (levels -range..0 on both axes). */
    juce::Rectangle<float> plotArea() const;

private:
    void timerCallback() override;
    void rebuild();
    float yFor (float db) const;
    float xFor (float db) const;
    float dbForY (float y) const;
    int thresholdIndex() const;
    juce::Rectangle<float> handleArea() const;

    DynSlotParams& slot;
    std::function<float()> readGr, readKey, readOut;
    int builtModel = -1;
    std::vector<float> builtValues, curve;
    float range = 60.0f, gr = 0.0f, key = -120.0f, out = 0.0f;
    float shownGr = 0.0f, shownKey = -120.0f, shownOut = 0.0f;
    bool builtOn = false, dragging = false;
};

//==============================================================================
/** The ENVELOPE panel of a WING dynamics model: the title bar, then ATTACK as
    a ramp, HOLD as a flat top between two grey lines and RELEASE as a ramp
    down, each as wide as its knob is turned, green over a dark green fill
    while the slot is on. The round handles (attack and release at the
    bottom, hold at the top) drag their parameter sideways. */
class DynEnvelopeGraph final : public juce::Component, private juce::Timer
{
public:
    explicit DynEnvelopeGraph (DynSlotParams&);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

    /** Paints the shape in `area` (also for the model list's tiles). */
    static void paintShape (juce::Graphics&, juce::Rectangle<float> area, float attack, float hold, float release, bool on, bool handles);

private:
    void timerCallback() override;
    juce::Rectangle<float> graphArea() const;
    /** Normalised ATTACK, HOLD, RELEASE (0..1; -1 when the model has none). */
    std::array<float, 3> positions() const;
    std::array<juce::Point<float>, 3> handlePoints() const;
    int paramIndex (int which) const;

    DynSlotParams& slot;
    std::array<float, 3> shown { -2.0f, -2.0f, -2.0f };
    bool shownOn = false;
    int dragHandle = -1;
    float dragStart = 0.0f;
};

} // namespace livedaw::ui
