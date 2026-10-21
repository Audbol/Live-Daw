#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <optional>

namespace livedaw::ui
{

/** How long the WING's +48V keys must be held to switch phantom power (either
    way): a tap does nothing, so a stray touch can't power a microphone input. */
constexpr double kPhantomHoldSeconds = 2.0;

/** The timing of a press-and-hold key, with the clock passed in: `press` starts
    it, `release` (letting go, or the pointer leaving the key) cancels it, and
    `update` returns true once when the key has been held for the hold time.
    Still held after that, the key does nothing more until it's pressed again. */
class HoldToSwitch
{
public:
    explicit HoldToSwitch (double holdSeconds = kPhantomHoldSeconds) : hold (holdSeconds) {}

    void press (double now)
    {
        heldSince = now;
        done = false;
    }

    void release() { heldSince.reset(); }

    /** Pressed and not yet switched. */
    bool isCounting() const noexcept { return heldSince.has_value() && ! done; }

    bool update (double now)
    {
        if (! isCounting() || now - *heldSince < hold)
            return false;
        done = true;
        return true;
    }

    /** 0 .. 1 while counting, 0 otherwise. */
    float progress (double now) const
    {
        if (! isCounting())
            return 0.0f;
        return (float) juce::jlimit (0.0, 1.0, (now - *heldSince) / hold);
    }

private:
    double hold;
    std::optional<double> heldSince;
    bool done = false;
};

/** A HoldToSwitch on the message thread's clock: runs `onSwitch` when the hold
    completes and `onProgress` (a repaint) as it counts. */
class HoldKey final : private juce::Timer
{
public:
    HoldKey (std::function<void()> switched, std::function<void()> progressed, double holdSeconds = kPhantomHoldSeconds)
        : hold (holdSeconds), onSwitch (std::move (switched)), onProgress (std::move (progressed))
    {
    }

    void press()
    {
        hold.press (now());
        startTimerHz (30);
        if (onProgress)
            onProgress();
    }

    void release()
    {
        const bool counting = hold.isCounting();
        hold.release();
        stopTimer();
        if (counting && onProgress)
            onProgress();
    }

    float progress() const { return hold.progress (now()); }

private:
    static double now() { return juce::Time::getMillisecondCounterHiRes() * 0.001; }

    void timerCallback() override
    {
        if (hold.update (now()))
        {
            stopTimer();
            // The switch may rebuild (and delete) the key: run a copy, last.
            if (onProgress)
                onProgress();
            if (auto switched = onSwitch)
                switched();
            return;
        }
        if (onProgress)
            onProgress();
    }

    HoldToSwitch hold;
    std::function<void()> onSwitch, onProgress;
};

/** A held key's progress: `colour` (what the key turns to) rising from the
    bottom of `key`, under its print. */
inline void paintHoldProgress (juce::Graphics& g, juce::Rectangle<float> key, float progress, juce::Colour colour)
{
    if (progress <= 0.0f)
        return;
    g.setColour (colour);
    g.fillRect (key.withTop (key.getBottom() - key.getHeight() * progress));
}

} // namespace livedaw::ui
