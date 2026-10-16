#pragma once

#include "ui/HomePage.h"
#include "ui/Pages.h"

namespace livedaw::ui
{

/** The WING's main display: status bar, the screen buttons on the left (HOME,
    EFFECTS, METERS, ROUTING, SETUP, LIBRARY, UTILITY), the current screen and
    the six knobs underneath, which follow what the screen shows. */
class MainDisplay final : public juce::Component, private juce::Timer
{
public:
    explicit MainDisplay (UiContext&);
    ~MainDisplay() override;

    void showScreen (Screen);
    void showHomeTab (HomeTab);
    Screen getScreen() const noexcept { return screen; }

    /** Selection or structure changed. */
    void refresh();
    /** Scenes changed. */
    void refreshLibrary();

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void updateKnobs();
    DisplayPage* page (Screen) const;

    UiContext& ctx;
    Screen screen = Screen::Home;
    std::unique_ptr<HomePage> home;
    std::unique_ptr<EffectsPage> effects;
    std::unique_ptr<MetersPage> meters;
    std::unique_ptr<RoutingPage> routing;
    std::unique_ptr<SetupPage> setup;
    std::unique_ptr<LibraryPage> library;
    std::unique_ptr<UtilityPage> utility;

    juce::OwnedArray<juce::TextButton> screenButtons;
    DisplayKnob knobs[6];
    juce::Rectangle<int> statusArea, knobArea;
    juce::String statusLeft, statusRight;
    juce::Colour statusColour;
    bool alert = false;
};

} // namespace livedaw::ui
