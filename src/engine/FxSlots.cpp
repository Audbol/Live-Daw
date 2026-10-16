#include "engine/FxSlots.h"

namespace livedaw
{

struct FxSlots::Holder
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

FxSlots::Holder* FxSlots::holderAt (int slot) const noexcept
{
    // Every slot context registered with the engine is a Holder (see setPlugin).
    return juce::isPositiveAndBelow (slot, kNumFx) ? static_cast<Holder*> (ld::ld_fx_ctx (engine, slot)) : nullptr;
}

void FxSlots::prepare (double sampleRate, int maxBlockSize)
{
    for (int slot = 0; slot < kNumFx; ++slot)
    {
        if (auto* h = holderAt (slot))
        {
            h->plugin->releaseResources();
            h->prepare (sampleRate, maxBlockSize);
        }
    }
}

void FxSlots::setPlugin (int slot, std::unique_ptr<juce::AudioPluginInstance> plugin)
{
    if (! juce::isPositiveAndBelow (slot, kNumFx))
        return;

    if (plugin == nullptr)
    {
        ld::ld_fx_set (engine, slot, nullptr);
        return;
    }

    auto* holder = new Holder();
    holder->plugin = std::move (plugin);
    configureForStereo (*holder->plugin);
    holder->prepare (ld::ld_engine_sample_rate (engine), ld::ld_engine_block_size (engine));

    const ld::InsertCallbacks callbacks { &Holder::process, &Holder::release, holder };
    ld::ld_fx_set (engine, slot, &callbacks);
}

juce::AudioPluginInstance* FxSlots::getPlugin (int slot) const noexcept
{
    auto* h = holderAt (slot);
    return h != nullptr ? h->plugin.get() : nullptr;
}

juce::String FxSlots::describe (int slot) const
{
    juce::String s ("FX " + juce::String (slot + 1));
    if (auto* p = getPlugin (slot))
        s << ": " << p->getName();
    return s;
}

} // namespace livedaw
