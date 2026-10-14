#include "engine/PluginHost.h"

namespace livedaw
{

static constexpr const char* kPluginListKey = "pluginList";

PluginHost::PluginHost (juce::PropertiesFile* s) : settings (s)
{
    formatManager.addDefaultFormats();

    if (settings != nullptr)
        if (auto xml = settings->getXmlValue (kPluginListKey))
            knownPlugins.recreateFromXml (*xml);

    knownPlugins.addChangeListener (this);
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

std::unique_ptr<juce::AudioPluginInstance> PluginHost::createInstance (const juce::PluginDescription& description,
                                                                       double sampleRate, int blockSize,
                                                                       juce::String& errorMessage)
{
    return formatManager.createPluginInstance (description, sampleRate, blockSize, errorMessage);
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
