#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace livedaw
{

/** Owns the plugin formats (VST3, AU, LV2, optional VST2) and the list of scanned plugins. */
class PluginHost final : private juce::ChangeListener
{
public:
    /** `settings` is used to persist the scanned plugin list (may be nullptr). */
    explicit PluginHost (juce::PropertiesFile* settings);
    ~PluginHost() override;

    juce::AudioPluginFormatManager& getFormatManager() noexcept { return formatManager; }
    juce::KnownPluginList& getKnownPlugins() noexcept { return knownPlugins; }
    juce::PropertiesFile* getSettings() const noexcept { return settings; }

    /** File used by the scanner to blacklist plugins that crash during a scan. */
    juce::File getDeadMansPedalFile() const;

    /** If the previous run crashed while instantiating a plugin, blacklists that
        plugin and returns its name (empty otherwise). Call once at startup. */
    juce::String checkForCrashDuringPluginLoad();

    /** Synchronously instantiates a plugin (message thread). The plugin being
        loaded is recorded on disk first, so a crash inside its constructor can be
        traced and that plugin blacklisted on the next start. */
    std::unique_ptr<juce::AudioPluginInstance> createInstance (const juce::PluginDescription& description,
                                                               double sampleRate, int blockSize,
                                                               juce::String& errorMessage);

    /** Plugin types sorted for a popup menu. */
    juce::Array<juce::PluginDescription> getSortedTypes() const;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    juce::File getPendingLoadFile() const;

    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownPlugins;
    juce::PropertiesFile* settings;

    JUCE_DECLARE_NON_COPYABLE (PluginHost)
};

} // namespace livedaw
