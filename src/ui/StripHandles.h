#pragma once

#include "ui/Controls.h"
#include "ui/EqEditor.h"
#include "ui/UiContext.h"

namespace livedaw::ui
{

/** Insert slots on a mixer strip. Click an empty slot to load a plugin, click a
    loaded one to open its editor, right-click for bypass/replace/remove.
    Faulted (auto-bypassed) plugins show in red. */
class InsertSlotsHandle final : public juce::Component, public juce::TooltipClient, private juce::Timer
{
public:
    InsertSlotsHandle (Strip&, UiContext&);

    void setNumRows (int rows);
    static constexpr int kRowHeight = 14;

    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;
    juce::String getTooltip() override { return "Inserts: click to load or open, right-click for options"; }

private:
    void timerCallback() override;
    int rowAt (juce::Point<int>) const;

    Strip& strip;
    UiContext& ctx;
    int numRows = 4;
    juce::String lastSignature;
};

/** Gate on a mixer strip: [G] toggles, the bar shows the input level against the
    threshold (yellow tick). Drag the bar to set the threshold. */
class GateHandle final : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    explicit GateHandle (Strip&);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    void timerCallback() override;

    Strip& strip;
    float levelDb = -100.0f, dragStartThreshold = 0.0f;
    bool draggingBar = false;
    juce::String lastSignature;
};

/** Compressor on a mixer strip: [C] toggles, the bar shows gain reduction.
    Drag to set the threshold. */
class CompHandle final : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    explicit CompHandle (Strip&);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    void timerCallback() override;

    Strip& strip;
    float reductionDb = 0.0f, dragStartThreshold = 0.0f;
    bool draggingBar = false;
    juce::String lastSignature;
};

/** Tiny painted on/off toggle bound to an atomic<bool> (fits where a TextButton can't). */
class MiniToggle final : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    MiniToggle (std::atomic<bool>& p, const juce::String& text, juce::Colour onColour);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    void timerCallback() override;

    std::atomic<bool>& param;
    juce::String label;
    juce::Colour colour;
    bool shown = false;
};

/** Mini EQ on a mixer strip with draggable band handles (wheel = Q), plus EQ
    and HPF on/off toggles. Clicking the background selects the strip. */
class EqHandle final : public juce::Component
{
public:
    EqHandle (Strip&, UiContext&);
    void resized() override;

private:
    EqEditor editor;
    MiniToggle eqOn, hpfOn;
};

} // namespace livedaw::ui
