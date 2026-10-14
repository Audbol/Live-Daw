#pragma once

#include "ui/UiContext.h"

namespace livedaw::ui
{

/** The layer selector beside the faders (like the layer keys on X32/Wing):
    default banks first, then user layers. Ctrl/Cmd+1..9 select layers too. */
class LayerBar final : public juce::Component
{
public:
    static constexpr int kWidth = 108;

    explicit LayerBar (UiContext&);

    /** Rebuilds the buttons (after the console or the layers changed). */
    void refresh();

    std::function<void()> onEditLayers;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    UiContext& ctx;
    juce::OwnedArray<juce::TextButton> buttons;
    juce::TextButton editButton { "Edit layers" };
    int numDefault = 0;
};

} // namespace livedaw::ui
