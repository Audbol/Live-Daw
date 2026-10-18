#pragma once

#include "ui/ScreenWidgets.h"

namespace livedaw::ui
{

/** The LIBRARY's SCOPE window (manual 5.6, fig. 47), which covers the screen
    below the status bar: strip tiles (CHANNEL, AUX, BUS, MAIN, MATRIX), DCA,
    MUTEGROUP and FX, the CONFIGURATION grid and the yellow CONTENTS keys, with
    NONE / ALL / CANCEL and SAVE.

    The same window edits a scene's recall scope (EDIT SCOPE), the scope of a
    PARTIAL LOAD (with NO GLOB) or PARTIAL UPDATE, a snippet's REC FOCUS and
    its ADD / REMOVE ITEMS, and the GLOBAL safes (strips: their recall safe;
    the rest: the show's global safes). CONFIG in CONFIGURATION is the
    console-wide settings (monitor setup, INPUT SELECT, automix enables); the
    other configuration tiles (surface, preferences, sections) are drawn
    disabled. CONTENTS keys that share one of our sections light together. */
class ScopeWindow final : public juce::Component
{
public:
    enum class Mode
    {
        SceneScope,
        SnippetFocus,
        SnippetAdd,
        SnippetRemove,
        Global,
        PartialLoad,
        PartialUpdate
    };

    ScopeWindow (UiContext&, Mode, juce::String title, state::Scope);

    /** SAVE (LOAD for a partial load); `noGlobal` is the NO GLOB key. */
    std::function<void (const state::Scope&, bool noGlobal)> onSave;
    std::function<void()> onCancel;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    enum class Kind
    {
        Strip,
        Dca,
        MuteGroup,
        Fx,
        Config,
        Content
    };

    struct Tile
    {
        Kind kind;
        juce::Rectangle<int> area;
        juce::String caption, number;
        state::StripRef strip {};
        juce::uint32 bits = 0; // Content: sections; Dca, MuteGroup, Fx: the item's bit; Config: 1 = CONFIG
        bool dropdown = false;
    };

    struct Group
    {
        juce::String title;
        juce::Rectangle<int> header;
        size_t first = 0, count = 0;
    };

    bool isActive (const Tile&) const;
    bool isSelected (const Tile&) const;
    void toggle (const Tile&, bool on);
    bool hasStrip (const state::StripRef&) const;
    void toggleStrip (const state::StripRef&, bool on);
    juce::uint32* maskOf (Kind);

    UiContext& ctx;
    Mode mode;
    juce::String title;
    state::Scope selection;
    bool noGlobal = false;
    std::vector<Tile> tiles;
    std::vector<Group> groups;
    juce::Rectangle<int> titleArea, configHeader, contentsHeader;
    WingKey noneKey { "NONE", Icon::ListNone }, allKey { "ALL", Icon::ListAll }, cancelKey { "CANCEL", Icon::ClearCircle },
        noGlobKey { "NO GLOB", Icon::LockOpen }, saveKey { "SAVE", Icon::SaveTray };
};

} // namespace livedaw::ui
