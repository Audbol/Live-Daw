#include "engine/EventLog.h"
#include "engine/MixerEngine.h"
#include "engine/PluginHost.h"
#include "engine/PluginScanner.h"
#include "engine/Show.h"
#include "engine/VirtualAudioDevice.h"
#include "ui/MainComponent.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <csignal>

namespace livedaw
{

/** Enough to open every channel of large ASIO/CoreAudio/ALSA/JACK interfaces. */
constexpr int kMaxDeviceChannels = 256;
/** A fresh console is a WING: 40 channels. */
constexpr int kDefaultChannelCount = kWingChannels;

//==============================================================================
/** Keeps audio running: reopens the device if it disappears, stops calling back
    or reports an error. Retries with a back-off so a missing interface doesn't
    hammer the driver. */
class DeviceWatchdog final : private juce::Timer, private juce::ChangeListener
{
public:
    DeviceWatchdog (juce::AudioDeviceManager& dm, MixerEngine& e, EventLog& l)
        : deviceManager (dm), engine (e), log (l)
    {
        deviceManager.addChangeListener (this);
        engine.onDeviceError = [this] (const juce::String& message)
        {
            log.error ("Audio device error: " + message);
            restartPending = true;
        };
        changeListenerCallback (nullptr);
        startTimer (1000);
    }

    ~DeviceWatchdog() override
    {
        engine.onDeviceError = nullptr;
        deviceManager.removeChangeListener (this);
    }

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        if (auto* device = deviceManager.getCurrentAudioDevice())
        {
            lastGoodSetup = deviceManager.getAudioDeviceSetup();
            haveGoodSetup = true;

            const auto description = device->getTypeName() + " / " + device->getName() + " @ "
                                     + juce::String (device->getCurrentSampleRate(), 0) + " Hz, "
                                     + juce::String (device->getCurrentBufferSizeSamples()) + " samples";
            if (description != lastDescription)
                log.info ("Audio device: " + description);
            lastDescription = description;
        }
        else if (lastDescription.isNotEmpty())
        {
            log.error ("Audio device closed or lost: " + lastDescription);
            lastDescription = {};
        }
    }

    void timerCallback() override
    {
        const auto now = juce::Time::getMillisecondCounter();
        auto* device = deviceManager.getCurrentAudioDevice();

        const bool stalled = device != nullptr && device->isPlaying() && engine.isDeviceRunning()
                             && now - engine.getLastCallbackTime() > 2000;

        if (stalled && ! restartPending)
        {
            log.error ("Audio callbacks stalled for more than 2 s");
            restartPending = true;
        }

        const bool lost = device == nullptr && haveGoodSetup;

        if ((! restartPending && ! lost) || now < nextAttemptMs)
            return;

        ++attempts;
        log.warning ("Restarting audio device (attempt " + juce::String (attempts) + ")");

        juce::String error;
        if (device != nullptr)
        {
            deviceManager.closeAudioDevice();
            deviceManager.restartLastAudioDevice();
        }
        else
        {
            error = deviceManager.setAudioDeviceSetup (lastGoodSetup, true);
        }

        if (deviceManager.getCurrentAudioDevice() != nullptr && error.isEmpty())
        {
            log.info ("Audio device restarted");
            restartPending = false;
            attempts = 0;
            nextAttemptMs = 0;
        }
        else
        {
            // Back off: 1 s, 2 s, 4 s ... capped at 10 s.
            nextAttemptMs = now + static_cast<juce::uint32> (juce::jmin (10000, 1000 << juce::jmin (attempts, 4)));
        }
    }

    juce::AudioDeviceManager& deviceManager;
    MixerEngine& engine;
    EventLog& log;
    juce::AudioDeviceManager::AudioDeviceSetup lastGoodSetup;
    bool haveGoodSetup = false, restartPending = false;
    int attempts = 0;
    juce::uint32 nextAttemptMs = 0;
    juce::String lastDescription;
};

//==============================================================================
/** SIGTERM/SIGINT (OS shutdown, service managers, Ctrl+C) quit cleanly, so they
    save state and aren't mistaken for a crash on the next start. The handler only
    sets a flag; a timer on the message thread does the actual quit. */
class SignalQuitter final : private juce::Timer
{
public:
    SignalQuitter()
    {
        std::signal (SIGTERM, handler);
        std::signal (SIGINT, handler);
        startTimer (200);
    }

private:
    static void handler (int) { requested = 1; }

    void timerCallback() override
    {
        if (requested != 0)
        {
            stopTimer();
            juce::JUCEApplicationBase::quit();
        }
    }

    static inline volatile std::sig_atomic_t requested = 0;
};

//==============================================================================
class LiveDawApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }

    // The plugin scanner runs as a second instance of this executable.
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const juce::String& commandLine) override
    {
        if (auto worker = PluginScannerWorker::tryToStart (commandLine))
        {
            scannerWorker = std::move (worker);
            return;
        }

        juce::PropertiesFile::Options options;
        options.applicationName = "LiveDAW";
        options.filenameSuffix = "settings";
        options.folderName = "LiveDAW";
        options.osxLibrarySubFolder = "Application Support";
        properties.setStorageParameters (options);
        auto& settings = *properties.getUserSettings();

        if (const char* mismatch = ld::verifyLayout())
        {
            // A mismatched engine library would corrupt the shared parameter blocks.
            const auto message = juce::String ("The audio engine library does not match this build (")
                                 + mismatch + "). Please reinstall Live DAW.";
            juce::NativeMessageBox::showAsync (juce::MessageBoxOptions::makeOptionsOk (juce::MessageBoxIconType::WarningIcon,
                                                                                       "Live DAW", message),
                                               [] (int) { quit(); });
            return;
        }

        getDataDirectory().createDirectory();

        // Did the previous run end without a clean shutdown?
        const bool crashedLastTime = getRunningMarker().existsAsFile();
        getRunningMarker().replaceWithText (juce::String (juce::Time::getCurrentTime().toMilliseconds()));

        pluginHost = std::make_unique<PluginHost> (&settings);
        engine = std::make_unique<MixerEngine>();
        show = std::make_unique<Show> (*engine, *pluginHost);

        log.info ("Live DAW " + getApplicationVersion() + " starting, "
                  + juce::String (engine->getNumWorkerThreads() + 1) + " DSP threads");

        const auto crashedPlugin = pluginHost->checkForCrashDuringPluginLoad();
        if (crashedPlugin.isNotEmpty())
            log.error ("\"" + crashedPlugin + "\" crashed while loading last time and has been blacklisted");

        // Restore the console from the last run. After a crash, restore it without
        // plugins (safe mode) so a bad plugin can't crash the app again.
        safeMode = crashedLastTime;
        if (getAutosaveFile().existsAsFile())
        {
            const auto result = show->loadFromFile (getAutosaveFile(), ! safeMode);
            if (result.failed())
                log.warning (result.getErrorMessage());
        }

        if (engine->getChannels().empty())
            engine->setNumChannels (kDefaultChannelCount);

        // STARTUP MAIN MUTE: the mains start muted (before any audio runs) when
        // the show asks for it.
        if (engine->applyStartupMainMute())
            log.info ("Mains muted at start-up (STARTUP MAIN MUTE)");

        // A device with no hardware behind it, for machines without a sound card.
        // Added after the platform's types (created by getAvailableDeviceTypes),
        // so it is never picked by default over real hardware.
        deviceManager.getAvailableDeviceTypes();
        deviceManager.addAudioDeviceType (std::make_unique<VirtualAudioIODeviceType>());

        // First launch: open a low-latency driver (ASIO on Windows, JACK on Linux)
        // directly, instead of trying the OS default device first.
        auto savedDevice = settings.getXmlValue ("audioDevice");
        if (savedDevice == nullptr)
            savedDevice = makePreferredDeviceState();

        const auto error = deviceManager.initialise (kMaxDeviceChannels, kMaxDeviceChannels, savedDevice.get(), true);
        if (error.isNotEmpty())
            log.error ("Audio device: " + error);

        deviceManager.addAudioCallback (engine.get());
        watchdog = std::make_unique<DeviceWatchdog> (deviceManager, *engine, log);

        auto* main = new ui::MainComponent (deviceManager, *engine, *pluginHost, *show, log, settings);
        mainComponent = main;
        mainWindow = std::make_unique<MainWindow> (getApplicationName(), main, settings.getValue ("ui.windowState"));
        main->setShowFile (juce::File (settings.getValue ("lastShowFile")).existsAsFile()
                               ? juce::File (settings.getValue ("lastShowFile")) : juce::File());

        if (safeMode)
            offerSafeModeRecovery (crashedPlugin);

        startAutosave();
        signalQuitter = std::make_unique<SignalQuitter>();
    }

    void shutdown() override
    {
        if (scannerWorker != nullptr)
        {
            scannerWorker.reset();
            return;
        }

        if (engine == nullptr) // refused to start
            return;

        signalQuitter.reset();
        autosaveTimer.reset();
        saveState();

        watchdog.reset();
        mainWindow.reset();
        deviceManager.removeAudioCallback (engine.get());
        deviceManager.closeAudioDevice();
        show.reset();
        engine.reset();
        pluginHost.reset();

        // Clean exit: next start is not a crash recovery.
        getRunningMarker().deleteFile();
    }

    void systemRequestedQuit() override { quit(); }
    void anotherInstanceStarted (const juce::String&) override {}

private:
    class MainWindow final : public juce::DocumentWindow
    {
    public:
        MainWindow (const juce::String& name, juce::Component* content, const juce::String& savedState)
            : DocumentWindow (name, ui::Colours::background, DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (content, true);
            setResizable (true, true);
            // Below about 1152x680 the main display and the fader sections
            // no longer fit side by side and on top of each other.
            setResizeLimits (1152, 680, 32768, 32768);

            // Reopen where it was (size, position, maximised); fall back to centred.
            if (savedState.isEmpty() || ! restoreWindowStateFromString (savedState))
                centreWithSize (getWidth(), getHeight());

            setVisible (true);
        }

        void closeButtonPressed() override
        {
            juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Quit Live DAW",
                                                "Quit and stop all audio?", "Quit", "Cancel", this,
                                                juce::ModalCallbackFunction::create ([] (int result)
            {
                if (result != 0)
                    JUCEApplication::getInstance()->systemRequestedQuit();
            }));
        }
    };

    static juce::File getDataDirectory()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("LiveDAW");
    }

    static juce::File getRunningMarker() { return getDataDirectory().getChildFile ("running.lock"); }

    /** The autosave restored at startup (always the full one, with plugins). */
    static juce::File getAutosaveFile() { return getDataDirectory().getChildFile ("autosave.lshow"); }

    /** Where autosaves go: in safe mode, elsewhere, so the plugin-less console
        never overwrites the real autosave before the user decides. */
    juce::File getAutosaveTarget() const
    {
        return safeMode ? getDataDirectory().getChildFile ("autosave-safemode.lshow") : getAutosaveFile();
    }

    /** Device state selecting the first ASIO (or JACK) device, or nullptr to use
        the OS default. Uses the same format as AudioDeviceManager::createStateXml(). */
    std::unique_ptr<juce::XmlElement> makePreferredDeviceState()
    {
        for (const auto* typeName : { "ASIO", "JACK" })
        {
            for (auto* type : deviceManager.getAvailableDeviceTypes())
            {
                if (type->getTypeName() != typeName)
                    continue;

                type->scanForDevices();
                const auto devices = type->getDeviceNames (false);
                if (devices.isEmpty())
                    continue;

                log.info (juce::String (typeName) + " driver found: " + devices[0]);
                auto xml = std::make_unique<juce::XmlElement> ("DEVICESETUP");
                xml->setAttribute ("deviceType", typeName);
                xml->setAttribute ("audioOutputDeviceName", devices[0]);
                xml->setAttribute ("audioInputDeviceName",
                                   type->hasSeparateInputsAndOutputs() ? type->getDeviceNames (true)[0] : devices[0]);
                return xml;
            }
        }

        return nullptr;
    }

    void offerSafeModeRecovery (const juce::String& crashedPlugin)
    {
        juce::String message = "Live DAW did not shut down cleanly last time.\n\n"
                               "The console has been restored in SAFE MODE, without any plugins.";
        if (crashedPlugin.isNotEmpty())
            message << "\n\n\"" << crashedPlugin << "\" crashed while loading and has been blacklisted.";

        log.warning ("Started in safe mode (previous run did not exit cleanly)");

        juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::WarningIcon, "Safe mode", message,
                                            "Reload with plugins", "Stay in safe mode", nullptr,
                                            juce::ModalCallbackFunction::create ([this] (int result)
        {
            if (result == 0 || mainComponent == nullptr)
                return;

            safeMode = false;
            const auto loadResult = mainComponent->reloadShow (getAutosaveFile(), true);
            log.info ("Left safe mode: plugins reloaded");
            if (loadResult.failed())
                log.warning (loadResult.getErrorMessage());
        }));
    }

    void saveState()
    {
        if (engine == nullptr)
            return;

        auto& settings = *properties.getUserSettings();
        if (auto xml = deviceManager.createStateXml())
            settings.setValue ("audioDevice", xml.get());
        if (mainComponent != nullptr)
            settings.setValue ("lastShowFile", mainComponent->getShowFile().getFullPathName());
        if (mainWindow != nullptr)
            settings.setValue ("ui.windowState", mainWindow->getWindowStateAsString());
        settings.saveIfNeeded();

        const auto result = show->saveToFile (getAutosaveTarget());
        if (result.failed())
            log.error ("Autosave failed: " + result.getErrorMessage());
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

    std::unique_ptr<PluginScannerWorker> scannerWorker;

    juce::ApplicationProperties properties;
    juce::AudioDeviceManager deviceManager;
    EventLog log;
    std::unique_ptr<PluginHost> pluginHost;
    std::unique_ptr<MixerEngine> engine;
    std::unique_ptr<Show> show;
    std::unique_ptr<DeviceWatchdog> watchdog;
    std::unique_ptr<MainWindow> mainWindow;
    juce::Component::SafePointer<ui::MainComponent> mainComponent;
    std::unique_ptr<juce::Timer> autosaveTimer;
    std::unique_ptr<SignalQuitter> signalQuitter;
    bool safeMode = false;
};

} // namespace livedaw

START_JUCE_APPLICATION (livedaw::LiveDawApplication)
