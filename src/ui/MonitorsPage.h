#pragma once

#include "ui/DisplayPage.h"
#include "ui/ScreenWidgets.h"

#include <memory>

namespace livedaw::ui
{

/** A WING knob box: a ring at the left, a small caption over a big value, on a
    dark well. Drag up / down or turn the wheel; double-click resets. Drawn
    dim (and inert) without a binding. */
class KnobField final : public juce::Component, private juce::Timer
{
public:
    explicit KnobField (KnobBinding b = {}, juce::Colour ringColour = Dark::text);

    void setBinding (KnobBinding);
    const KnobBinding& getBinding() const noexcept { return binding; }
    /** The label to show while unbound (the console's control that isn't here yet). */
    juce::String placeholder, placeholderValue;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    void timerCallback() override;

    KnobBinding binding;
    juce::Colour ring;
    float dragStart = 0.0f;
    juce::String shown;
};

/** MONITORS (manual chapter 7, fig. 72), opened by SETUP's MONITORS key and the
    MONITORING/TALKBACK VIEW key: the toolbar (TALK A/B, PHONES, SPEAKERS,
    PH+SPK, DIM, MUTE, MONO, LR SWAP), the TALK A / TALK B settings, the
    PHONES / SPEAKERS settings and the LEVEL panel. The engine has one monitor
    path (PH+SPK) with level, DIM (and its amount), MUTE, MONO, LR SWAP,
    PFL / AFL and its outputs; talkback, the second path, source select, SRC MIX,
    PFL DIM, BAND SOLO, DIRECT IN, EQ, delay and limiter are drawn disabled. */
class MonitorsPage final : public DisplayPage, private juce::Timer
{
public:
    explicit MonitorsPage (UiContext&);
    ~MonitorsPage() override;

    std::vector<KnobBinding> getKnobs() override;
    StatusTitle getStatusTitle() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    struct ToolKey
    {
        juce::String text;
        Icon icon;
        juce::Colour colour;
        std::function<bool()> lit;
        std::function<void()> press;
        juce::Rectangle<int> area;
    };

    void timerCallback() override;
    void paintToolbar (juce::Graphics&);
    void paintTalk (juce::Graphics&);
    void paintPath (juce::Graphics&);
    void paintLevel (juce::Graphics&);
    juce::String outputName (int port) const;
    void chooseOutput (bool right);

    UiContext& ctx;
    std::vector<ToolKey> tools;
    juce::Rectangle<int> toolbar, talkTabs, talkArea, pathTabs, pathHeader, pathArea, levelHeader, levelArea;
    juce::Rectangle<int> switchBox, autoDimBox, busGrid, mtxGrid, mainGrid, sourceField, directField, eqKey, distSlider, invertKey, delayKey,
        outputBox, leftField, rightField, soloBox, meterArea, faderArea;
    KnobField srcLevel, srcMix, dim, pflDim, bandSolo, busDim, monDim, limiter;
    std::unique_ptr<ChoiceKeys> soloKeys, switchMode;
    juce::Component anchor;
    std::array<float, 2> shownPeak {};
};

} // namespace livedaw::ui
