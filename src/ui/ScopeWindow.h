#pragma once

#include "ui/ScreenWidgets.h"

namespace livedaw::ui
{

/** The LIBRARY's SCOPE window (manual 5.6, fig. 47), which covers the screen
    below the status bar: strip tiles (CHANNEL, AUX, BUS, MAIN, MATRIX), DCA,
    MUTEGROUP and FX, the CONFIGURATION grid and the yellow CONTENTS keys, with
    NONE / ALL / CANCEL and SAVE.

    The same window edits a scene's recall scope (EDIT SCOPE), a snippet's
    REC FOCUS and its ADD / REMOVE ITEMS, and the GLOBAL safes. A scene's scope
    is a set of console sections for every strip, so its strip tiles are
    disabled (per-strip scene scope is engine item E15); CONTENTS keys that
    share one of our sections light together. */
class ScopeWindow final : public juce::Component
{
public:
    enum class Mode
    {
        SceneScope,
        SnippetFocus,
        SnippetAdd,
        SnippetRemove,
        Global
    };

    struct Selection
    {
        juce::uint32 sections = 0;
        std::vector<state::StripRef> strips;
    };

    ScopeWindow (UiContext&, Mode, juce::String title, Selection);

    std::function<void (const Selection&)> onSave;
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
        juce::uint32 bits = 0;
        bool dropdown = false;
    };

    struct Group
    {
        juce::String title;
        juce::Rectangle<int> header;
        size_t first = 0, count = 0;
    };

    bool stripsActive() const noexcept { return mode != Mode::SceneScope; }
    bool contentsActive() const noexcept { return mode != Mode::Global; }
    bool isSelected (const Tile&) const;
    bool hasStrip (const state::StripRef&) const;
    void toggleStrip (const state::StripRef&, bool on);

    UiContext& ctx;
    Mode mode;
    juce::String title;
    Selection selection;
    std::vector<Tile> tiles;
    std::vector<Group> groups;
    juce::Rectangle<int> titleArea, configHeader, contentsHeader;
    WingKey noneKey { "NONE" }, allKey { "ALL" }, cancelKey { "CANCEL" }, saveKey { "SAVE", Icon::SaveTray };
};

} // namespace livedaw::ui
