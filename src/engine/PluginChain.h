#pragma once

#include "engine/Parameters.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <memory>

namespace livedaw
{

/** The insert slots of one strip. The plugin instances live here (JUCE); the Rust
    engine calls them from the audio thread through a callback per slot, checks
    their output and auto-bypasses a plugin that throws or emits NaN/inf.

    Replaced plugins are destroyed on the message thread once the engine can no
    longer call them (the engine invokes the release callback from
    ld_engine_collect). Plugins run in stereo: a mono strip feeds both inputs and
    takes the left output back. */
class PluginChain
{
public:
    PluginChain (ld::Engine* engine, ld::Strip* strip);

    /** Message thread, device stopped: re-prepares the loaded plugins. */
    void prepare (double sampleRate, int maxBlockSize);

    /** Message thread: installs `plugin` in `slotIndex` (nullptr clears it). */
    void setPlugin (int slotIndex, std::unique_ptr<juce::AudioPluginInstance> plugin);

    /** Message thread only. */
    juce::AudioPluginInstance* getPlugin (int slotIndex) const noexcept;

    void setBypassed (int slotIndex, bool bypassed) noexcept;
    bool isBypassed (int slotIndex) const noexcept;
    bool isFaulted (int slotIndex) const noexcept;
    void clearFault (int slotIndex) noexcept;
    float getSlotLoad (int slotIndex) const noexcept;
    bool isEmpty() const noexcept;
    int getTotalLatencySamples() const noexcept;

private:
    struct Holder;

    ld::Engine* engine;
    ld::Strip* strip;
    std::array<Holder*, kNumInsertSlots> holders {}; // owned by the engine until released

    JUCE_DECLARE_NON_COPYABLE (PluginChain)
};

} // namespace livedaw
