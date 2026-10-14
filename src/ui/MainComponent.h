#pragma once

#include "engine/MixerEngine.h"
#include "engine/PluginHost.h"
#include "ui/LookAndFeel.h"
#include "ui/StripComponent.h"
#include "ui/StripDetailPanel.h"

#include <juce_audio_utils/juce_audio_utils.h>

namespace livedaw::ui
{

/** Top-level window content: toolbar, the console (inputs, buses, master) and the
    selected-channel panel. */
class MainComponent final : public juce::Component, private juce::Timer
{
public:
    MainComponent (juce::AudioDeviceManager&, MixerEngine&, PluginHost&, juce::PropertiesFile&);
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Recreates all strip components (after structural changes or loading a session). */
    void rebuildStrips();

private:
    class PluginWindow;
    class StripsContainer;

    void timerCallback() override;

    void showAudioSettings();
    void showPluginScanner();
    void openPluginEditor (Strip&, int slot);
    void closePluginWindowsFor (juce::AudioProcessor*);
    void closePluginWindowsFor (Strip&);

    void selectStrip (Strip*);
    void removeStrip (Strip&);
    void setChannelCount (int);
    void addBus (StripKind);

    void newSession();
    void openSession();
    void saveSession();
    void updateTitle();

    juce::AudioDeviceManager& deviceManager;
    MixerEngine& engine;
    PluginHost& host;
    juce::PropertiesFile& settings;

    ConsoleLookAndFeel lookAndFeel;
    juce::TooltipWindow tooltips { this };

    juce::TextButton audioButton { "Audio Setup" }, pluginsButton { "Plugins" };
    juce::TextButton newButton { "New" }, openButton { "Open" }, saveButton { "Save" };
    juce::TextButton addAuxButton { "+ Aux" }, addGroupButton { "+ Group" };
    juce::TextButton fewerButton { "-" }, moreButton { "+" };
    juce::Label channelsCaption, channelCountLabel, statusLabel;

    juce::Viewport stripViewport;
    std::unique_ptr<StripsContainer> stripsContainer;
    juce::OwnedArray<StripComponent> strips;
    std::unique_ptr<StripComponent> masterStrip;
    StripDetailPanel detailPanel;

    juce::OwnedArray<PluginWindow> pluginWindows;
    std::unique_ptr<juce::DocumentWindow> scannerWindow;
    juce::Component::SafePointer<juce::DialogWindow> audioSettingsWindow;
    std::unique_ptr<juce::FileChooser> fileChooser;
    juce::File sessionFile;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace livedaw::ui
