#pragma once

#include "engine/Parameters.h"
#include "engine/RtGarbage.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <memory>

namespace livedaw
{

/** An ordered chain of insert plugins (VST3 / AU / LV2 / VST2 when enabled).

    Plugins are created and prepared on the message thread and then published to
    the audio thread with an atomic pointer swap. Replaced plugins go to the
    RtGarbage collector so they're destroyed on the message thread once the audio
    thread can no longer see them.

    Plugins always run in stereo; a mono strip feeds both plugin inputs and takes
    the left output back.

    Stability: a plugin whose output contains NaN/inf, or that throws, is bypassed
    on the spot (its input passes through unchanged) and flagged as faulted.
*/
class PluginChain
{
public:
    struct Slot
    {
        std::unique_ptr<juce::AudioPluginInstance> plugin;
        juce::AudioBuffer<float> scratch;
        juce::MidiBuffer midi;
        int numPluginChannels = 2;
    };

    explicit PluginChain (RtGarbage& garbage);
    ~PluginChain();

    /** Message thread. Prepares all loaded plugins for a new sample rate/block size.
        Must only be called while the audio device is stopped. */
    void prepare (double sampleRate, int maxBlockSize);

    /** Message thread. Installs `plugin` in `slotIndex` (nullptr clears the slot).
        The plugin is configured for stereo and prepared before being published. */
    void setPlugin (int slotIndex, std::unique_ptr<juce::AudioPluginInstance> plugin);

    /** Message thread only (the returned pointer must not be handed to other threads). */
    juce::AudioPluginInstance* getPlugin (int slotIndex) const noexcept;

    void setBypassed (int slotIndex, bool bypassed) noexcept;
    bool isBypassed (int slotIndex) const noexcept;

    bool isEmpty() const noexcept;

    /** True if the plugin in this slot was auto-bypassed because it produced NaN/inf
        or threw an exception. Cleared by setPlugin() or clearFault(). */
    bool isFaulted (int slotIndex) const noexcept;
    void clearFault (int slotIndex) noexcept;

    /** Smoothed processing time of the slot's plugin as a fraction of the block duration. */
    float getSlotLoad (int slotIndex) const noexcept;

    /** Sum of the reported latencies of the active plugins, in samples. */
    int getTotalLatencySamples() const noexcept;

    /** Audio thread. Processes `numChannels` (1 or 2) of `channels` in place. */
    void process (float* const* channels, int numChannels, int numSamples) noexcept;

private:
    RtGarbage& garbage;
    std::array<std::atomic<Slot*>, kNumInsertSlots> slots {};
    std::array<std::atomic<bool>, kNumInsertSlots> bypass {};
    std::array<std::atomic<bool>, kNumInsertSlots> faulted {};
    std::array<std::atomic<float>, kNumInsertSlots> slotLoad {};
    double sampleRate = 48000.0;
    int blockSize = 512;

    JUCE_DECLARE_NON_COPYABLE (PluginChain)
};

} // namespace livedaw
