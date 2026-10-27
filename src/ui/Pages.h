#pragma once

#include "ui/DisplayPage.h"
#include "ui/HoldToSwitch.h"
#include "ui/RoutingGroups.h"
#include "ui/ScreenWidgets.h"
#include "ui/ShowPanel.h"
#include "ui/WingPopups.h"

#include <memory>
#include <unordered_map>

namespace livedaw::ui
{

/** EFFECTS: the console's FX rack. 16 slots (VST/AU/LV2 plugins) used by the
    strips' inserts; slots 1-8 are the premium column, 9-16 the standard one.
    Each loaded slot shows its source strip, PRE/POST, ON and its mix. */
class EffectsPage final : public DisplayPage
{
public:
    explicit EffectsPage (UiContext&);
    ~EffectsPage() override;

    std::vector<KnobBinding> getKnobs() override;
    /** The rack has no parameter bar [3.1]; a slot's editor has. */
    bool usesKnobs() const override;
    void resized() override;
    void paint (juce::Graphics&) override;

    /** The blank key beside the display taps the tempo of a delay open in the editor. */
    bool hasBlankKeyFunction() const override { return tapParam() >= 0; }
    void blankKeyPressed() override;

    /** Shows slot `slot` in the device editor (-1 = back to the rack). */
    void editSlot (int slot);

    class Tile;
    class Editor;

private:
    /** The TIME (ms) parameter of the device open in the editor, or -1. */
    int tapParam() const;

    UiContext& ctx;
    juce::OwnedArray<Tile> tiles;
    std::unique_ptr<Editor> editor;
    double lastTap = 0.0;
    juce::Rectangle<int> rack, premiumLabel, standardLabel;
    juce::Image metal; // the rack's background at its size
};

/** METERS (manual 5.3, laid out as firmware 3.1): channels and buses in
    groups of eight with number keys in the strips' colours (select) and M
    keys (mute), AUX, DCA, MAIN and MATRIX, a CLIP..-60 scale per row. The
    SETTINGS key in the status bar sets PRE or POST per kind of strip (the
    settings SETUP > SURFACE > METERING also holds). Touching a group's header
    opens its section's FADERS screen. Consoles with more than 40 channels or
    16 buses page through them. */
class MetersPage final : public DisplayPage, private juce::Timer
{
public:
    explicit MetersPage (UiContext&);
    ~MetersPage() override;

    bool usesKnobs() const override { return false; }
    std::vector<StatusKey> getStatusKeys() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    class Settings;

    struct Group
    {
        juce::String title;
        StripKind kind;
        bool dca = false;
        int first = 0, count = 0;
        LayerSet::Section section;
        juce::Rectangle<int> header, body;
    };

    struct Hold
    {
        float level = 0.0f;
        int frames = 0;
    };

    void timerCallback() override;
    static float unitsOf (const std::vector<Group*>&);
    void layoutRow (int headerY, float k, std::vector<Group*> left, std::vector<Group*> centre, std::vector<Group*> right);
    int numPages() const;
    juce::Rectangle<int> numberRow (const Group&) const;
    juce::Rectangle<int> muteRow (const Group&) const;
    juce::Rectangle<float> slot (const Group&, int index) const;
    float levelOf (const Group&, int index, int channel) const;
    bool isStereo (const Group&, int index) const;
    bool isMuted (const Group&, int index) const;
    Strip* stripOf (const Group&, int index) const;
    void paintScale (juce::Graphics&, juce::Rectangle<int> column, const Group& reference) const;
    void bringToSection (const Group&, int index);
    void showSettings (bool);
    /** A length in WING pixels (1280 x 720 page) across and down the page. */
    float px (float wingX) const { return wingX * (float) getWidth() / 1280.0f; }
    float py (float wingY) const { return wingY * (float) getHeight() / 720.0f; }

    UiContext& ctx;
    std::unique_ptr<Settings> settings;
    std::vector<Group> groups;
    juce::Rectangle<int> titleArea, prevKey, nextKey, scaleTop, scaleBottom, scaleHeaderTop, scaleHeaderBottom;
    float slotW = 23.0f; // a strip's width in the groups (23 WING pixels unless squeezed)
    int page = 0;
    std::vector<int> laidOutCounts;
    juce::uint64 shownHash = 0;
    std::unordered_map<const void*, Hold> holds;
};

/** ROUTING (manual 5.4), switched with the status-bar keys CHANNELS, SOURCES
    and OUTPUTS, laid out as the console's (firmware 3.1). CHANNELS: pick a
    channel on the left, then its source on the right (+1 AUTO moves on to the
    next channel; the lock protects the patch). SOURCES: the inputs of a group
    and the selected one's settings. OUTPUTS: pick an output on the left, then
    the signal that feeds it. The groups are the WING's (routing::Group), with
    Sidecar's own ports in the ones they stand in for. */
class RoutingPage final : public DisplayPage, private juce::Timer
{
public:
    explicit RoutingPage (UiContext&);
    ~RoutingPage() override;

    bool usesKnobs() const override { return sourcesUseKnobs(); }
    std::vector<StatusKey> getStatusKeys() override;
    StatusTitle getStatusTitle() override;
    void refresh() override;
    void resized() override;
    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;

    /** The lock: true while the patch can't be changed (CHANNELS, OUTPUTS). */
    bool isLocked() const noexcept { return lock.isLocked(); }
    void setLocked (bool);

private:
    using Group = routing::Group;
    class GroupMenu;

    enum class Mode
    {
        Channels,
        Sources,
        Outputs
    };

    /** A signal that can feed an output: a strip's left or right channel,
        the monitor (strip == nullptr, channel 0-3) or a source (`source`, an
        input index, manual 5.4 OUTPUTS). Channel -1: an n/a tile. */
    struct Signal
    {
        Strip* strip = nullptr;
        int channel = 0;
        int source = -1;
    };

    void timerCallback() override;
    void setMode (Mode);
    std::vector<Strip*> channelTargets() const;
    /** The tiles of the OUTPUTS source group (n/a tiles for groups without signals here). */
    std::vector<Signal> signals() const;
    int numTargetPages() const;
    int numHardware (bool outputs) const;
    Strip* selectedStrip() const;
    /** The source table index of the selected channel's MAIN (or, editing
        ALT, ALT) input, -1 if none. */
    int patchedSourceIndex() const;
    void select (Strip*);
    void layoutTiles();
    void assignInput (int port, bool right);
    void assignOutput (int port, const Signal&);
    void clearOutput (int port);
    /** What feeds output `port` (strip nullptr + channel -1: nothing). */
    Signal feederOf (int port) const;
    juce::String signalLabel (const Signal&) const;
    /** The channels patched to a source, each with true when it's their ALT input. */
    std::vector<std::pair<Strip*, bool>> sourceUsers (int port) const;
    /** A source's name (RENAME), else its label ("LCL 3"). */
    juce::String sourceName (int source) const;
    /** False for an interface port the interface doesn't have (drawn n/a). */
    bool sourcePresent (int source) const;
    void showGroupMenu (bool rightSide);
    void chooseGroup (bool rightSide, int group);
    /** The invisible component the drop-down lists open under, moved over `area`. */
    juce::Component& anchorAt (juce::Rectangle<int> area);

    void paintHeaders (juce::Graphics&) const;
    void paintChannelTile (juce::Graphics&, juce::Rectangle<int>, Strip&, bool selected) const;
    void paintSourceTile (juce::Graphics&, juce::Rectangle<int>, Group, int index, bool marked, int slot) const;
    void paintOutputTile (juce::Graphics&, juce::Rectangle<int>, Group, int index, bool selected) const;
    void paintSignal (juce::Graphics&, juce::Rectangle<int>, const Signal&, int index, bool marked) const;
    void paintSourcePanel (juce::Graphics&) const;

    UiContext& ctx;
    Mode mode = Mode::Channels;
    Group inputGroup = Group::LocalIn, outputGroup = Group::LocalOut, signalGroup = Group::Bus;
    int targetPage = 0, signalPage = 0, layoutVersion = -1;
    int selectedPort = 0, selectedSource = 0;
    StripKind selectedKind = StripKind::Channel;
    int selectedIndex = 0;
    // The console's defaults: +1 AUTO off; the patch locked (the closed lock
    // lit red), on a fresh console and after INITIALIZE CONSOLE.
    bool autoAdvance = false;
    routing::PatchLock lock { ctx.engine };
    bool editAlt = false; // CHANNEL INPUT MAIN / ALT: which input the source grid patches

    juce::Rectangle<int> leftPanel, rightPanel, leftHeader, rightHeader, leftGrid, rightGrid;
    juce::Rectangle<int> inputCaption, mainKey, altKey, monoKey, stereoKey, midSideKey, autoKey, lockFrame, lockKey, unlockKey, prevPage, nextPage, leftSelector,
        rightSelector, noneKey, copyCaption, srcToChKey, chToSrcKey, separator, renameKey, meterLegend, prevSignals, nextSignals;
    std::vector<std::pair<juce::Rectangle<int>, Strip*>> channelTiles;
    std::vector<juce::Rectangle<int>> leftPorts, rightPorts;
    std::vector<std::pair<juce::Rectangle<int>, Signal>> signalTiles;
    juce::Component anchor;
    std::unique_ptr<GroupMenu> groupMenu;

public:
    // SOURCES settings (fig. 31): the parameter bar holds the source's GAIN.
    bool sourcesUseKnobs() const noexcept { return mode == Mode::Sources; }
    std::vector<KnobBinding> getKnobs() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    /** The source table entry of an input port (nullptr if it has none). */
    ld::SourceParams* sourceParams (int port) const;
    void layoutSourcePanel();
    void paintSourceSettings (juce::Graphics&) const;
    /** The GAIN (or an oscillator's LEVEL) fader with its value flag. */
    void paintLevelFader (juce::Graphics&, juce::Rectangle<int>, float value, float min, float max) const;
    void paintSourceLook (juce::Graphics&) const;
    void paintSourceTags (juce::Graphics&) const;
    void selectSource (int port);
    void renameSource();
    void setSourceGainFromY (int y);
    /** Switches the selected source's +48V (both of a pair), when its key has been held. */
    void switchPhantom();
    /** The selected source's oscillator (OSC 1 / 2; a pair's is OSC 1's), else nullptr. */
    ld::OscParams* selectedOscillator() const;
    void showOscFreqMenu();
    /** The selected USR source: a user signal (USR 1-24) or a user patch (USR 25-56), else nullptr. */
    ld::UserSignalParams* selectedUserSignal() const;
    ld::UserPatchParams* selectedUserPatch() const;
    /** What the selected user signal / patch copies ("CH 3", "BUS 12", "LCL 5", "OFF"). */
    juce::String userSourceText() const;
    /** USER SIGNAL's SOURCE: the group, then the strip (a user signal) or the port (a user patch). */
    void showUserSourceMenu();
    /** Applies `set` to every selected source (with each one's pair partner) and its table index. */
    void forSelectedSources (const std::function<void (int index, ld::SourceParams&)>& set);
    /** SOURCES: the tiles from the drag's first to `tile` are selected (manual 5.4: "Touch and drag to select multiple Sources"). */
    void dragSelectTo (int tile);
    bool isSelectedSource (int port) const;

    int sourceTab = 0; // SETTINGS, ICON/COLOR, TAGS
    std::array<juce::Rectangle<int>, 3> sourceTabKeys, sourceModeKeys;
    // The SETTINGS boxes: INITIALIZE, the source (group, number, MONO / STEREO
    // / MID/SIDE), REMOTE, RCV CUST and the meter, +48V, POL, MUTE and GAIN.
    juce::Rectangle<int> sourceTabBar, sourceBanner, sourceBody, initBox, sourceInitKey, sourceBox, modeBox, remoteBox, rcvBox, rcvKey, meterBox,
        sourceMeter, sourcePhantomKey, sourcePolKey, sourceMuteKey, sourceGainFader;
    std::array<juce::Rectangle<int>, 4> remoteKeys;
    // An OSC source: SINE / PINK / WHITE in the REMOTE column, FREQ in place of
    // +48V and POL, LEVEL in place of GAIN.
    std::array<juce::Rectangle<int>, 3> oscModeKeys;
    juce::Rectangle<int> oscFreqField, oscLevelFader;
    // A USR source: L+R / L / R in the REMOTE column and TAP (PRE / POST) in
    // place of RCV CUST (user signals), SOURCE in place of +48V.
    std::array<juce::Rectangle<int>, 3> userSideKeys;
    juce::Rectangle<int> userTapKey, userSourceField;
    // SOURCES' selection: the sources besides selectedSource that parameter
    // changes also go to, and the tile a drag started on (-1 none).
    std::vector<int> selectedSources;
    int dragStartTile = -1;
    bool dragSelected = false;
    /** The GAIN fader, or an OSC source's LEVEL fader. */
    juce::Rectangle<int> levelFader() const;
    std::vector<std::pair<juce::Rectangle<int>, int>> sourceColourCells, sourceIconCells;
    // SOURCE SOLO's SRC SOLO key (fig. 36) and SOURCES > TAGS (the mute groups).
    juce::Rectangle<int> srcSoloKey;
    std::vector<std::pair<juce::Rectangle<int>, int>> sourceTagCells;
    std::vector<float> sourcePeaks; // per tile of the source grid, decaying
    float selectedSourcePeak = 0.0f;
    int refreshTicks = 0;
    bool draggingGain = false;
    HoldKey phantomHold { [this] { switchPhantom(); }, [this] { repaint (sourcePhantomKey); } };
};

/** SETUP (manual 5.5): the tabs GENERAL | AUDIO | SURFACE | REMOTE | DAW (and
    the app's PLUGINS) with INITIALIZE CONSOLE and SHUTDOWN on the tab row;
    MONITORS and CC EDIT in the status bar. Settings are changed by touch, so
    the parameter bar is hidden. */
class SetupPage final : public DisplayPage
{
public:
    explicit SetupPage (UiContext&);
    ~SetupPage() override;

    /** Settings are changed by touch; CUSTOM CONTROLS and MONITORS use the knobs. */
    bool usesKnobs() const override { return subScreen != nullptr && subScreen->usesKnobs(); }
    std::vector<KnobBinding> getKnobs() override;
    std::vector<KnobStyle> getKnobStyles() override;
    std::vector<StatusKey> getStatusKeys() override;
    StatusTitle getStatusTitle() override;
    void refresh() override;
    void resized() override;
    void paint (juce::Graphics&) override;
    void visibilityChanged() override;
    /** SETUP pressed again: CC EDIT from the tabs, back to the tabs from CC EDIT or MONITORS. */
    bool screenKeyPressedAgain() override;

    /** CUSTOM CONTROLS (CC EDIT), at a layer when >= 0. */
    void openCustomControls (int layer);
    /** The MONITORS screen. */
    void openMonitors();

private:
    void showTab();
    /** Shows a screen of SETUP in place of its tabs (nullptr: back to the tabs). */
    void showSubScreen (std::unique_ptr<DisplayPage>);
    void closeSubScreen();
    /** INITIALIZE CONSOLE: the page choosing what to reset. */
    void openInitialize();
    void closeInitialize();
    void initialize (const state::Scope&, juce::uint32 sourceGroups, bool everything);

    UiContext& ctx;
    TabRow tabRow;
    std::unique_ptr<juce::Component> general;
    juce::OwnedArray<juce::Component> ownedPages;
    std::vector<juce::Component*> pages;
    WingKey initKey { "INITIALIZE\nCONSOLE", Icon::Reset }, shutdownKey { "SHUTDOWN", Icon::Power };
    OverlayHolder overlays;
    std::unique_ptr<DisplayPage> subScreen;
    std::unique_ptr<juce::Component> initWindow; // a ScopeWindow
    juce::Rectangle<int> tabBar;
};

/** LIBRARY (manual 5.6): the open show's scenes and snippets on the left
    (ShowPanel), the tabs SHOW | SNAP | SNIP | CLIP | FX | CHAN on the right
    with the GO PREV / GO NEXT / PREV / NEXT / GO transport, the SCOPE window,
    and the GLOBAL (safes) key in the status bar. */
class LibraryPage final : public DisplayPage, private juce::Timer
{
public:
    explicit LibraryPage (UiContext&);
    ~LibraryPage() override;

    bool usesKnobs() const override { return false; }
    std::vector<StatusKey> getStatusKeys() override;
    StatusTitle getStatusTitle() override;
    void refresh() override;
    void resized() override;
    void paint (juce::Graphics&) override;

    /** Shows the SNAP tab. */
    void showScenes();

    class ShowTab;
    class SnapTab;
    class SnipTab;
    class ClipTab;
    class FxTab;
    class ChanTab;

private:
    enum Tab
    {
        TabShow,
        TabSnap,
        TabSnip,
        TabClip,
        TabFx,
        TabChan
    };

    void timerCallback() override;
    void showTab (int);
    void updateTransport();
    /** The scene of the selected row (or the current scene). */
    int selectedScene() const;
    /** Runs `action` after the CONFIRM LIBRARY LOAD question, if that's on. */
    void confirmLoad (const juce::String& what, std::function<void()> action);
    void load (ShowPanel::Item);
    void openScope (int mode, const juce::String& title, state::Scope initial, std::function<void (const state::Scope&, bool noGlobal)> save);
    void closeScope();
    void openGlobal();
    bool anyGlobalSafe() const;
    /** EXPORT on SNAP / SNIP: the selected scene (with its snippets) or snippet
        as a file in the folder shown at INT / USB (INT's folder at SHOW). */
    void exportSelected (bool snippet);
    /** IMPORT on SNAP / SNIP: the selected scene or snippet file (a file dialog
        when none is selected) into the show. */
    void importSelected (bool snippet);
    /** A scene file goes in after the selected scene, a snippet file into it. */
    void importFile (const juce::File&);
    /** The WING snapshot (.snap) selected at INT / USB, if any. */
    juce::File selectedWingSnapshot() const;
    /** Applies a WING snapshot (.snap) to the console, or adds a WING show's
        (.show) snapshots as scenes; the log gets what could not be applied. */
    void applyWingFile (const juce::File&);

    UiContext& ctx;
    std::unique_ptr<ShowPanel> list;
    TabRow tabs;
    juce::OwnedArray<juce::Component> tabPages;
    WingKey goPrevKey { "GO\nPREV", Icon::GoPrev }, goNextKey { "GO\nNEXT", Icon::GoNext }, prevKey { "PREV", Icon::ChevronLeft },
        nextKey { "NEXT", Icon::ChevronRight }, goKey { "GO", Icon::Play };
    std::unique_ptr<juce::Component> scope;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::Rectangle<int> rightPanel, transportArea, tabBar;

    // The snippet REC FOCUS: what SAVE on the SNIP tab captures (or keeps of a recording).
    state::Scope focus = state::Scope::none();
    bool focusEdited = false;
    bool shownGlobal = false;
    juce::String shownTitle;
};

/** System health and the event log (SETUP > LOG). */
class UtilityPage final : public DisplayPage, private juce::Timer, private juce::ChangeListener, private juce::ListBoxModel
{
public:
    /** `onSetup`: the SETUP > LOG tab. Otherwise this is the main display's
        UTILITY page, which the UTILITY key never shows (it opens SETUP > LOG:
        the WING's UTILITY key has no screen of its own). */
    explicit UtilityPage (UiContext&, bool onSetup = false);
    ~UtilityPage() override;

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    juce::String getTooltipForRow (int row) override;

    struct HealthRow
    {
        juce::String label, value;
        int state = 0; // 0 fine, 1 note, 2 fault
    };

    UiContext& ctx;
    const bool onSetup;
    std::vector<HealthRow> health;
    WingKey clearFaultsButton { "CLEAR FX FAULTS", Icon::ClearCircle }, clearClipsButton { "RESET COUNTERS", Icon::Reset },
        aboutButton { "ABOUT", Icon::Info };
    juce::ListBox logList;
    juce::Rectangle<int> leftArea, rightArea, statsArea;
};

} // namespace livedaw::ui
