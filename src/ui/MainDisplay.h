#pragma once

#include "ui/HomePage.h"
#include "ui/Pages.h"
#include "ui/ViewPages.h"
#include "ui/daw/RecorderPage.h"
#include "ui/UsbRecorderPage.h"

namespace livedaw::ui
{

/** The WING's main display and its frame (manual 4.1, Fig. 8): the seven
    screen keys left of it over its full height (HOME, EFFECTS, METERS,
    ROUTING, SETUP, LIBRARY, UTILITY); right of it CLR SOLO, the main L/R
    meter, the knob for fine adjustments and the blank key; the 16:10 screen
    itself: the status bar (the title tile, the status tiles, the screen's own
    keys, the clock), the current screen, and the parameter bar of the six
    knobs underneath, which follow the screen; and the six touch encoders
    under the screen.

    The screen keys, CLR SOLO and the blank key (ALT) also form the console's
    shortcuts: hold CLR SOLO and press UTILITY for a screenshot, hold HOME to
    lock the surface (with other keys beside the display held as a password),
    hold SETUP + ALT and press CLR SOLO to turn touch off
    (ALT + CLR SOLO turns it on again), hold ALT and turn the knob for the
    display's brightness. A right-click "holds" a key for a few seconds. */
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

    /** The knob right of the display adjusts `binding` from now on, and the
        screen outlines `target` (TOUCH FADER SELECT, a touched knob). */
    void setFineTarget (const KnobBinding& binding, juce::Component* target);

    /** USE CRSR/WHEEL: moves the focus frame over the screen's controls (the
        parameter bar's slots, then the knobs on the page): dx steps through
        them in order, dy moves to the nearest one above or below. */
    void moveFocus (int dx, int dy);
    /** Turns the focused control by `steps` detents. */
    void turnFocus (int steps);

    /** Touch off (SETUP + ALT + CLR SOLO): the screen ignores the mouse and the
        status bar shows a small X. */
    void setTouchDisabled (bool);

    /** Surface lock (MainComponent, SurfaceLock): the screen, the knobs and
        the meter ignore input, the keys beside the display only unlock;
        with SHOW METER PAGE WHEN LOCKED the METERS screen shows meanwhile. */
    void setLocked (bool);
    bool isLocked() const noexcept { return locked; }
    bool isTouchDisabled() const noexcept { return touchDisabled; }

    /** Screen only (the main component shows nothing else): the display, its
        keys and knobs as one compact unit centred on black, the screen as
        large as fits; the key under the right-hand column reads RESTORE. */
    void setMaximized (bool);
    bool isMaximized() const noexcept { return maximized; }

    /** The width the display unit (keys, the 16:10 screen as tall as `height`
        allows, its right-hand column) wants at that height: more than this
        only leaves empty panel beside it. */
    static int preferredWidth (int height);
    /** The height the display unit needs for a screen `screenHeight` px tall. */
    static int heightForScreen (int screenHeight);
    /** The smallest screen height whose body text (TypeScale::body) is still
        about 9 px: the console view keeps the screen at least this tall when
        the window allows (it is the screen of a 1680 x 1000 window). */
    static constexpr int kLegibleScreenHeight = 360;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;
    /** On an empty part of the status bar: screen only, or back. */
    void mouseDoubleClick (const juce::MouseEvent&) override;

    class ParamBarSlot;
    class ContextKnob;
    class MainMeter;
    class Highlight;
    class ChannelSelect;
    class FocusTracker;
    class ConsoleKey;
    class Overlay;
    class Encoder;

private:
    void timerCallback() override;
    void updateKnobs();
    /** Status keys and the parameter bar follow the page. */
    void updateFrame();
    void layoutStatusBar();
    DisplayPage* page (Screen) const;
    void paintStatusBar (juce::Graphics&);
    void paintTitleTile (juce::Graphics&);
    void paintClock (juce::Graphics&);
    void clearSolo();
    void showChannelSelect (bool);
    /** The knob right of the display adjusts this (the last touched control). */
    void setFocus (const KnobBinding&, juce::Component* target);
    void screenKeyPressed (Screen);
    void utilityPressed();
    void clearSoloPressed();
    /** The focus model's targets, in order: the parameter bar's slots, then
        the knobs on the page (top to bottom, left to right). */
    std::vector<juce::Component*> focusTargets() const;
    void applyLighting();
    juce::Rectangle<int> wingRect (float x, float y, float w, float h) const;

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
    std::unique_ptr<UsbRecorderPage> usbRecorder;

    juce::OwnedArray<ConsoleKey> screenButtons;
    juce::OwnedArray<ParamBarSlot> slots;
    juce::OwnedArray<Encoder> encoders;
    std::unique_ptr<ConsoleKey> clearSoloKey, blankKey, maximizeKey;
    std::unique_ptr<MainMeter> mainMeter;
    std::unique_ptr<ContextKnob> contextKnob;
    std::unique_ptr<Highlight> highlight;
    std::unique_ptr<ChannelSelect> channelSelect;
    std::unique_ptr<FocusTracker> focusTracker;
    std::unique_ptr<Overlay> popupLayer, touchBlocker, lcdScrim, lockBlocker;
    Screen screenBeforeLock = Screen::Home;
    bool locked = false;

    juce::Rectangle<int> frameArea, screenArea, statusArea, slotArea, pageArea, encoderArea;
    juce::Rectangle<int> channelTab, numberBox, chipsTile, usbTile, liveTile, clockTile;
    std::vector<StatusKey> statusKeys;
    std::vector<juce::Rectangle<int>> statusKeyAreas;
    StatusTitle shownTitle;
    juce::String shownTime;
    juce::uint32 shownHash = 0;
    int touchedSlot = -1;
    int focusIndex = -1;
    int acknowledgedFaults = 0;
    bool blink = false, flash = false, touchDisabled = false, liveWritable = true, maximized = false;
    int ticks = 0;
};

} // namespace livedaw::ui
