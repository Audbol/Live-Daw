#pragma once

#include "engine/MixerEngine.h"
#include "engine/PluginHost.h"

#include <juce_core/juce_core.h>

namespace livedaw::session
{

/** Serialises the whole console (strips, routing, processing, plugins + their state). */
std::unique_ptr<juce::XmlElement> save (const MixerEngine& engine);

/** Rebuilds the console from XML. Plugins that fail to load are reported in the
    returned Result but don't abort the load. */
juce::Result load (MixerEngine& engine, const juce::XmlElement& xml, PluginHost& host);

juce::Result saveToFile (const MixerEngine& engine, const juce::File& file);
juce::Result loadFromFile (MixerEngine& engine, const juce::File& file, PluginHost& host);

} // namespace livedaw::session
