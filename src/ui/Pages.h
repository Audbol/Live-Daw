#pragma once

#include "ui/DisplayPage.h"

#include <memory>

namespace livedaw::ui
{

class ShowPanel;

/** EFFECTS: the 16 FX slots (VST/AU/LV2 plugins), used by the strips' inserts. */
class EffectsPage final : public DisplayPage
{
public:
    explicit EffectsPage (UiContext&);
    ~EffectsPage() override;

    std::vector<KnobBinding> getKnobs() override;
    void resized() override;
    void paint (juce::Graphics&) override;

    class Tile;

private:
    UiContext& ctx;
    juce::OwnedArray<Tile> tiles;
    int selected = 0;
};

/** METERS: every signal path of the console with mute status. Click a group to
    bring it to a fader section, a meter to select its strip. */
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
        LayerSet::Section section;
        juce::String bank;
        juce::Rectangle<int> area;
    };

    void timerCallback() override;
    int count (const Group&) const;

    UiContext& ctx;
    std::vector<Group> groups;
    std::vector<int> laidOutCounts;
};

/** ROUTING: channel sources, outputs and the monitor. */
class RoutingPage final : public DisplayPage
{
public:
    explicit RoutingPage (UiContext&);
    ~RoutingPage() override;

    void refresh() override;
    void resized() override;
    void paint (juce::Graphics&) override;

    class Model;

private:
    void setMode (int);

    UiContext& ctx;
    int mode = 0;
    juce::TextButton inputsButton { "CHANNEL INPUTS" }, outputsButton { "OUTPUTS" }, monitorButton { "MONITOR" };
    std::unique_ptr<Model> model;
    juce::ListBox list;
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
    UiContext& ctx;
    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };
    ShowPanel* showPanel = nullptr;
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

    UiContext& ctx;
    juce::Label stats;
    juce::TextButton clearFaultsButton { "CLEAR FX FAULTS" }, clearClipsButton { "RESET COUNTERS" };
    juce::ListBox logList;
    juce::Rectangle<int> leftArea, rightArea;
};

} // namespace livedaw::ui
