#include "engine/FxSlots.h"
#include "engine/Models.h"
#include "engine/SandboxedPlugin.h"

namespace livedaw
{

struct FxSlots::Holder final : private juce::AudioProcessorListener
{
    Holder (ld::Engine* e, int s) : engine (e), slot (s) {}

    ld::Engine* engine;
    int slot;
    /** Cleared once replaced: a retired plugin must not report into its old slot. */
    std::atomic<bool> installed { true };
    /** Shared with a PluginChain that takes the plugin over (FxSlots::addToChain). */
    std::shared_ptr<juce::AudioPluginInstance> plugin;
    SandboxedPlugin* sandboxed = nullptr; // the plugin, if it runs in the sandbox
    PluginChain* chain = nullptr;         // the plugin, if it's a chain
    juce::AudioBuffer<float> scratch;
    juce::MidiBuffer midi;
    int numPluginChannels = 2;
    int blockSize = 512;
    double sampleRate = 0.0;

    void prepare (double rate, int maxBlock)
    {
        blockSize = juce::jmax (1, maxBlock);
        sampleRate = rate;
        sandboxed = dynamic_cast<SandboxedPlugin*> (plugin.get());
        chain = dynamic_cast<PluginChain*> (plugin.get());
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
                if (h.sandboxed != nullptr && h.sandboxed->didLastBlockFail())
                    return 1; // timed out or the helper is gone: same as a plugin error
                if (h.chain != nullptr && h.chain->didLastBlockFail())
                    return 1; // a member failed (the chain knows which)

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
        auto* h = static_cast<Holder*> (ctx);
        h->plugin->removeListener (h);
        delete h;
    }

    /** Message thread: reports the latency to the engine and follows its changes. */
    void listen()
    {
        reportLatency();
        plugin->addListener (this);
    }

    /** Any thread (a plugin may change its latency inside processBlock): an atomic store. */
    void reportLatency()
    {
        if (installed.load())
            ld::ld_fx_set_latency (engine, slot, plugin->getLatencySamples());
    }

private:
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails& details) override
    {
        if (details.latencyChanged)
            reportLatency();
    }

    void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override {}
};

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
            h->reportLatency(); // plugins may change it in prepareToPlay
        }
    }
}

void FxSlots::setPlugin (int slot, std::unique_ptr<juce::AudioPluginInstance> plugin)
{
    install (slot, std::move (plugin));
}

void FxSlots::install (int slot, std::shared_ptr<juce::AudioPluginInstance> plugin)
{
    if (! juce::isPositiveAndBelow (slot, kNumFx))
        return;

    if (auto* old = holderAt (slot))
        old->installed = false;

    if (plugin == nullptr)
    {
        ld::ld_fx_set (engine, slot, nullptr);
        return;
    }

    auto* holder = new Holder (engine, slot);
    holder->plugin = std::move (plugin);
    if (dynamic_cast<PluginChain*> (holder->plugin.get()) == nullptr) // a chain is stereo (its members may be running)
        PluginChain::configureForStereo (*holder->plugin);
    holder->prepare (ld::ld_engine_sample_rate (engine), ld::ld_engine_block_size (engine));

    const ld::InsertCallbacks callbacks { &Holder::process, &Holder::release, holder };
    ld::ld_fx_set (engine, slot, &callbacks);
    holder->listen(); // after ld_fx_set, which resets the slot's latency
}

bool FxSlots::updateLatencies()
{
    bool changed = false;
    for (int slot = 0; slot < kNumFx; ++slot)
    {
        auto* h = holderAt (slot);
        if (h != nullptr && h->chain != nullptr)
            h->chain->updateLatency(); // its members may have changed theirs quietly
        if (h != nullptr && h->plugin->getLatencySamples() != getLatency (slot))
        {
            h->reportLatency();
            changed = true;
        }
    }
    return changed;
}

juce::AudioPluginInstance* FxSlots::getPlugin (int slot) const noexcept
{
    auto* h = holderAt (slot);
    return h != nullptr ? h->plugin.get() : nullptr;
}

bool FxSlots::isSandboxed (int slot) const noexcept
{
    auto* h = holderAt (slot);
    return h != nullptr && (h->sandboxed != nullptr || (h->chain != nullptr && h->chain->hasSandboxedMember()));
}

//==============================================================================
PluginChain* FxSlots::getChain (int slot) const noexcept
{
    auto* h = holderAt (slot);
    return h != nullptr ? h->chain : nullptr;
}

int FxSlots::getNumChainMembers (int slot) const noexcept
{
    auto* h = holderAt (slot);
    return h == nullptr ? 0 : (h->chain != nullptr ? h->chain->size() : 1);
}

juce::AudioPluginInstance* FxSlots::getChainMember (int slot, int index) const noexcept
{
    auto* h = holderAt (slot);
    if (h == nullptr || ! juce::isPositiveAndBelow (index, getNumChainMembers (slot)))
        return nullptr;
    return h->chain != nullptr ? &h->chain->getMember (index) : h->plugin.get();
}

bool FxSlots::isChainMemberBypassed (int slot, int index) const noexcept
{
    auto* chain = getChain (slot);
    return chain != nullptr && juce::isPositiveAndBelow (index, chain->size()) && chain->isBypassed (index);
}

std::vector<PluginChain::MemberPtr> FxSlots::members (int slot) const
{
    auto* h = holderAt (slot);
    if (h == nullptr)
        return {};
    if (h->chain != nullptr)
        return h->chain->getMembers();
    // A plugin on its own: running, prepared by its holder.
    return { PluginChain::makeMember (h->plugin, false, h->sampleRate, h->blockSize) };
}

void FxSlots::setChainMemberBypassed (int slot, int index, bool bypassed)
{
    if (! juce::isPositiveAndBelow (index, getNumChainMembers (slot)))
        return;
    if (getChain (slot) == nullptr)
    {
        if (! bypassed)
            return;
        install (slot, std::make_shared<PluginChain> (members (slot)));
    }
    getChain (slot)->setBypassed (index, bypassed);
}

bool FxSlots::isChainMemberFaulted (int slot, int index) const noexcept
{
    auto* chain = getChain (slot);
    return chain != nullptr && juce::isPositiveAndBelow (index, chain->size()) && chain->isFaulted (index);
}

void FxSlots::clearChainMemberFault (int slot, int index)
{
    if (auto* chain = getChain (slot))
        chain->clearFault (index);
}

int FxSlots::getNumFaultedChainMembers (int slot) const noexcept
{
    auto* chain = getChain (slot);
    return chain != nullptr ? chain->getNumFaulted() : 0;
}

void FxSlots::clearFlags (int slot)
{
    ld::ld_fx_clear_flags (engine, slot);
    if (auto* chain = getChain (slot))
        for (int i = 0; i < chain->size(); ++i)
            if (chain->isFaulted (i))
                chain->clearFault (i);
}

PluginChain::View FxSlots::getChainMemberView (int slot, int index) const noexcept
{
    auto* chain = getChain (slot);
    return chain != nullptr && juce::isPositiveAndBelow (index, chain->size()) ? chain->getView (index) : PluginChain::View {};
}

void FxSlots::setChainMemberView (int slot, int index, PluginChain::View view)
{
    if (! juce::isPositiveAndBelow (index, getNumChainMembers (slot)))
        return;
    if (getChain (slot) == nullptr)
    {
        if (view.isDefault())
            return;
        install (slot, std::make_shared<PluginChain> (members (slot)));
    }
    getChain (slot)->setView (index, view);
}

bool FxSlots::addToChain (int slot, std::unique_ptr<juce::AudioPluginInstance> plugin, int index)
{
    auto list = members (slot);
    if (plugin == nullptr || list.empty() || (int) list.size() >= PluginChain::kMaxMembers)
        return false;
    if (! juce::isPositiveAndNotGreaterThan (index, (int) list.size()))
        index = (int) list.size();
    list.insert (list.begin() + index, PluginChain::makeMember (std::move (plugin)));
    install (slot, std::make_shared<PluginChain> (std::move (list)));
    return true;
}

bool FxSlots::removeFromChain (int slot, int index)
{
    auto list = members (slot);
    if (! juce::isPositiveAndBelow (index, (int) list.size()))
        return false;
    list.erase (list.begin() + index);
    if (list.empty())
        install (slot, nullptr);
    else
        install (slot, std::make_shared<PluginChain> (std::move (list)));
    return true;
}

bool FxSlots::moveInChain (int slot, int from, int to)
{
    auto list = members (slot);
    const int n = (int) list.size();
    if (! juce::isPositiveAndBelow (from, n) || ! juce::isPositiveAndBelow (to, n) || from == to)
        return false;
    auto member = list[(size_t) from];
    list.erase (list.begin() + from);
    list.insert (list.begin() + to, std::move (member));
    install (slot, std::make_shared<PluginChain> (std::move (list)));
    return true;
}


bool FxSlots::isAllowed (int model, int slot)
{
    const auto& table = models::table (models::Fx);
    if (! juce::isPositiveAndBelow (model, (int) table.size()))
        return false;
    return table[(size_t) model].category != models::Category::FxPremium || slot < 8;
}

bool FxSlots::setModel (int slot, int model)
{
    if (! juce::isPositiveAndBelow (slot, kNumFx) || ! isAllowed (model, slot))
        return false;
    if (model == ld::kFxExt)
        return true; // EXT: the slot becomes a plugin slot when a plugin is loaded
    if (hasPlugin (slot))
        setPlugin (slot, nullptr);
    return ld::ld_fx_set_model (engine, slot, model, true);
}

juce::String FxSlots::describe (int slot) const
{
    juce::String s ("FX " + juce::String (slot + 1));
    if (auto* p = getPlugin (slot))
        s << ": " << p->getName();
    else if (const int m = getModel (slot); m > ld::kFxExt)
        s << ": " << models::model (models::Fx, m).name;
    return s;
}

} // namespace livedaw
