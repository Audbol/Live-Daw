#pragma once

#include "ui/DisplayPage.h"
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
    int selected = 0;
};

/** METERS (manual 5.3): channels and buses in groups of eight with number keys
    in the strips' colours (select) and mute squares, AUX, DCA, MAIN and MATRIX.
    Touching a group's header opens its section's FADERS screen. Consoles with
    more than 40 channels or 16 buses page through them. */
class MetersPage final : public DisplayPage, private juce::Timer
{
public:
    explicit MetersPage (UiContext&);

    bool usesKnobs() const override { return false; }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

private:
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
    static void layoutRow (juce::Rectangle<int> row, float unit, std::vector<Group*> left, std::vector<Group*> centre, std::vector<Group*> right);
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

    UiContext& ctx;
    std::vector<Group> groups;
    juce::Rectangle<int> titleArea, prevKey, nextKey, scaleTop, scaleBottom;
    int page = 0;
    std::vector<int> laidOutCounts;
    std::unordered_map<const void*, Hold> holds;
};

/** ROUTING (manual 5.4), switched with the status-bar keys CHANNELS, SOURCES
    and OUTPUTS. CHANNELS: pick a channel on the left, then its source on the
    right (+1 AUTO moves on to the next channel; the lock protects the patch).
    SOURCES: the inputs and which channels use them. OUTPUTS: pick an output on
    the left, then the signal that feeds it (a strip's L / R, or the monitor). */
class RoutingPage final : public DisplayPage, private juce::Timer
{
public:
    explicit RoutingPage (UiContext&);
    ~RoutingPage() override;

    bool usesKnobs() const override { return false; }
    std::vector<StatusKey> getStatusKeys() override;
    StatusTitle getStatusTitle() override;
    void refresh() override;
    void resized() override;
    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    enum class Mode
    {
        Channels,
        Sources,
        Outputs
    };

    /** A signal that can feed an output: a strip's left or right channel, or the monitor (strip == nullptr). */
    struct Signal
    {
        Strip* strip = nullptr;
        int channel = 0;
    };

    void timerCallback() override;
    void setMode (Mode);
    std::vector<Strip*> channelTargets() const;
    std::vector<Signal> signals() const;
    int numTargetPages() const;
    int numPorts (bool outputs) const;
    int numPortGroups (bool outputs) const;
    juce::String portName (int index, bool output) const;
    Strip* selectedStrip() const;
    void select (Strip*);
    void layoutTiles();
    void assignInput (int port, bool right);
    void assignOutput (int port, const Signal&);
    void clearOutput (int port);
    /** What feeds output `port` (strip nullptr + channel -1: nothing). */
    Signal feederOf (int port) const;
    juce::String signalLabel (const Signal&) const;
    juce::String inputUsers (int port) const;
    /** Source groups: the hardware inputs in groups of 48, then the recorder's
        playback channels (PLAY 1-48, PLAY 49-64). */
    int numSourceGroups() const;
    juce::String sourceGroupName (int group) const;
    /** The input index of tile `index` of a source group (-1 past its end). */
    int sourceAt (int group, int index) const;
    /** "IN 3" (or the port's name), "PLAY 5", "NONE". */
    juce::String sourceName (int source) const;
    void showGroupMenu (bool rightSide);
    /** The invisible component the drop-down lists open under, moved over `area`. */
    juce::Component& anchorAt (juce::Rectangle<int> area);

    void paintChannelTile (juce::Graphics&, juce::Rectangle<int>, Strip&, bool selected) const;
    void paintInputPort (juce::Graphics&, juce::Rectangle<int>, int port, bool marked) const;
    void paintOutputPort (juce::Graphics&, juce::Rectangle<int>, int port, bool selected) const;
    void paintSignal (juce::Graphics&, juce::Rectangle<int>, const Signal&, bool marked) const;
    void paintSourcePanel (juce::Graphics&) const;

    UiContext& ctx;
    Mode mode = Mode::Channels;
    int targetPage = 0, inputGroup = 0, outputGroup = 0, signalGroup = 2, signalPage = 0, layoutVersion = -1;
    int selectedPort = 0, selectedSource = 0;
    StripKind selectedKind = StripKind::Channel;
    int selectedIndex = 0;
    bool autoAdvance = true, locked = false;
    bool editAlt = false; // CHANNEL INPUT MAIN / ALT: which input the source grid patches

    juce::Rectangle<int> leftPanel, rightPanel, leftHeader, rightHeader;
    juce::Rectangle<int> inputField, stereoKey, autoKey, lockKey, unlockKey, prevPage, nextPage, leftSelector, rightSelector,
        noneKey, copyCaption, srcToChKey, chToSrcKey, separator, renameKey;
    std::vector<std::pair<juce::Rectangle<int>, Strip*>> channelTiles;
    std::vector<juce::Rectangle<int>> leftPorts, rightPorts;
    std::vector<std::pair<juce::Rectangle<int>, Signal>> signalTiles;
    juce::Component anchor;
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

    UiContext& ctx;
    TabRow tabRow;
    std::unique_ptr<juce::Component> general;
    juce::OwnedArray<juce::Component> ownedPages;
    std::vector<juce::Component*> pages;
    WingKey initKey { "INITIALIZE\nCONSOLE", Icon::Reset }, shutdownKey { "SHUTDOWN", Icon::Power };
    OverlayHolder overlays;
    std::unique_ptr<DisplayPage> subScreen;
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
    class PlaceholderTab;
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

    UiContext& ctx;
    std::unique_ptr<ShowPanel> list;
    TabRow tabs;
    juce::OwnedArray<juce::Component> tabPages;
    WingKey goPrevKey { "GO\nPREV", Icon::GoPrev }, goNextKey { "GO\nNEXT", Icon::GoNext }, prevKey { "PREV", Icon::ChevronLeft },
        nextKey { "NEXT", Icon::ChevronRight }, goKey { "GO", Icon::Play };
    std::unique_ptr<juce::Component> scope;
    juce::Rectangle<int> rightPanel, transportArea;

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

    struct HealthRow
    {
        juce::String label, value;
        int state = 0; // 0 fine, 1 note, 2 fault
    };

    UiContext& ctx;
    const bool onSetup;
    std::vector<HealthRow> health;
    WingKey clearFaultsButton { "CLEAR FX FAULTS", Icon::ClearCircle }, clearClipsButton { "RESET COUNTERS", Icon::Reset };
    juce::ListBox logList;
    juce::Rectangle<int> leftArea, rightArea, statsArea;
};

} // namespace livedaw::ui
