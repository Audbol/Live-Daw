#pragma once

#include "ui/PluginChainView.h"
#include "ui/UiContext.h"

namespace livedaw::ui
{

/** The plugin window of an FX slot (opened with ctx.openPluginEditor): one
    window with the slot's chain bar and the editors of all its plugins, in
    chain order (PluginChainView). + adds a plugin (the plugin list); the chips
    and title rows bypass, clear, move, zoom, collapse and remove plugins. The
    title names the chain; the window grows to fit and scrolls beyond the
    screen. It is resizable; its size and its layout (ROW / FOCUS) are kept per
    slot in the settings. It closes itself when the slot empties. */
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
    void resized() override;

private:
    void chainChanged();
    void requestClose();
    void showAddMenu (juce::Component& target);
    void removeMember (int index);
    juce::String settingsKey (const char* what) const;

    UiContext& ctx;
    PluginChainView view;
    std::function<void (PluginChainWindow&)> onClose;
    bool remembering = false; // the size is saved once the window is set up

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginChainWindow)
};

} // namespace livedaw::ui
