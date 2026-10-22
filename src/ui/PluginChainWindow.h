#pragma once

#include "ui/PluginChainView.h"
#include "ui/UiContext.h"

namespace livedaw::ui
{

/** The plugin window of an FX slot (opened with ctx.openPluginEditor): one
    window with the editors of all the slot's plugins side by side, in chain
    order (PluginChainView). + adds a plugin (the plugin list), the header strips
    bypass, move and remove plugins. The title names the chain; the window grows
    to fit and scrolls beyond the screen. It closes itself when the slot empties. */
class PluginChainWindow final : public juce::DocumentWindow
{
public:
    /** `onClose` deletes the window (called asynchronously). */
    PluginChainWindow (UiContext&, int slot, std::function<void (PluginChainWindow&)> onClose);
    ~PluginChainWindow() override;

    int getSlot() const noexcept { return view.getSlot(); }
    /** The slot's plugin or chain this window shows (it closes when that goes). */
    juce::AudioProcessor* getShownProcessor() const noexcept { return view.getShownProcessor(); }
    /** `p` is about to be destroyed: a plugin of the chain loses its editor. */
    void dropEditorFor (const juce::AudioProcessor* p) { view.dropEditorFor (p); }

    void closeButtonPressed() override;

private:
    void chainChanged();
    void requestClose();
    void showAddMenu (juce::Component& target);
    void removeMember (int index);

    UiContext& ctx;
    PluginChainView view;
    std::function<void (PluginChainWindow&)> onClose;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginChainWindow)
};

} // namespace livedaw::ui
