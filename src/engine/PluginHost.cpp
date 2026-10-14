#include "engine/PluginHost.h"
#include "engine/PluginScanner.h"

namespace livedaw
{

static constexpr const char* kPluginListKey = "pluginList";

PluginHost::PluginHost (juce::PropertiesFile* s) : settings (s)
{
    juce::addDefaultFormatsToManager (formatManager);

    if (settings != nullptr)
        if (auto xml = settings->getXmlValue (kPluginListKey))
            knownPlugins.recreateFromXml (*xml);

    knownPlugins.addChangeListener (this);

    // A plugin that crashes during scanning only takes down the scanner process.
    knownPlugins.setCustomScanner (std::make_unique<OutOfProcessScanner>());
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

std::unique_ptr<juce::AudioPluginInstance> PluginHost::createInstance (const juce::PluginDescription& description,
                                                                       double sampleRate, int blockSize,
                                                                       juce::String& errorMessage)
{
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
