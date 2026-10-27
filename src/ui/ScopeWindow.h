#pragma once

#include "ui/ScreenWidgets.h"

#include <set>

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
    disabled. CONTENTS keys that share one of our sections light together.

    SETUP's INITIALIZE CONSOLE page is the same window (Mode::Initialize)
    with the SOURCE and OUTPUT groups and INIT: Sidecar's source groups take
    the console's LCL (the audio interface's inputs), CRD (the multitrack
    playback channels), PLAY (the USB player) and OSC; the other source and
    output groups are drawn disabled. */
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
        PartialUpdate,
        Initialize
    };

    /** The SOURCE groups of INITIALIZE CONSOLE, as bits of getSourceGroups(). */
    enum SourceGroup
    {
        SourceLocal = 1 << 0,    // LCL: the audio interface's inputs
        SourceCard = 1 << 7,     // CRD: the multitrack playback channels
        SourcePlayer = 1 << 9,   // PLAY: the USB player
        SourceUser = 1 << 11,    // USR: the user signals and user patches
        SourceOsc = 1 << 12      // OSC: the oscillators
    };
    juce::uint32 getSourceGroups() const noexcept { return sourceGroups; }

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
        Content,
        Source,
        Output
    };

    struct Tile
    {
        Kind kind;
        juce::Rectangle<int> area;
        juce::String caption, number;
        state::StripRef strip {};
        juce::uint32 bits = 0; // Content: sections; Dca, MuteGroup, Fx, Source: the item's bit; Config: 1 = CONFIG
        bool dropdown = false;
        std::optional<Icon> icon {}; // Config and Content keys: an icon over the caption
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
    /** A SOURCE / OUTPUT tile's group (its index in the group list). */
    static int groupOf (const Tile&);
    /** GLOBAL SAFE: how many of a SOURCE / OUTPUT group's items are safe. */
    int ioSafeCount (const Tile&) const;

    UiContext& ctx;
    Mode mode;
    juce::String title;
    state::Scope selection;
    bool noGlobal = false;
    juce::uint32 sourceGroups = 0;
    /** GLOBAL SAFE: the safe sources (table indices) and outputs (ports), written back on SAVE. */
    std::set<int> safeSources, safeOutputs;
    std::vector<Tile> tiles;
    std::vector<Group> groups;
    juce::Rectangle<int> titleArea, configHeader, contentsHeader, configBox, contentsBox;
    WingKey noneKey { "NONE", Icon::ListNone }, allKey { "ALL", Icon::ListAll }, cancelKey { "CANCEL", Icon::ClearCircle },
        noGlobKey { "NO GLOB", Icon::LockOpen }, saveKey { "SAVE", Icon::SaveTray };
};

} // namespace livedaw::ui
