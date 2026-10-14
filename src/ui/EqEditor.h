#pragma once

#include "engine/Parameters.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace livedaw::ui
{

/** Graphical 4-band EQ display. Drag a band's handle to change its frequency and
    gain; use the mouse wheel over it to change Q. */
class EqEditor final : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    EqEditor (EqParams& params, std::function<double()> sampleRate);

    /** Compact mode is used for the mini EQ on mixer strips: no labels, small handles. */
    void setCompact (bool shouldBeCompact);

    /** Called when the background (not a band handle) is clicked. */
    std::function<void()> onBackgroundClick;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    void timerCallback() override;

    float freqToX (float freq) const;
    float xToFreq (float x) const;
    float dbToY (float db) const;
    float yToDb (float y) const;
    juce::Point<float> handlePosition (int band) const;
    int hitTestBand (juce::Point<float>) const;

    EqParams& params;
    std::function<double()> getSampleRate;
    dsp::ParametricEq::Settings lastDrawn;
    int dragBand = -1;
    bool compact = false;

    static constexpr float kMinFreq = 20.0f, kMaxFreq = 20000.0f, kRangeDb = 18.0f;
};

} // namespace livedaw::ui
