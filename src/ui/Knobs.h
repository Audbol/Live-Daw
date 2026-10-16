#pragma once

#include "engine/Models.h"
#include "ui/LookAndFeel.h"

#include <atomic>
#include <functional>

namespace livedaw::ui
{

/** The WING fader law: +10 dB at the top, 0 dB at 3/4, -inf at the bottom. */
namespace fader
{
    constexpr float kMinDb = -100.0f; // and below: -inf
    constexpr float kMaxDb = 10.0f;

    float dbToPos (float db) noexcept;
    float posToDb (float pos) noexcept;
    /** "-oo", "-3.5", "+10.0" */
    juce::String text (float db);
    juce::NormalisableRange<double> range();
} // namespace fader

/** "80.0", "1.25k", "16k" */
juce::String formatHz (float hz);

/** What one of the six knobs under the display (or any generic knob) controls. */
struct KnobBinding
{
    juce::String label;
    std::function<float()> get;        // normalised 0..1
    std::function<void (float)> set;   // normalised 0..1
    std::function<juce::String()> text;
    std::function<void()> reset;       // double-click
    bool bipolar = false;
    float steps = 0.0f;                // > 0: snap to this many steps

    bool isValid() const noexcept { return static_cast<bool> (get); }
};

KnobBinding bindLinear (const juce::String& label, std::atomic<float>&, float min, float max,
                        const juce::String& suffix, int decimals, float defaultValue);
KnobBinding bindLog (const juce::String& label, std::atomic<float>&, float min, float max,
                     const juce::String& suffix, float defaultValue);
KnobBinding bindFreq (const juce::String& label, std::atomic<float>&, float min, float max, float defaultValue);
KnobBinding bindFader (const juce::String& label, std::atomic<float>&);
KnobBinding bindModelParam (std::atomic<float>& value, const models::Param&);
KnobBinding bindChoice (const juce::String& label, std::atomic<int32_t>&, const juce::StringArray& items, int defaultIndex = 0);
KnobBinding bindPan (const juce::String& label, std::atomic<float>&);

/** A captioned rotary knob driven by a KnobBinding (polled, so edits made
    elsewhere show up). An unbound knob is drawn empty. */
class DisplayKnob final : public juce::Component, private juce::Timer
{
public:
    DisplayKnob();

    void setBinding (KnobBinding);
    const KnobBinding& getBinding() const noexcept { return binding; }

    /** Size of the caption and value fonts. */
    void setCompact (bool c) { compact = c; resized(); }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    KnobBinding binding;
    juce::Slider slider;
    juce::String valueText;
    bool compact = false;
};

/** One of the six parameter slots along the bottom of the WING's display:
    the parameter name and value above a position bar. Drag sideways or turn the
    mouse wheel to change it, double-click to reset. */
class ParamSlot final : public juce::Component, private juce::Timer
{
public:
    ParamSlot();

    void setBinding (KnobBinding);
    const KnobBinding& getBinding() const noexcept { return binding; }

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    void timerCallback() override;
    void setPosition (float);

    KnobBinding binding;
    float position = 0.0f, dragStart = 0.0f;
    juce::String valueText;
};

/** Toggle button bound to an atomic<bool> (polled). */
class BoolButton final : public juce::TextButton, private juce::Timer
{
public:
    BoolButton (const juce::String& buttonText, juce::Colour onColour = Colours::accent);
    void bind (std::atomic<bool>* p);

private:
    void timerCallback() override;
    std::atomic<bool>* param = nullptr;
};

/** Combo box bound to an atomic<int32_t> whose values are 0..n-1 (or -1 = first item when `withNone`). */
class IntCombo final : public juce::ComboBox, private juce::Timer
{
public:
    IntCombo();
    /** Items for values 0..n-1; with `noneText`, value -1 is shown as that item first. */
    void setItems (const juce::StringArray& items, const juce::String& noneText = {});
    void bind (std::atomic<int32_t>* p);
    std::function<void (int value)> onValueChosen; // instead of writing the atomic

private:
    void timerCallback() override;
    void refreshSelection();
    std::atomic<int32_t>* param = nullptr;
    bool hasNone = false;
};

} // namespace livedaw::ui
