#pragma once

#include "ui/Controls.h"
#include "ui/FaderSection.h"
#include "ui/LookAndFeel.h"
#include "ui/MainDisplay.h"
#include "ui/MeterBank.h"
#include "ui/SurfaceSections.h"
#include "ui/UiContext.h"

#include <juce_audio_utils/juce_audio_utils.h>

namespace livedaw::ui
{

/** The window: a WING surface. Top: the channel strip section, the main
    display and the monitor / show control section. Bottom: the left (12),
    center (8) and right (4) fader sections with their banks. */
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

    void selectStrip (Strip*);
    Strip* getSelectedStrip() const;
    void refreshAll();
    void refreshSections();
    void layoutSections();

    void showPluginScanner();
    void openPluginEditor (int slot);
    void closePluginWindowsFor (juce::AudioProcessor*);
    void closeAllPluginWindows();

    void newShow();
    void openShow();
    void saveShow (bool saveAs);
    void updateTitle();

    juce::AudioDeviceManager& deviceManager;
    MixerEngine& engine;
    Show& show;
    EventLog& log;
    juce::PropertiesFile& settings;
    MeterBank meterBank;
    UiContext ctx;

    ConsoleLookAndFeel lookAndFeel;
    juce::TooltipWindow tooltips { this };

    state::StripRef selected;
    bool hasSelection = false;

    std::unique_ptr<ChannelStripSection> channelStrip;
    std::unique_ptr<MainDisplay> display;
    std::unique_ptr<MonitorSection> monitor;
    std::unique_ptr<FaderSection> sections[LayerSet::kNumSections];
    ResizerBar splitter { false };
    float topFraction = 0.47f;

    juce::OwnedArray<PluginWindow> pluginWindows;
    std::unique_ptr<juce::DocumentWindow> scannerWindow;
    std::unique_ptr<juce::FileChooser> fileChooser;
    juce::File showFile;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace livedaw::ui
