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
    the surface's DAW key). */
class MainComponent final : public juce::Component
{
public:
    MainComponent (juce::AudioDeviceManager&, MixerEngine&, PluginHost&, Show&, EventLog&, juce::PropertiesFile&);
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    /** File the show was last opened from / saved to. */
    juce::File getShowFile() const { return showFile; }
    void setShowFile (const juce::File& f) { showFile = f; updateTitle(); }

    /** Reloads `file` (used by safe-mode recovery). */
    juce::Result reloadShow (const juce::File& file, bool loadPlugins);

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

    void showPluginScanner();
    void openPluginEditor (int slot);
    void closePluginWindowsFor (juce::AudioProcessor*);
    void closeAllPluginWindows();

    void newShow();
    void openShow();
    void saveShow (bool saveAs);
    void updateTitle();

    /** The multitrack editor in place of the console view, or back. */
    void showEditor (bool);
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
