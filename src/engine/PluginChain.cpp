#include "engine/PluginChain.h"

namespace livedaw
{

namespace
{
    void configureForStereo (juce::AudioPluginInstance& plugin)
    {
        // Prefer a plain stereo-in/stereo-out layout; keep the plugin's own layout
        // (e.g. sidechain buses) if it refuses.
        auto layout = plugin.getBusesLayout();

        if (layout.inputBuses.size() > 0)
            layout.inputBuses.getReference (0) = juce::AudioChannelSet::stereo();
        if (layout.outputBuses.size() > 0)
            layout.outputBuses.getReference (0) = juce::AudioChannelSet::stereo();

        if (plugin.checkBusesLayoutSupported (layout))
            plugin.setBusesLayout (layout);
    }

    void prepareSlot (PluginChain::Slot& slot, double sampleRate, int blockSize)
    {
        auto& p = *slot.plugin;
        p.setRateAndBufferSizeDetails (sampleRate, blockSize);
        p.prepareToPlay (sampleRate, blockSize);
        slot.numPluginChannels = juce::jmax (2, p.getTotalNumInputChannels(), p.getTotalNumOutputChannels());
        slot.scratch.setSize (slot.numPluginChannels, blockSize, false, true, false);
        slot.midi.ensureSize (256);
    }
} // namespace

PluginChain::PluginChain (RtGarbage& g) : garbage (g)
{
    for (auto& s : slots)
        s.store (nullptr);
    for (auto& b : bypass)
        b.store (false);
}

PluginChain::~PluginChain()
{
    // The owner guarantees the audio thread no longer references this chain.
    for (auto& s : slots)
        delete s.exchange (nullptr);
}

void PluginChain::prepare (double newSampleRate, int maxBlockSize)
{
    sampleRate = newSampleRate;
    blockSize = maxBlockSize;

    for (auto& s : slots)
        if (auto* slot = s.load())
        {
            slot->plugin->releaseResources();
            prepareSlot (*slot, sampleRate, blockSize);
        }
}

void PluginChain::setPlugin (int slotIndex, std::unique_ptr<juce::AudioPluginInstance> plugin)
{
    jassert (juce::isPositiveAndBelow (slotIndex, kNumInsertSlots));

    std::unique_ptr<Slot> newSlot;

    if (plugin != nullptr)
    {
        newSlot = std::make_unique<Slot>();
        newSlot->plugin = std::move (plugin);
        configureForStereo (*newSlot->plugin);
        prepareSlot (*newSlot, sampleRate, blockSize);
    }

    std::unique_ptr<Slot> old (slots[(size_t) slotIndex].exchange (newSlot.release(), std::memory_order_acq_rel));
    bypass[(size_t) slotIndex].store (false);
    garbage.retire (std::move (old));
}

juce::AudioPluginInstance* PluginChain::getPlugin (int slotIndex) const noexcept
{
    if (auto* slot = slots[(size_t) slotIndex].load())
        return slot->plugin.get();

    return nullptr;
}

void PluginChain::setBypassed (int slotIndex, bool bypassed) noexcept
{
    bypass[(size_t) slotIndex].store (bypassed);
}

bool PluginChain::isBypassed (int slotIndex) const noexcept
{
    return bypass[(size_t) slotIndex].load();
}

bool PluginChain::isEmpty() const noexcept
{
    for (auto& s : slots)
        if (s.load (std::memory_order_relaxed) != nullptr)
            return false;

    return true;
}

int PluginChain::getTotalLatencySamples() const noexcept
{
    int total = 0;

    for (size_t i = 0; i < slots.size(); ++i)
        if (auto* slot = slots[i].load())
            if (! bypass[i].load())
                total += slot->plugin->getLatencySamples();

    return total;
}

void PluginChain::process (float* const* channels, int numChannels, int numSamples) noexcept
{
    for (size_t i = 0; i < slots.size(); ++i)
    {
        auto* slot = slots[i].load (std::memory_order_acquire);

        if (slot == nullptr || bypass[i].load (std::memory_order_relaxed))
            continue;

        auto& scratch = slot->scratch;

        // Hosts must never pass more samples than were prepared; chunk if needed.
        for (int start = 0; start < numSamples; start += blockSize)
        {
            const int n = juce::jmin (blockSize, numSamples - start);

            scratch.copyFrom (0, 0, channels[0] + start, n);
            scratch.copyFrom (1, 0, channels[numChannels > 1 ? 1 : 0] + start, n);
            for (int ch = 2; ch < slot->numPluginChannels; ++ch)
                scratch.clear (ch, 0, n);

            juce::AudioBuffer<float> view (scratch.getArrayOfWritePointers(), slot->numPluginChannels, n);
            slot->midi.clear();
            slot->plugin->processBlock (view, slot->midi);

            for (int ch = 0; ch < numChannels; ++ch)
                juce::FloatVectorOperations::copy (channels[ch] + start, scratch.getReadPointer (ch), n);
        }
    }
}

} // namespace livedaw
