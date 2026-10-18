#pragma once

#include "daw/Session.h"
#include "engine/MidiRemote.h"
#include "ui/Controls.h"
#include "ui/DawSurface.h"
#include "ui/FaderSection.h"
#include "ui/FourChannelSection.h"
#include "ui/LookAndFeel.h"
#include "ui/MainDisplay.h"
#include "ui/MeterBank.h"
#include "ui/SurfaceSections.h"
#include "ui/UiContext.h"

#include <juce_audio_utils/juce_audio_utils.h>

namespace livedaw::ui
{

class DawEditor;

/** The window: a WING surface. Top: the channel strip section, the main
    display and the monitoring panel. Bottom: the left (12) and center (8)
    fader sections, the custom controls section and the right (4) fader
    section, each fader section with its layer keys. The multitrack editor
    can take the whole window instead (Ctrl+E, EDIT on the recorder screen,
    the surface's DAW key). The main display can take the whole window too
    (F10, its maximize key): the rest of the surface stays alive underneath. */
class MainComponent final : public juce::Component
{
public:
    MainComponent (juce::AudioDeviceManager&, MixerEngine&, PluginHost&, Show&, EventLog&, juce::PropertiesFile&);
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    void focusGained (FocusChangeType) override;

    /** File the show was last opened from / saved to. */
    juce::File getShowFile() const { return showFile; }
    void setShowFile (const juce::File& f) { showFile = f; updateTitle(); }

    /** Reloads `file` (used by safe-mode recovery). */
    juce::Result reloadShow (const juce::File& file, bool loadPlugins);

    /** The start-up notice (about the project and how it differs from a
        WING), unless it was turned off for this version. */
    void showStartupNotice();

    /** Full screen (F11): the window covers the screen without its title bar
        (kiosk mode). Works together with the screen-only mode (F10). */
    void setFullScreen (bool);
    bool isFullScreen() const noexcept { return fullScreen; }
    /** The window's state from before full screen (what to save and restore). */
    juce::String getWindowStateBeforeFullScreen() const { return windowStateBeforeFullScreen; }

private:
    class PluginWindow;
    class LockOverlay;
    class SoloWatcher;

    void selectStrip (Strip*);
    void saveScreenshot();
    void setSurfaceLocked (bool);
    Strip* getSelectedStrip() const;
    void refreshAll();
    void refreshSections();
    void layoutTopRow();
    void layoutSections();
    /** The top row's height: the splitter's share, a little more on wide
        windows (the screen gains most from it). */
    int topRowHeight() const;
    /** The fader strips' width and whether the custom controls fit beside
        them, for a bottom row `total` wide and `height` tall. */
    struct BottomRowPlan
    {
        int stripW = 40;
        bool withCc = true;
    };
    BottomRowPlan planBottomRow (int total, int height) const;
    /** The console's columns: the whole window, or on very wide windows the
        width the sections use at their natural size, centred. */
    /** The widths of the top row's sections beside the display unit. */
    struct TopRowSides
    {
        int monitor, strip, fourChannel;
        int total() const noexcept { return monitor + strip + fourChannel; }
    };
    static TopRowSides topRowSideWidths (int width);
    juce::Rectangle<int> consoleArea() const;

    void showPluginScanner();
    void openPluginEditor (int slot);
    void closePluginWindowsFor (juce::AudioProcessor*);
    void closeAllPluginWindows();

    void newShow();
    void openShow();
    void openShowFile (const juce::File&);
    void saveShowTo (juce::File);
    void saveShow (bool saveAs);
    void updateTitle();

    /** The multitrack editor in place of the console view, or back. */
    void showEditor (bool);
    /** Only the main display, as large as the window allows (F10), or back. */
    void setScreenMaximized (bool);
    /** Shows the parts that the editor and the screen-only mode leave. */
    void updateVisibility();
    /** Reopens the last recording project (offering its autosave). */
    void openLastProject();

    juce::AudioDeviceManager& deviceManager;
    MixerEngine& engine;
    Show& show;
    EventLog& log;
    juce::PropertiesFile& settings;
    MeterBank meterBank;
    UiContext ctx;
    std::unique_ptr<daw::Session> recording;
    std::unique_ptr<cc::Assignments> customControlAssignments;
    std::unique_ptr<MidiRemote> midiRemote; // MIDI remote control and DAW remote control
    std::unique_ptr<DawEditor> editor;
    bool editorShown = false;
    bool screenMaximized = false, fullScreen = false;
    /** Screen only: the display's layout height; taller windows scale it up. */
    static constexpr float kMaxScreenOnlyLayoutHeight = 1100.0f;
    juce::String windowStateBeforeFullScreen;
    juce::uint32 lastFullScreenSwitch = 0;

    ConsoleLookAndFeel lookAndFeel;
    juce::TooltipWindow tooltips { this };

    state::StripRef selected;
    bool hasSelection = false;

    std::unique_ptr<ChannelStripSection> channelStrip;
    std::unique_ptr<MainDisplay> display;
    std::unique_ptr<MonitorSection> monitor;
    std::unique_ptr<FourChannelSection> fourChannel;
    std::unique_ptr<FaderSection> sections[LayerSet::kNumSections];
    std::unique_ptr<CustomControlSection> customControls;
    std::unique_ptr<DawFaderSection> dawSections[LayerSet::kNumSections]; // the fader sections in DAW mode
    bool ccInBottomRow = true; // else docked under the monitoring panel
    std::unique_ptr<LockOverlay> lockOverlay;
    std::unique_ptr<SoloWatcher> soloWatcher;
    ResizerBar splitter { false };
    float topFraction = 0.52f;

    juce::OwnedArray<PluginWindow> pluginWindows;
    std::unique_ptr<juce::DocumentWindow> scannerWindow;
    std::unique_ptr<juce::FileChooser> fileChooser;
    juce::File showFile;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace livedaw::ui
