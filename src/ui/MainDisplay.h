#pragma once

#include "ui/HomePage.h"
#include "ui/Pages.h"
#include "ui/ViewPages.h"
#include "ui/daw/RecorderPage.h"

namespace livedaw::ui
{

/** The WING's main display and its frame (manual 4.1, Fig. 8): the seven
    screen keys left of it over its full height (HOME, EFFECTS, METERS,
    ROUTING, SETUP, LIBRARY, UTILITY); right of it CLR SOLO, the main L/R
    meter, the knob for fine adjustments and the blank key; and the 16:10
    screen itself: the status bar (the selected channel's tab in its colour,
    the status tiles, the screen's own keys, the clock), the current screen, and
    the parameter bar of the six knobs underneath, which follow the screen. */
class MainDisplay final : public juce::Component, private juce::Timer
{
public:
    explicit MainDisplay (UiContext&);
    ~MainDisplay() override;

    void showScreen (Screen);
    void showHomeTab (HomeTab);
    /** EFFECTS, with slot `slot` open in the device editor. */
    void editFx (int slot);
    Screen getScreen() const noexcept { return screen; }

    /** Selection or structure changed. */
    void refresh();
    /** Scenes changed. */
    void refreshLibrary();

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;

    class ParamBarSlot;
    class ContextKnob;
    class MainMeter;
    class Highlight;
    class ChannelSelect;
    class FocusTracker;

private:
    void timerCallback() override;
    void updateKnobs();
    /** Status keys and the parameter bar follow the page. */
    void updateFrame();
    void layoutStatusBar();
    DisplayPage* page (Screen) const;
    void paintStatusBar (juce::Graphics&);
    void clearSolo();
    void showChannelSelect (bool);
    /** The knob right of the display adjusts this (the last touched control). */
    void setFocus (const KnobBinding&, juce::Component* target);

    UiContext& ctx;
    Screen screen = Screen::Home;
    std::unique_ptr<HomePage> home;
    std::unique_ptr<EffectsPage> effects;
    std::unique_ptr<MetersPage> meters;
    std::unique_ptr<RoutingPage> routing;
    std::unique_ptr<SetupPage> setup;
    std::unique_ptr<LibraryPage> library;
    std::unique_ptr<UtilityPage> utility;
    std::unique_ptr<ViewPage> overview, faders, sends, config, muteGroups;
    std::unique_ptr<RecorderPage> recorder;

    juce::OwnedArray<juce::TextButton> screenButtons;
    juce::OwnedArray<ParamBarSlot> slots;
    std::unique_ptr<juce::TextButton> clearSoloKey, blankKey;
    std::unique_ptr<MainMeter> mainMeter;
    std::unique_ptr<ContextKnob> contextKnob;
    std::unique_ptr<Highlight> highlight;
    std::unique_ptr<ChannelSelect> channelSelect;
    std::unique_ptr<FocusTracker> focusTracker;

    juce::Rectangle<int> screenArea, statusArea, slotArea, pageArea;
    juce::Rectangle<int> channelTab, numberBox, chipsTile, usbTile, liveTile, clockTile;
    std::vector<StatusKey> statusKeys;
    std::vector<juce::Rectangle<int>> statusKeyAreas;
    juce::String shownTime;
    juce::uint32 shownHash = 0;
    int touchedSlot = -1;
    bool blink = false;
};

} // namespace livedaw::ui
