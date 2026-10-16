#pragma once

#include "ui/Icons.h"
#include "ui/LookAndFeel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <functional>
#include <optional>

namespace livedaw::ui
{

/** A flat screen key like the console's: grey, optional icon left of the text,
    lit in `onColour` (with black ink) while `isOn` returns true. */
class WingKey : public juce::Button, private juce::Timer
{
public:
    WingKey (const juce::String& keyText, std::optional<Icon> keyIcon = std::nullopt, juce::Colour on = Colours::yellow)
        : juce::Button (keyText), icon (keyIcon), onColour (on)
    {
        startTimerHz (6);
    }

    std::function<bool()> isOn;
    juce::Colour offColour = juce::Colour (0xff5a5a5a);
    bool outlined = false;
    /** Icon above a small label (the library's FOLDER / RENAME / DELETE keys). */
    bool iconAbove = false;

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        const bool on = isOn != nullptr && isOn();
        auto r = getLocalBounds().toFloat();
        auto fill = on ? onColour : offColour;
        if (down)
            fill = fill.brighter (0.15f);
        else if (over)
            fill = fill.brighter (0.06f);
        if (! isEnabled())
            fill = fill.darker (0.4f);
        g.setColour (fill);
        g.fillRect (r);
        if (outlined)
        {
            g.setColour (juce::Colour (0xffbdbdbd));
            g.drawRect (r, 1.0f);
        }
        const auto ink = on ? juce::Colours::black : (isEnabled() ? Colours::text : Colours::textDim);
        if (iconAbove && icon.has_value())
        {
            auto label = r.removeFromBottom (r.getHeight() * 0.36f);
            drawIcon (g, *icon, r.reduced (r.getHeight() * 0.12f), ink);
            g.setColour (ink);
            g.setFont (juce::FontOptions (juce::jlimit (10.0f, 15.0f, label.getHeight() * 0.8f), juce::Font::bold));
            g.drawFittedText (getButtonText(), label.toNearestInt().reduced (2, 0), juce::Justification::centredTop, 1);
            return;
        }
        auto content = r.reduced (juce::jmin (10.0f, r.getHeight() * 0.18f), 0.0f);
        if (icon.has_value())
        {
            const float s = juce::jmin (r.getHeight() * 0.62f, content.getWidth() * 0.4f);
            auto iconBox = getButtonText().isEmpty() ? content.withSizeKeepingCentre (s, s) : content.removeFromLeft (s).withSizeKeepingCentre (s, s);
            drawIcon (g, *icon, iconBox, ink);
            content.removeFromLeft (6.0f);
        }
        if (getButtonText().isNotEmpty())
        {
            g.setColour (ink);
            g.setFont (juce::FontOptions (juce::jlimit (12.0f, 22.0f, r.getHeight() * 0.46f), juce::Font::bold));
            g.drawFittedText (getButtonText(), content.toNearestInt(), juce::Justification::centred, 2);
        }
    }

private:
    void timerCallback() override
    {
        if (isOn != nullptr && isOn() != lastOn)
        {
            lastOn = isOn();
            repaint();
        }
    }

    std::optional<Icon> icon;
    juce::Colour onColour;
    bool lastOn = false;
};

/** The console's "ON" chip: yellow while the bound flag is set, click toggles it. */
class OnChip final : public WingKey
{
public:
    explicit OnChip (std::atomic<bool>& flag) : WingKey ("ON"), value (flag)
    {
        isOn = [this] { return value.load(); };
        offColour = juce::Colour (0xff4b4b4b);
        outlined = true;
        onClick = [this] { value = ! value.load(); };
    }

private:
    std::atomic<bool>& value;
};

/** A row of keys of which one is lit (NORM | FINE | AUTO on the console). */
class SegmentedKeys final : public juce::Component
{
public:
    SegmentedKeys (juce::StringArray labels, std::function<int()> get, std::function<void (int)> set, juce::Colour on = Colours::yellow)
        : names (std::move (labels)), getter (std::move (get)), setter (std::move (set)), onColour (on)
    {
    }

    void paint (juce::Graphics& g) override
    {
        const int current = getter != nullptr ? getter() : -1;
        auto r = getLocalBounds().toFloat();
        g.setColour (juce::Colour (0xff1e1e1e));
        g.fillRect (r);
        const float w = r.getWidth() / (float) juce::jmax (1, names.size());
        for (int i = 0; i < names.size(); ++i)
        {
            auto k = juce::Rectangle<float> (r.getX() + w * (float) i, r.getY(), w, r.getHeight()).reduced (1.0f);
            const bool on = i == current;
            g.setColour (on ? onColour : juce::Colour (0xff4b4b4b));
            g.fillRect (k);
            g.setColour (on ? juce::Colours::black : Colours::text);
            g.setFont (juce::FontOptions (juce::jlimit (12.0f, 22.0f, k.getHeight() * 0.48f), juce::Font::bold));
            g.drawFittedText (names[i], k.toNearestInt().reduced (2, 0), juce::Justification::centred, 1);
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (names.isEmpty() || ! getLocalBounds().contains (e.getPosition()) || setter == nullptr)
            return;
        setter (juce::jlimit (0, names.size() - 1, e.x * names.size() / juce::jmax (1, getWidth())));
        repaint();
    }

private:
    juce::StringArray names;
    std::function<int()> getter;
    std::function<void (int)> setter;
    juce::Colour onColour;
};

} // namespace livedaw::ui
