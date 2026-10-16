#pragma once

#include "ui/DisplayPage.h"

#include <memory>
#include <unordered_map>

namespace livedaw::ui
{

class ShowPanel;

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

    /** Shows slot `slot` in the device editor (-1 = back to the rack). */
    void editSlot (int slot);

    class Tile;
    class Editor;

private:
    UiContext& ctx;
    juce::OwnedArray<Tile> tiles;
    std::unique_ptr<Editor> editor;
    juce::Rectangle<int> rack, premiumLabel, standardLabel;
    int selected = 0;
};

/** METERS: like the console's meter screen. Channels and buses in groups of
    eight with number keys (select) and M keys (mute), AUX, DCA, MAIN and MATRIX.
    Consoles with more than 40 channels or 16 buses page through them. */
class MetersPage final : public DisplayPage, private juce::Timer
{
public:
    explicit MetersPage (UiContext&);

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
    void bringToSection (const Group&, Strip*);

    UiContext& ctx;
    std::vector<Group> groups;
    juce::Rectangle<int> titleArea, prevKey, nextKey, scaleTop, scaleBottom;
    int page = 0;
    std::vector<int> laidOutCounts;
    std::unordered_map<const void*, Hold> holds;
};

/** ROUTING: like the console's routing screen. CHANNELS: channel and aux
    tiles on the left, a group of 48 sources on the right; pick a channel, then
    its source (+1 AUTO moves on to the next channel, the lock protects the
    patch). OUTPUTS: the same for the strips' outputs. MONITOR: monitor outputs,
    PFL/AFL and level. */
class RoutingPage final : public DisplayPage, private juce::Timer
{
public:
    explicit RoutingPage (UiContext&);
    ~RoutingPage() override;

    void refresh() override;
    void resized() override;
    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    enum class Mode
    {
        Channels,
        Outputs,
        Monitor
    };

    struct TargetTile
    {
        juce::Rectangle<int> area;
        Strip* strip;
    };

    void timerCallback() override;
    void setMode (Mode);
    std::vector<Strip*> allTargets() const;
    int numTargetPages() const;
    int numPorts() const;
    int numPortGroups() const;
    juce::String portName (int index) const;
    Strip* selectedStrip() const;
    void select (Strip*);
    void layoutTiles();
    void assign (int port, bool right);
    void showGroupMenu();
    juce::String portUsers (int port, bool& usedBySelected) const;

    void paintTarget (juce::Graphics&, const TargetTile&, bool selected) const;
    void paintPort (juce::Graphics&, juce::Rectangle<int>, int port) const;

    UiContext& ctx;
    Mode mode = Mode::Channels;
    int targetPage = 0, portGroup = 0, layoutVersion = -1;
    StripKind selectedKind = StripKind::Channel;
    int selectedIndex = 0;
    bool autoAdvance = true, locked = false;

    juce::Rectangle<int> modeKeys[3], leftPanel, rightPanel, leftHeader, rightHeader;
    juce::Rectangle<int> monoKey, stereoKey, autoKey, lockKey, unlockKey, prevPage, nextPage, groupSelector, offKey, separator;
    std::vector<TargetTile> targetTiles;
    std::vector<juce::Rectangle<int>> portTiles;
    std::unique_ptr<juce::Component> monitorPanel;
};

/** SETUP: audio device, console size, surface, global safes, group names, plugins. */
class SetupPage final : public DisplayPage
{
public:
    explicit SetupPage (UiContext&);
    ~SetupPage() override;

    void refresh() override;
    void resized() override;
    void paint (juce::Graphics&) override;

private:
    UiContext& ctx;
    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };
};

/** LIBRARY: show files, scenes and snippets, user fader banks. */
class LibraryPage final : public DisplayPage
{
public:
    explicit LibraryPage (UiContext&);
    ~LibraryPage() override;

    void refresh() override;
    void resized() override;
    void paint (juce::Graphics&) override;

    /** Shows the scenes tab. */
    void showScenes();

private:
    /** Tabs that report the change of tab. */
    struct Tabs final : public juce::TabbedComponent
    {
        Tabs() : juce::TabbedComponent (juce::TabbedButtonBar::TabsAtTop) {}
        std::function<void (int)> onTabChanged;
        void currentTabChanged (int index, const juce::String&) override
        {
            if (onTabChanged)
                onTabChanged (index);
        }
    };

    UiContext& ctx;
    Tabs tabs;
    ShowPanel* showPanel = nullptr;
    std::unique_ptr<juce::Component> ownedShowPanel;
};

/** UTILITY: system health and the event log. */
class UtilityPage final : public DisplayPage, private juce::Timer, private juce::ChangeListener, private juce::ListBoxModel
{
public:
    explicit UtilityPage (UiContext&);
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
    std::vector<HealthRow> health;
    juce::TextButton clearFaultsButton { "CLEAR FX FAULTS" }, clearClipsButton { "RESET COUNTERS" };
    juce::ListBox logList;
    juce::Rectangle<int> leftArea, rightArea, statsArea;
};

} // namespace livedaw::ui
