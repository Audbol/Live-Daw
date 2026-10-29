#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace livedaw::sandbox
{

/** Test plugins for the sandbox tests: a stateful gain + one-pole filter that
    can be told (through its FAULT parameter, i.e. by automation) to crash its
    process or to hang in processBlock. The helper only offers them when the
    environment variable SIDECAR_SANDBOX_TEST_PLUGINS is 1; the tests also run them
    in-process to compare. */
class TestPluginFormat final : public juce::AudioPluginFormat
{
public:
    static constexpr const char* kFormatName = "SidecarTest";
    static constexpr const char* kEnvironmentVariable = "SIDECAR_SANDBOX_TEST_PLUGINS";
    static constexpr const char* kHangingScanId = "sidecar-test:hang-scan"; // findAllTypesForFile never returns

    enum class Kind
    {
        gain,  // FAULT does nothing
        crash, // FAULT > 0.5: segfaults in processBlock
        hang   // FAULT > 0.5: never returns from processBlock
    };

    static juce::PluginDescription describe (Kind);
    static std::unique_ptr<juce::AudioPluginInstance> create (Kind);
    static bool isEnabledByEnvironment();

    juce::String getName() const override { return kFormatName; }
    void findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>&, const juce::String&) override;
    bool fileMightContainThisPluginType (const juce::String& id) override { return id.startsWith ("sidecar-test:"); }
    juce::String getNameOfPluginFromIdentifier (const juce::String& id) override { return id; }
    bool pluginNeedsRescanning (const juce::PluginDescription&) override { return false; }
    bool doesPluginStillExist (const juce::PluginDescription& d) override { return fileMightContainThisPluginType (d.fileOrIdentifier); }
    bool canScanForPlugins() const override { return false; }
    bool isTrivialToScan() const override { return true; }
    juce::StringArray searchPathsForPlugins (const juce::FileSearchPath&, bool, bool) override { return {}; }
    juce::FileSearchPath getDefaultLocationsToSearch() override { return {}; }
    bool requiresUnblockedMessageThreadDuringCreation (const juce::PluginDescription&) const override { return false; }

private:
    void createPluginInstance (const juce::PluginDescription&, double, int, PluginCreationCallback) override;
};

} // namespace livedaw::sandbox
