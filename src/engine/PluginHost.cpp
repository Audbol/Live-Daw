#include "engine/PluginHost.h"
#include "engine/PluginScanner.h"

namespace livedaw
{

static constexpr const char* kPluginListKey = "pluginList";
static constexpr const char* kSandboxKey = "plugins.sandbox";

PluginHost::PluginHost (juce::PropertiesFile* s) : settings (s)
{
    juce::addDefaultFormatsToManager (formatManager);

    if (settings != nullptr)
        if (auto xml = settings->getXmlValue (kPluginListKey))
            knownPlugins.recreateFromXml (*xml);

    knownPlugins.addChangeListener (this);

    // A plugin that crashes during scanning only takes down the scanner process.
    knownPlugins.setCustomScanner (std::make_unique<OutOfProcessScanner>());

    SandboxOptions options;
    options.helper = SandboxedPlugin::findHelper();
    sandboxOptions = std::make_shared<const SandboxOptions> (std::move (options));
    sandboxEnabled = settings != nullptr && settings->getBoolValue (kSandboxKey, false);
}

void PluginHost::setSandboxEnabled (bool on)
{
    sandboxEnabled = on;
    if (settings != nullptr)
    {
        settings->setValue (kSandboxKey, on);
        settings->saveIfNeeded();
    }
}

void PluginHost::setSandboxOptions (const SandboxOptions& options)
{
    sandboxOptions = std::make_shared<const SandboxOptions> (options);
}

PluginHost::~PluginHost()
{
    knownPlugins.removeChangeListener (this);
}

juce::File PluginHost::getDeadMansPedalFile() const
{
    if (settings != nullptr)
        return settings->getFile().getSiblingFile ("RecentlyCrashedPluginsList");

    return juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("LiveDAWCrashedPlugins");
}

void PluginHost::changeListenerCallback (juce::ChangeBroadcaster*)
{
    if (settings == nullptr)
        return;

    if (auto xml = knownPlugins.createXml())
    {
        settings->setValue (kPluginListKey, xml.get());
        settings->saveIfNeeded();
    }
}

juce::File PluginHost::getPendingLoadFile() const
{
    return getDeadMansPedalFile().getSiblingFile ("PluginBeingLoaded");
}

juce::String PluginHost::checkForCrashDuringPluginLoad()
{
    const auto file = getPendingLoadFile();

    if (! file.existsAsFile())
        return {};

    const auto lines = juce::StringArray::fromLines (file.loadFileAsString());
    file.deleteFile();

    const auto identifier = lines[0].trim();
    if (identifier.isEmpty())
        return {};

    knownPlugins.addToBlacklist (identifier);
    return lines[1].isNotEmpty() ? lines[1] : identifier;
}

juce::PluginDescription PluginHost::resolveDescription (const juce::PluginDescription& description)
{
    // On Linux a VST3 instance describes itself with the library inside its
    // bundle (X.vst3/Contents/x86_64-linux/X.so), which no format accepts, and
    // shows saved that: load the bundle instead.
    if (description.pluginFormatName != "VST3" || ! juce::File::isAbsolutePath (description.fileOrIdentifier))
        return description;
    const juce::File file (description.fileOrIdentifier);
    if (file.hasFileExtension (".vst3"))
        return description;
    for (auto dir = file.getParentDirectory(); dir != dir.getParentDirectory(); dir = dir.getParentDirectory())
    {
        if (dir.hasFileExtension (".vst3"))
        {
            auto resolved = description;
            resolved.fileOrIdentifier = dir.getFullPathName();
            return resolved;
        }
    }
    return description;
}

std::unique_ptr<juce::AudioPluginInstance> PluginHost::createInstance (const juce::PluginDescription& requested,
                                                                       double sampleRate, int blockSize,
                                                                       juce::String& errorMessage)
{
    const auto description = resolveDescription (requested);

    if (sandboxEnabled)
    {
        // A crash while loading only takes down the helper: no marker needed.
        auto sandboxed = SandboxedPlugin::create (description, sampleRate, blockSize, sandboxOptions, errorMessage);
        if (sandboxed == nullptr && errorMessage.isNotEmpty())
            errorMessage = "in the plugin sandbox: " + errorMessage;
        return sandboxed;
    }

    const auto pending = getPendingLoadFile();
    pending.getParentDirectory().createDirectory();
    pending.replaceWithText (description.fileOrIdentifier + "\n" + description.name);

    auto instance = formatManager.createPluginInstance (description, sampleRate, blockSize, errorMessage);

    pending.deleteFile();
    return instance;
}

juce::Array<juce::PluginDescription> PluginHost::getSortedTypes() const
{
    auto types = knownPlugins.getTypes();
    std::sort (types.begin(), types.end(), [] (const auto& a, const auto& b)
    {
        if (a.manufacturerName != b.manufacturerName)
            return a.manufacturerName.compareIgnoreCase (b.manufacturerName) < 0;
        return a.name.compareIgnoreCase (b.name) < 0;
    });
    return types;
}

} // namespace livedaw
