#pragma once

#include "ui/Icons.h"
#include "ui/Knobs.h"
#include "ui/UiContext.h"

#include <optional>
#include <vector>

namespace livedaw::ui
{

/** A key at the right of the status bar. The WING changes these with the screen
    (INIT / COPY / SETTINGS on HOME, MONITORS / CC EDIT on SETUP, GLOBAL on
    LIBRARY, the screen keys of the VIEW screens...). */
struct StatusKey
{
    juce::String text;
    std::optional<Icon> icon;
    bool lit = false;
    std::function<void()> onClick;
};

/** A screen of the main display. */
class DisplayPage : public juce::Component
{
public:
    ~DisplayPage() override = default;

    /** The six knobs under the display for what this page shows (fewer is fine). */
    virtual std::vector<KnobBinding> getKnobs() { return {}; }

    /** False for screens that don't use the six knobs: the display then hides
        the parameter bar and gives the page its space. */
    virtual bool usesKnobs() const { return true; }

    /** The keys this screen puts at the right of the status bar, left to right
        (the clock always follows). Empty: the display's default keys. */
    virtual std::vector<StatusKey> getStatusKeys() { return {}; }

    /** The selected strip or the console structure changed. */
    virtual void refresh() {}

    /** Called by the page when its knob assignment changed. */
    std::function<void()> knobsChanged;

    /** Called by the page when its status keys or `usesKnobs()` changed. */
    std::function<void()> statusKeysChanged;

protected:
    void notifyKnobsChanged()
    {
        if (knobsChanged)
            knobsChanged();
    }

    void notifyStatusKeysChanged()
    {
        if (statusKeysChanged)
            statusKeysChanged();
    }
};

} // namespace livedaw::ui
