#include "engine/MixerEngine.h"
#include "engine/PluginHost.h"
#include "engine/Session.h"
#include "ui/MainComponent.h"

#include <juce_audio_utils/juce_audio_utils.h>

namespace livedaw
{

/** Enough to open every channel of large ASIO/CoreAudio/ALSA/JACK interfaces. */
constexpr int kMaxDeviceChannels = 256;
constexpr int kDefaultChannelCount = 16;

class LiveDawApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise (const juce::String&) override
    {
        juce::PropertiesFile::Options options;
        options.applicationName = "LiveDAW";
        options.filenameSuffix = "settings";
        options.folderName = "LiveDAW";
        options.osxLibrarySubFolder = "Application Support";
        properties.setStorageParameters (options);
        auto& settings = *properties.getUserSettings();

        pluginHost = std::make_unique<PluginHost> (&settings);
        engine = std::make_unique<MixerEngine>();

        // Restore the console from the last run (crash-safe autosave), or start fresh.
        // A plugin that fails to load doesn't stop the rest of the console restoring.
        if (getAutosaveFile().existsAsFile())
            session::loadFromFile (*engine, getAutosaveFile(), *pluginHost);

        if (engine->getInputs().empty() && engine->getBuses().empty())
            engine->setNumInputs (kDefaultChannelCount);

        auto savedDevice = settings.getXmlValue ("audioDevice");
        const auto error = deviceManager.initialise (kMaxDeviceChannels, kMaxDeviceChannels,
                                                     savedDevice.get(), true);
        if (error.isNotEmpty())
            juce::Logger::writeToLog ("Audio device error: " + error);

        deviceManager.addAudioCallback (engine.get());

        mainWindow = std::make_unique<MainWindow> (getApplicationName(),
                                                   new ui::MainComponent (deviceManager, *engine, *pluginHost, settings));
        startAutosave();
    }

    void shutdown() override
    {
        autosaveTimer.reset();
        saveState();

        mainWindow.reset();
        deviceManager.removeAudioCallback (engine.get());
        deviceManager.closeAudioDevice();
        engine.reset();
        pluginHost.reset();
    }

    void systemRequestedQuit() override { quit(); }

    void anotherInstanceStarted (const juce::String&) override {}

private:
    class MainWindow final : public juce::DocumentWindow
    {
    public:
        MainWindow (const juce::String& name, juce::Component* content)
            : DocumentWindow (name, ui::Colours::background, DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (content, true);
            setResizable (true, true);
            setResizeLimits (900, 600, 10000, 10000);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override
        {
            JUCEApplication::getInstance()->systemRequestedQuit();
        }
    };

    static juce::File getAutosaveFile()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
            .getChildFile ("LiveDAW")
            .getChildFile ("autosave.livedaw");
    }

    void saveState()
    {
        if (engine == nullptr)
            return;

        auto& settings = *properties.getUserSettings();
        if (auto xml = deviceManager.createStateXml())
            settings.setValue ("audioDevice", xml.get());
        settings.saveIfNeeded();

        getAutosaveFile().getParentDirectory().createDirectory();
        session::saveToFile (*engine, getAutosaveFile());
    }

    void startAutosave()
    {
        struct AutosaveTimer final : juce::Timer
        {
            explicit AutosaveTimer (LiveDawApplication& a) : app (a) { startTimer (30000); }
            void timerCallback() override { app.saveState(); }
            LiveDawApplication& app;
        };

        autosaveTimer = std::make_unique<AutosaveTimer> (*this);
    }

    juce::ApplicationProperties properties;
    juce::AudioDeviceManager deviceManager;
    std::unique_ptr<PluginHost> pluginHost;
    std::unique_ptr<MixerEngine> engine;
    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<juce::Timer> autosaveTimer;
};

} // namespace livedaw

START_JUCE_APPLICATION (livedaw::LiveDawApplication)
