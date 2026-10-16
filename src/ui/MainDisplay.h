#pragma once

#include "ui/HomePage.h"
#include "ui/Pages.h"
#include "ui/ViewPages.h"

namespace livedaw::ui
{

/** The WING's main display: the screen keys left of it (HOME, EFFECTS, METERS,
    ROUTING, SETUP, LIBRARY, UTILITY), the status bar (selected channel tab,
    audio status, scene, SETTINGS/TOOLS, clock), the current screen, and the
    parameter bar of the six knobs underneath, which follow the screen. */
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

private:
    void timerCallback() override;
    void updateKnobs();
    /** Status keys and the parameter bar follow the page. */
    void updateFrame();
    void layoutStatusBar();
    DisplayPage* page (Screen) const;
    void paintStatusBar (juce::Graphics&);

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

    juce::OwnedArray<juce::TextButton> screenButtons;
    ParamSlot slots[6];
    juce::Rectangle<int> screenArea, statusArea, slotArea;
    juce::Rectangle<int> channelTab, chipsTile, sceneTile, clockTile;
    std::vector<StatusKey> statusKeys;
    std::vector<juce::Rectangle<int>> statusKeyAreas;
    juce::String shownTime;
    juce::uint32 shownHash = 0;
};

} // namespace livedaw::ui
