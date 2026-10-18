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

/** A draggable divider between two areas. A vertical bar is dragged left/right,
    a horizontal one up/down. Reports incremental drag deltas. */
class ResizerBar : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit ResizerBar (bool isVertical) : vertical (isVertical)
    {
        setMouseCursor (vertical ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::UpDownResizeCursor);
        setRepaintsOnMouseActivity (true);
    }

    std::function<void (int delta)> onDrag;
    std::function<void()> onDragEnd, onDoubleClick;

    void paint (juce::Graphics& g) override
    {
        const bool hot = isMouseOverOrDragging();
        auto b = getLocalBounds().toFloat();
        g.setColour (hot ? Colours::accent.withAlpha (0.6f) : Colours::outline.withAlpha (0.5f));

        // A short grip in the middle of the bar.
        const auto grip = vertical ? juce::Rectangle<float> (2.0f, 36.0f).withCentre (b.getCentre())
                                   : juce::Rectangle<float> (36.0f, 2.0f).withCentre (b.getCentre());
        g.fillRoundedRectangle (grip, 1.0f);

        if (hot)
            g.fillRect (vertical ? b.withSizeKeepingCentre (1.0f, b.getHeight()) : b.withSizeKeepingCentre (b.getWidth(), 1.0f));
    }

    void mouseDown (const juce::MouseEvent&) override { lastDistance = 0; }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        const int distance = vertical ? e.getDistanceFromDragStartX() : e.getDistanceFromDragStartY();
        if (distance != lastDistance && onDrag)
            onDrag (distance - lastDistance);
        lastDistance = distance;
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (onDragEnd)
            onDragEnd();
    }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        if (onDoubleClick)
            onDoubleClick();
    }

private:
    bool vertical;
    int lastDistance = 0;
};

/** The gain reduction of a dynamics slot as the WING draws it on its GR
    meters (FADERS page, block thumbnails): a `GR` caption with the value over
    a segmented amber bar on a dark well that fills from the left, with the
    scale under it, and for a gate the square that lights green while it is
    open (0 .. maxDb). */
class GainReductionMeter : public juce::Component, private juce::Timer
{
public:
    /** `read` returns the current reduction in dB (>= 0) and resets the source. */
    explicit GainReductionMeter (std::function<float()> read, float maxDb = 24.0f)
        : reader (std::move (read)), range (maxDb)
    {
        startTimerHz (30);
    }

    /** Set for a gate: true while the gate is open (the square lights green). */
    std::function<bool()> isOpen;

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        const float screenH = getWingScreenHeight();
        auto top = b.removeFromTop (b.getHeight() * 0.42f);
        g.setFont (wingFontPx (juce::jmin (TypeScale::caption * screenH, top.getHeight() * 0.7f)));
        g.setColour (Colours::caption);
        g.drawText ("GR", top, juce::Justification::centredLeft, false);
        g.setColour (Colours::text);
        g.drawText ((value >= 0.05f ? "-" : "") + juce::String (value, 1) + " dB", top, juce::Justification::centredRight, false);
        auto scale = b.removeFromBottom (b.getHeight() * 0.38f);
        auto bar = b.reduced (0.0f, 1.0f);
        if (isOpen != nullptr)
        {
            auto square = bar.removeFromLeft (bar.getHeight());
            g.setColour (open ? Colours::meterGreen : Colours::screenTile);
            g.fillRect (square);
            bar.removeFromLeft (3.0f);
            scale.removeFromLeft (square.getWidth() + 3.0f);
        }
        g.setColour (Colours::screenWell);
        g.fillRect (bar);
        const int segments = juce::jmax (8, (int) (bar.getWidth() / 4.0f));
        const float w = bar.getWidth() / (float) segments;
        const float lit = juce::jlimit (0.0f, 1.0f, value / range) * (float) segments;
        for (int i = 0; i < segments; ++i)
        {
            g.setColour ((float) i + 0.5f < lit ? Colours::amber : Colours::screenTile);
            g.fillRect (bar.getX() + (float) i * w, bar.getY() + 1.0f, juce::jmax (1.0f, w - 1.0f), bar.getHeight() - 2.0f);
        }
        g.setColour (Colours::textDim);
        g.setFont (wingFontPx (juce::jmin (TypeScale::meterScale * screenH, scale.getHeight() * 0.8f)));
        for (float db : { 0.0f, range * 0.25f, range * 0.5f, range * 0.75f, range })
        {
            const float x = scale.getX() + scale.getWidth() * db / range;
            auto just = db <= 0.0f ? juce::Justification::centredLeft : db >= range ? juce::Justification::centredRight : juce::Justification::centred;
            auto cell = juce::Rectangle<float> (24.0f, scale.getHeight()).withX (db <= 0.0f ? x : db >= range ? x - 24.0f : x - 12.0f);
            g.drawText (juce::String (juce::roundToInt (db)), cell.withY (scale.getY()), just, false);
        }
    }

private:
    void timerCallback() override
    {
        const float v = reader();
        value = v > value ? v : value * 0.85f + v * 0.15f;
        const bool nowOpen = isOpen != nullptr && isOpen();

        const int shown = juce::roundToInt (value * 10.0f);
        if (shown != lastShown || nowOpen != open)
        {
            lastShown = shown;
            open = nowOpen;
            repaint();
        }
    }

    std::function<float()> reader;
    float range, value = 0.0f;
    int lastShown = -1;
    bool open = false;
};

} // namespace livedaw::ui
