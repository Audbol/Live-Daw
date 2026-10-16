#pragma once

#include "ui/Knobs.h"
#include "ui/UiContext.h"

#include <vector>

namespace livedaw::ui
{

/** A screen of the main display. */
class DisplayPage : public juce::Component
{
public:
    ~DisplayPage() override = default;

    /** The six knobs under the display for what this page shows (fewer is fine). */
    virtual std::vector<KnobBinding> getKnobs() { return {}; }

    /** The selected strip or the console structure changed. */
    virtual void refresh() {}

    /** Called by the page when its knob assignment changed. */
    std::function<void()> knobsChanged;

protected:
    void notifyKnobsChanged()
    {
        if (knobsChanged)
            knobsChanged();
    }
};

} // namespace livedaw::ui
