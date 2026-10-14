#pragma once

#include "ui/LayerBar.h"
#include "ui/LayersPanel.h"
#include "ui/LookAndFeel.h"
#include "ui/ShowPanel.h"
#include "ui/StripComponent.h"
#include "ui/StripDetailPanel.h"
#include "ui/SystemPanel.h"
#include "ui/UiContext.h"

#include <juce_audio_utils/juce_audio_utils.h>

namespace livedaw::ui
{

/** Top-level window content: toolbar with scene controls, the mixer page
    (inputs, buses, master, each with processing handles) and a tabbed panel
    (selected channel, show/scenes/snippets, system). */
class MainComponent final : public juce::Component, private juce::Timer
{
public:
    MainComponent (juce::AudioDeviceManager&, MixerEngine&, PluginHost&, Show&, EventLog&, juce::PropertiesFile&);
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    /** Recreates all strip components (after structural changes or loading a show). */
    void rebuildStrips();

    /** File the show was last opened from / saved to. */
    juce::File getShowFile() const { return showFile; }
    void setShowFile (const juce::File& f) { showFile = f; updateTitle(); }

    /** Reloads `file` (used by safe-mode recovery). */
    juce::Result reloadShow (const juce::File& file, bool loadPlugins);

private:
    class PluginWindow;
    class StripsContainer;

    void timerCallback() override;

    void showAudioSettings();
    void showPluginScanner();
    void openPluginEditor (Strip&, int slot);
    void closePluginWindowsFor (juce::AudioProcessor*);
    void closePluginWindowsFor (Strip&);
    void closeAllPluginWindows();

    void selectStrip (Strip*, bool reveal = true);
    void removeStrip (Strip&);
    void setChannelCount (int);
    void addBus (StripKind);
    void updateInsertRows();
    void setPanelVisible (bool);

    void showShowMenu();
    void newShow();
    void openShow();
    void saveShow (bool saveAs);
    void updateTitle();
    void updateSceneControls();
    void recall (state::ApplyResult (Show::*) ());

    juce::AudioDeviceManager& deviceManager;
    MixerEngine& engine;
    Show& show;
    EventLog& log;
    juce::PropertiesFile& settings;
    UiContext ctx;

    ConsoleLookAndFeel lookAndFeel;
    juce::TooltipWindow tooltips { this };

    juce::TextButton audioButton { "Audio Setup" }, pluginsButton { "Plugins" }, showButton { "Show" };
    juce::TextButton addAuxButton { "+ Aux" }, addGroupButton { "+ Group" };
    juce::TextButton fewerButton { "-" }, moreButton { "+" };
    juce::TextButton prevSceneButton { "<" }, goButton { "GO >" }, panelButton { "Panel" };
    juce::Label channelsCaption, channelCountLabel, sceneLabel, statusLabel;

    juce::Viewport stripViewport;
    std::unique_ptr<StripsContainer> stripsContainer;
    juce::OwnedArray<StripComponent> strips;
    std::unique_ptr<StripComponent> masterStrip;
    int insertRows = 3;

    LayerBar layerBar;
    StripDetailPanel detailPanel;
    ShowPanel showPanel;
    LayersPanel layersPanel;
    SystemPanel systemPanel;
    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };
    bool panelVisible = true;

    juce::OwnedArray<PluginWindow> pluginWindows;
    std::unique_ptr<juce::DocumentWindow> scannerWindow;
    juce::Component::SafePointer<juce::DialogWindow> audioSettingsWindow;
    std::unique_ptr<juce::FileChooser> fileChooser;
    juce::File showFile;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace livedaw::ui
