#pragma once

#include "engine/FxSlots.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

namespace livedaw::ui
{

/** The content of an FX slot's plugin window, in the screen's dark style: a
    chain bar across the top (IN > chip > chip > ... > OUT, +, FIT and the
    ROW / FOCUS layout keys) over the plugins' editors as cards, in chain order
    (a plugin on its own is a chain of one).

    - A chip shows a plugin's name, a bypass dot (click: bypass), a red mark
      when faulted and its latency when not 0. Clicking it scrolls to the
      plugin's card (ROW) or shows it (FOCUS); dragging it reorders the chain;
      its right-click menu has Bypass, Clear fault, Move earlier / later, Zoom,
      Collapse and Remove.
    - A card is a slim title row (collapse, name, FAULT + CLEAR, BYP, zoom -
      % +, remove) over the editor. A collapsed card is only a thin column
      (ROW) or its title row (FOCUS); a bypassed one is dimmed.
    - ROW: the cards side by side, scrolling horizontally. FOCUS: one card at
      a time, the chips acting as tabs.

    Zoom (50 / 67 / 75 / 85 / 100 %, FIT: the largest steps that fit the
    window's height) shrinks an editor the best way it allows (ZoomMethod): a
    resizable editor is resized within its constrainer; a VST3 / VST / LV2
    editor gets the host scale factor (setScaleFactor), kept only if it got
    smaller; JUCE component editors (generic controls, the sandbox's panel,
    in-process plugins) are scaled with a transform; any other native view has
    a fixed size (the zoom keys are disabled, COLLAPSE still works). Zoom and
    collapse are kept per plugin by FxSlots (saved with the show).

    The view takes the size its content needs, up to `maxSize`; beyond that it
    scrolls. A plugin on its own whose editor is resizable follows the view's
    size. It follows the slot (a timer notices changes made elsewhere: chains,
    bypass, faults, latency) and keeps the editors of the plugins that stay.

    Message thread only. Bypass, faults, moves and zoom go straight to FxSlots;
    adding and removing go through the callbacks (they open menus, log, may
    empty the slot). */
class PluginChainView final : public juce::Component, private juce::Timer, private juce::ComponentListener
{
public:
    enum class Layout
    {
        row,
        focus
    };

    /** How a plugin's editor is made smaller (see the class comment). */
    enum class ZoomMethod
    {
        resize,
        hostScale,
        transform,
        fixed
    };

    struct Callbacks
    {
        /** + was clicked (`target`: the button, for a menu). */
        std::function<void (juce::Component& target)> addPlugin;
        /** Remove member `index` (call dropEditorFor first). */
        std::function<void (int index)> removeMember;
        /** The chain changed (names, order) or the slot became empty. */
        std::function<void()> chainChanged;
        /** ROW / FOCUS was clicked. */
        std::function<void (Layout)> layoutChanged;
    };

    PluginChainView (FxSlots&, int slot, Callbacks);
    ~PluginChainView() override;

    int getSlot() const noexcept { return slot; }
    /** The slot's plugin (or chain) the view shows. */
    juce::AudioProcessor* getShownProcessor() const noexcept { return shown; }
    /** `p` is the shown processor or one of its plugins. */
    bool shows (const juce::AudioProcessor* p) const noexcept;
    int getNumCards() const noexcept { return (int) cards.size(); }

    /** Deletes the editor of `p` (about to be destroyed). */
    void dropEditorFor (const juce::AudioProcessor* p);
    /** Re-reads the slot: cards for new plugins, order, bypass, faults, zoom, names. */
    void refresh();

    /** The largest size before it scrolls (default: most of the main display). */
    void setMaxSize (juce::Point<int> size);

    Layout getLayout() const noexcept { return layoutMode; }
    void setLayout (Layout);
    /** The plugin FOCUS shows (and ROW marks). */
    int getSelected() const noexcept { return selected; }
    /** Selects plugin `index`: shown (FOCUS) or scrolled to (ROW). */
    void select (int index);

    /** Plugin `index` one zoom step smaller (-1) or larger (+1). */
    void stepZoom (int index, int direction);
    void setZoom (int index, int percent);
    /** Every plugin that can shrink at the largest step that fits the window's height. */
    void fitToHeight();
    void setCollapsed (int index, bool);

    /** For tests: how plugin `index`'s editor is shrunk, and where its card and editor are. */
    ZoomMethod getZoomMethod (int index) const;
    juce::Rectangle<int> getCardBounds (int index) const;
    juce::Rectangle<int> getEditorBounds (int index) const;
    bool isCardVisible (int index) const;

    /** The zoom step after `percent` in `direction` (capped at 50 and 100 %). */
    static int nextZoom (int percent, int direction) noexcept;
    /** The largest zoom step at which an editor `baseHeight` px tall at 100 %
        fits `available` px (the smallest step if none does). */
    static int fitZoom (int baseHeight, int available) noexcept;

    static constexpr int kBarHeight = 34;
    static constexpr int kTitleHeight = 26;
    static constexpr int kMinCardWidth = 250;
    static constexpr int kCollapsedWidth = 28;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class Card;
    class Chip;
    class Bar;
    class Rack;
    friend class Card;
    friend class Chip;
    friend class Bar;

    void layout();
    juce::Point<int> naturalSize() const;
    juce::String signature() const;
    void timerCallback() override;
    void componentMovedOrResized (juce::Component&, bool wasMoved, bool wasResized) override;

    void toggleBypass (int index);
    void clearFault (int index);
    void move (int from, int to);
    void remove (int index);
    void showMemberMenu (int index, juce::Component& target);
    int rackHeightAvailable() const;

    FxSlots& fx;
    const int slot;
    Callbacks callbacks;
    juce::AudioProcessor* shown = nullptr;
    juce::String lastSignature;

    std::unique_ptr<Bar> bar;
    juce::Viewport viewport;
    std::unique_ptr<Rack> rack;
    std::vector<std::unique_ptr<Card>> cards;
    Layout layoutMode = Layout::row;
    int selected = 0;
    juce::Point<int> maxSize, lastNatural;
    bool layingOut = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginChainView)
};

} // namespace livedaw::ui
