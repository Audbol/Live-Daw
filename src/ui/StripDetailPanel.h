#pragma once

#include "ui/UiContext.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace livedaw::ui
{

/** The "selected channel" view found on digital consoles: input stage, gate,
    4-band EQ, compressor, plugin inserts and sends/routing for one strip. */
class StripDetailPanel final : public juce::Component
{
public:
    explicit StripDetailPanel (UiContext&);
    ~StripDetailPanel() override;

    /** Shows `strip` (nullptr shows a placeholder). Rebuilds all controls. */
    void setStrip (Strip* strip);
    Strip* getStrip() const noexcept { return current; }

    /** Rebuilds the controls for the current strip (e.g. after buses changed). */
    void refresh() { setStrip (current); }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class Content;

    UiContext& ctx;
    Strip* current = nullptr;

    juce::Viewport viewport;
    std::unique_ptr<Content> content;
};

} // namespace livedaw::ui
