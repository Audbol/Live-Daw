#pragma once

#include "engine/FxSlots.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

namespace livedaw::ui
{

/** The content of an FX slot's plugin window: the editors of the slot's
    plugins side by side in chain order (a plugin on its own is a chain of
    one). Each editor has a header strip: the plugin's name, BYP, move left and
    right, remove; a + at the end adds a plugin. A plugin without an editor gets
    generic controls; a sandboxed plugin's editor is the sandbox's (SHOW PLUGIN
    WINDOW and generic controls).

    The view takes the size its editors need, up to `maxSize`; beyond that it
    scrolls. A single plugin whose editor is resizable follows the view's size
    (the window's). It follows the slot (a timer notices chains changed
    elsewhere) and keeps the editors of the plugins that stay.

    Message thread only. Bypass and moves go straight to FxSlots; adding and
    removing go through the callbacks (they open menus, log, may empty the slot). */
class PluginChainView final : public juce::Component, private juce::Timer, private juce::ComponentListener
{
public:
    struct Callbacks
    {
        /** + was clicked (`target`: the button, for a menu). */
        std::function<void (juce::Component& target)> addPlugin;
        /** Remove member `index` (call dropEditorFor first). */
        std::function<void (int index)> removeMember;
        /** The chain changed (names, order) or the slot became empty. */
        std::function<void()> chainChanged;
    };

    PluginChainView (FxSlots&, int slot, Callbacks);
    ~PluginChainView() override;

    int getSlot() const noexcept { return slot; }
    /** The slot's plugin (or chain) the view shows. */
    juce::AudioProcessor* getShownProcessor() const noexcept { return shown; }
    /** `p` is the shown processor or one of its plugins. */
    bool shows (const juce::AudioProcessor* p) const noexcept;
    int getNumColumns() const noexcept { return (int) columns.size(); }
    /** Several plugins (they scroll), or one with a resizable editor. */
    bool wantsResizableWindow() const;

    /** Deletes the editor of `p` (about to be destroyed). */
    void dropEditorFor (const juce::AudioProcessor* p);
    /** Re-reads the slot: columns for new plugins, order, bypass, names. */
    void refresh();

    /** The largest size before it scrolls (default: most of the main display). */
    void setMaxSize (juce::Point<int> size);

    static constexpr int kHeaderHeight = 28;
    static constexpr int kPlusWidth = 40;
    static constexpr int kMinColumnWidth = 220;
    static constexpr int kGap = 4;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class Column;
    class Row;

    void layout();
    juce::Point<int> naturalSize() const;
    juce::String signature() const;
    void timerCallback() override;
    void componentMovedOrResized (juce::Component&, bool wasMoved, bool wasResized) override;

    FxSlots& fx;
    const int slot;
    Callbacks callbacks;
    juce::AudioProcessor* shown = nullptr;
    juce::String lastSignature;

    juce::Viewport viewport;
    std::unique_ptr<Row> row;
    std::vector<std::unique_ptr<Column>> columns;
    juce::TextButton plus { "+" };
    juce::Point<int> maxSize, lastNatural;
    bool layingOut = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginChainView)
};

} // namespace livedaw::ui
