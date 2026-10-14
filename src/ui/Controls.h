#pragma once

#include "ui/LookAndFeel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <functional>

namespace livedaw::ui
{

/** Slider bound to an atomic parameter. It writes on change and polls the atomic so
    edits made elsewhere (other views, session loads) show up automatically. */
class ParamSlider : public juce::Slider, private juce::Timer
{
public:
    ParamSlider (std::atomic<float>& p, double min, double max, double interval,
                 const juce::String& suffix, SliderStyle style = RotaryHorizontalVerticalDrag)
        : param (p)
    {
        setSliderStyle (style);
        setRange (min, max, interval);
        setTextValueSuffix (suffix);
        setTextBoxStyle (style == LinearVertical ? NoTextBox : TextBoxBelow, false, 70, 14);
        setValue (param.load(), juce::dontSendNotification);
        setDoubleClickReturnValue (true, juce::jlimit (min, max, static_cast<double> (param.load())));
        onValueChange = [this] { param.store (static_cast<float> (getValue())); };
        startTimerHz (15);
    }

    void setSkewAround (double centre) { setSkewFactorFromMidPoint (centre); }

private:
    void timerCallback() override
    {
        if (! isMouseButtonDown())
        {
            const auto v = static_cast<double> (param.load());
            if (std::abs (v - getValue()) > 1.0e-6)
                setValue (v, juce::dontSendNotification);
        }
    }

    std::atomic<float>& param;
};

/** Toggle button bound to an atomic<bool>. */
class ParamButton : public juce::TextButton, private juce::Timer
{
public:
    ParamButton (std::atomic<bool>& p, const juce::String& buttonText, juce::Colour onColour = Colours::accent)
        : juce::TextButton (buttonText), param (p)
    {
        setClickingTogglesState (true);
        setColour (buttonOnColourId, onColour);
        setToggleState (param.load(), juce::dontSendNotification);
        onClick = [this] { param.store (getToggleState()); };
        startTimerHz (15);
    }

private:
    void timerCallback() override
    {
        if (getToggleState() != param.load())
            setToggleState (param.load(), juce::dontSendNotification);
    }

    std::atomic<bool>& param;
};

/** Combo box bound to an atomic<int>. Item ids are value + 2, so -1 ("none") is id 1. */
class ParamCombo : public juce::ComboBox, private juce::Timer
{
public:
    explicit ParamCombo (std::atomic<int>& p) : param (p)
    {
        onChange = [this]
        {
            if (getSelectedId() > 0)
                param.store (getSelectedId() - 2);
        };
        startTimerHz (10);
    }

    void addValue (const juce::String& text, int value) { addItem (text, value + 2); }

    void refresh() { setSelectedId (param.load() + 2, juce::dontSendNotification); }

private:
    void timerCallback() override
    {
        if (getSelectedId() != param.load() + 2)
            refresh();
    }

    std::atomic<int>& param;
};

/** A knob with a caption above it. */
class Knob : public juce::Component
{
public:
    Knob (const juce::String& caption, std::atomic<float>& p, double min, double max,
          double interval, const juce::String& suffix)
        : slider (p, min, max, interval, suffix)
    {
        label.setText (caption, juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        label.setFont (juce::FontOptions (11.0f));
        label.setColour (juce::Label::textColourId, Colours::textDim);
        addAndMakeVisible (label);
        addAndMakeVisible (slider);
    }

    void resized() override
    {
        auto b = getLocalBounds();
        label.setBounds (b.removeFromTop (13));
        slider.setBounds (b);
    }

    ParamSlider slider;

private:
    juce::Label label;
};

/** Small horizontal meter showing gain reduction in dB (0 .. maxDb). */
class GainReductionMeter : public juce::Component, private juce::Timer
{
public:
    /** `read` returns the current reduction in dB (>= 0) and resets the source. */
    explicit GainReductionMeter (std::function<float()> read, float maxDb = 24.0f)
        : reader (std::move (read)), range (maxDb)
    {
        startTimerHz (30);
    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        g.setColour (juce::Colours::black.withAlpha (0.5f));
        g.fillRoundedRectangle (b, 2.0f);
        const float w = b.getWidth() * juce::jlimit (0.0f, 1.0f, value / range);
        g.setColour (Colours::solo);
        g.fillRoundedRectangle (b.removeFromRight (w), 2.0f);
        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (10.0f));
        g.drawText ("GR " + juce::String (value, 1), getLocalBounds().reduced (3, 0), juce::Justification::centredLeft);
    }

private:
    void timerCallback() override
    {
        const float v = reader();
        value = v > value ? v : value * 0.85f + v * 0.15f;

        const int shown = juce::roundToInt (value * 10.0f);
        if (shown != lastShown)
        {
            lastShown = shown;
            repaint();
        }
    }

    std::function<float()> reader;
    float range, value = 0.0f;
    int lastShown = -1;
};

} // namespace livedaw::ui
