#include "engine/PluginChain.h"

namespace livedaw
{

struct PluginChain::Holder
{
    std::unique_ptr<juce::AudioPluginInstance> plugin;
    juce::AudioBuffer<float> scratch;
    juce::MidiBuffer midi;
    int numPluginChannels = 2;
    int blockSize = 512;

    void prepare (double sampleRate, int maxBlock)
    {
        blockSize = juce::jmax (1, maxBlock);
        plugin->setRateAndBufferSizeDetails (sampleRate, blockSize);
        plugin->prepareToPlay (sampleRate, blockSize);
        numPluginChannels = juce::jmax (2, plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels());
        scratch.setSize (numPluginChannels, blockSize, false, true, false);
        midi.ensureSize (256);
    }

    /** Audio thread (called by the engine). Returns non-zero if the plugin threw. */
    static int32_t process (void* ctx, float* const* channels, int32_t numChannels, int32_t numSamples)
    {
        auto& h = *static_cast<Holder*> (ctx);

        try
        {
            for (int start = 0; start < numSamples; start += h.blockSize)
            {
                const int n = juce::jmin (h.blockSize, numSamples - start);

                h.scratch.copyFrom (0, 0, channels[0] + start, n);
                h.scratch.copyFrom (1, 0, channels[numChannels > 1 ? 1 : 0] + start, n);
                for (int ch = 2; ch < h.numPluginChannels; ++ch)
                    h.scratch.clear (ch, 0, n);

                juce::AudioBuffer<float> view (h.scratch.getArrayOfWritePointers(), h.numPluginChannels, n);
                h.midi.clear();
                h.plugin->processBlock (view, h.midi);

                for (int ch = 0; ch < numChannels; ++ch)
                    juce::FloatVectorOperations::copy (channels[ch] + start, h.scratch.getReadPointer (ch), n);
            }
        }
        catch (...)
        {
            return 1; // the engine restores the dry signal and bypasses the slot
        }

        return 0;
    }

    /** Message thread (from ld_engine_collect / engine destruction). */
    static void release (void* ctx)
    {
        delete static_cast<Holder*> (ctx);
    }
};

namespace
{
    void configureForStereo (juce::AudioPluginInstance& plugin)
    {
        auto layout = plugin.getBusesLayout();
        if (layout.inputBuses.size() > 0)
            layout.inputBuses.getReference (0) = juce::AudioChannelSet::stereo();
        if (layout.outputBuses.size() > 0)
            layout.outputBuses.getReference (0) = juce::AudioChannelSet::stereo();
        if (plugin.checkBusesLayoutSupported (layout))
            plugin.setBusesLayout (layout);
    }
} // namespace

PluginChain::PluginChain (ld::Engine* e, ld::Strip* s) : engine (e), strip (s) {}

PluginChain::Holder* PluginChain::holderAt (int slot) const noexcept
{
    // Every insert callback context registered with the engine is a Holder (see setPlugin).
    return juce::isPositiveAndBelow (slot, kNumInsertSlots)
               ? static_cast<Holder*> (ld::ld_strip_insert_ctx (strip, slot))
               : nullptr;
}

void PluginChain::prepare (double sampleRate, int maxBlockSize)
{
    for (int slot = 0; slot < kNumInsertSlots; ++slot)
    {
        if (auto* h = holderAt (slot))
        {
            h->plugin->releaseResources();
            h->prepare (sampleRate, maxBlockSize);
        }
    }
}

void PluginChain::setPlugin (int slot, std::unique_ptr<juce::AudioPluginInstance> plugin)
{
    if (! juce::isPositiveAndBelow (slot, kNumInsertSlots))
        return;

    Holder* holder = nullptr;

    if (plugin != nullptr)
    {
        holder = new Holder();
        holder->plugin = std::move (plugin);
        configureForStereo (*holder->plugin);
        holder->prepare (ld::ld_engine_sample_rate (engine), ld::ld_engine_block_size (engine));
    }

    if (holder != nullptr)
    {
        const ld::InsertCallbacks callbacks { &Holder::process, &Holder::release, holder };
        ld::ld_strip_set_insert (engine, strip, slot, &callbacks);
    }
    else
    {
        ld::ld_strip_set_insert (engine, strip, slot, nullptr);
    }
}

juce::AudioPluginInstance* PluginChain::getPlugin (int slot) const noexcept
{
    auto* h = holderAt (slot);
    return h != nullptr ? h->plugin.get() : nullptr;
}

void PluginChain::setBypassed (int slot, bool bypassed) noexcept { ld::ld_strip_insert_set_bypassed (strip, slot, bypassed); }
bool PluginChain::isBypassed (int slot) const noexcept         { return ld::ld_strip_insert_is_bypassed (strip, slot); }
bool PluginChain::isFaulted (int slot) const noexcept          { return ld::ld_strip_insert_is_faulted (strip, slot); }
void PluginChain::clearFault (int slot) noexcept                { ld::ld_strip_insert_clear_fault (strip, slot); }
float PluginChain::getSlotLoad (int slot) const noexcept       { return ld::ld_strip_insert_load (strip, slot); }

bool PluginChain::isEmpty() const noexcept
{
    for (int slot = 0; slot < kNumInsertSlots; ++slot)
        if (holderAt (slot) != nullptr)
            return false;
    return true;
}

int PluginChain::getTotalLatencySamples() const noexcept
{
    int total = 0;
    for (int i = 0; i < kNumInsertSlots; ++i)
        if (auto* p = getPlugin (i); p != nullptr && ! isBypassed (i))
            total += p->getLatencySamples();
    return total;
}

} // namespace livedaw
