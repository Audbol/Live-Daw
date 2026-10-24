#pragma once

#include "engine/Parameters.h"
#include "engine/PluginChain.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>

namespace livedaw
{

/** The 16 FX slots of the EFFECTS screen. Each slot runs one of the
    console's built-in devices (reverbs, delays, EQs, amps... implemented in
    Rust, model table 4) or, with the EXT model, a VST/AU/LV2 plugin; strips use
    a slot through their PRE or POST insert. Premium devices run in slots 1-8. The plugin instances live here (JUCE); the Rust engine calls
    them from the audio thread through a callback per slot, checks their output
    and auto-bypasses a plugin that throws or emits NaN/inf. A slot runs for one
    strip per block: two inserts pointing at the same slot are flagged as
    "double assigned" and the second one passes its signal through.

    Replaced plugins are destroyed on the message thread once the engine can no
    longer call them (the engine invokes the release callback from
    ld_engine_collect). Plugins run in stereo: a mono strip feeds both inputs and
    takes the left output back; a plugin with a mono main bus gets both sides
    mixed and feeds both (MonoBuses). A sandboxed plugin (SandboxedPlugin) that misses a
    block or loses its helper process is reported like one that throws; its latency
    includes the sandbox's block, so delay compensation covers it.

    Each plugin's latency (getLatencySamples) goes to the engine's plugin
    delay compensation: when the plugin is installed, whenever it reports a
    change (from any thread: only an atomic store) and on updateLatencies().

    An EXT slot may hold a serial chain of up to 8 plugins (a PluginChain, run as
    one plugin; a plugin on its own is a chain of one). addToChain(),
    removeFromChain() and moveInChain() build the new chain on the message thread
    and swap it in like setPlugin(); the plugins that stay keep their instance
    and state, and keep running. In a chain of several a failing plugin is
    faulted on its own (passed through) and the slot keeps running; see
    PluginChain.

    Stateless view: any number of FxSlots may view one engine. */
class FxSlots
{
public:
    explicit FxSlots (ld::Engine* e) noexcept : engine (e) {}

    /** Message thread, device stopped: re-prepares the loaded plugins. */
    void prepare (double sampleRate, int maxBlockSize);

    /** Message thread: installs `plugin` in `slot` (nullptr clears it). */
    void setPlugin (int slot, std::unique_ptr<juce::AudioPluginInstance> plugin);

    /** Message thread only. */
    juce::AudioPluginInstance* getPlugin (int slot) const noexcept;
    /** Message thread: the slot's plugin (or a plugin of its chain) runs in the plugin sandbox (a helper process). */
    bool isSandboxed (int slot) const noexcept;

    //==============================================================================
    // Plugin chains (message thread). Member indices are in processing order.

    /** The slot's PluginChain, or nullptr (no plugin, or a plugin on its own). */
    PluginChain* getChain (int slot) const noexcept;
    /** Plugins in the slot: 0, 1 for a plugin on its own, else the chain's size. */
    int getNumChainMembers (int slot) const noexcept;
    juce::AudioPluginInstance* getChainMember (int slot, int index) const noexcept;
    bool isChainMemberBypassed (int slot, int index) const noexcept;
    /** No latency while bypassed. Bypassing a plugin on its own makes it a chain
        of one. Bypassing on or off clears the plugin's fault. */
    void setChainMemberBypassed (int slot, int index, bool bypassed);
    /** The plugin failed in a chain of several and is passed through until cleared. */
    bool isChainMemberFaulted (int slot, int index) const noexcept;
    /** Un-faults the plugin: it runs again. */
    void clearChainMemberFault (int slot, int index);
    /** Plugins of the slot's chain that are faulted on their own (the slot isn't). */
    int getNumFaultedChainMembers (int slot) const noexcept;
    /** How the chain window shows a plugin (zoom, collapsed; saved with the show). */
    PluginChain::View getChainMemberView (int slot, int index) const noexcept;
    /** A view other than the default for a plugin on its own makes it a chain of one. */
    void setChainMemberView (int slot, int index, PluginChain::View);
    /** Inserts `plugin` at `index` (-1: the end). False (and `plugin` dropped) if
        the slot has no plugin yet or its chain is full. */
    bool addToChain (int slot, std::unique_ptr<juce::AudioPluginInstance> plugin, int index = -1);
    /** Removes a member (the last one empties the slot). It is destroyed once
        the engine can no longer call it: close its editor first. */
    bool removeFromChain (int slot, int index);
    /** Moves a member to position `to`. */
    bool moveInChain (int slot, int from, int to);

    /** The slot's model (index into model table 4: kFxNone, kFxExt or a device). */
    int getModel (int slot) const noexcept { return params (slot).model.load(); }
    /** Selects a built-in device (removing a plugin first). False if not allowed in the slot. */
    bool setModel (int slot, int model);
    /** Whether `model` may run in `slot` (premium devices need slots 1-8). */
    static bool isAllowed (int model, int slot);

    /** The slot is in use (a plugin or a built-in device). */
    bool isLoaded (int slot) const noexcept { return hasPlugin (slot) || getModel (slot) > ld::kFxExt; }
    bool hasPlugin (int slot) const noexcept { return ld::ld_fx_state (engine, slot, 0); }
    bool hasDevice (int slot) const noexcept { return ld::ld_fx_state (engine, slot, 3); }
    /** Gain reduction of the slot's device (dB, <= 0). */
    float getMeter (int slot) const noexcept { return ld::ld_fx_meter (engine, slot); }
    bool isFaulted (int slot) const noexcept { return ld::ld_fx_state (engine, slot, 1); }
    bool isDoubleAssigned (int slot) const noexcept { return ld::ld_fx_state (engine, slot, 2); }
    /** Clears the slot's fault and double assignment, and its chain's faulted plugins. */
    void clearFlags (int slot);
    /** Share of the block time the plugin took (smoothed). */
    float getLoad (int slot) const noexcept { return ld::ld_fx_load (engine, slot); }
    /** Number of strip inserts using the slot. */
    int getNumUsers (int slot) const noexcept { return ld::ld_fx_num_users (engine, slot); }
    /** The latency the slot's plugin reports, in samples (0 without one). */
    int getLatency (int slot) const noexcept { return ld::ld_fx_latency (engine, slot); }

    /** Message thread: passes every plugin's current latency to the engine (a
        safety net for plugins that change it without telling their host).
        Returns true if one changed. */
    bool updateLatencies();

    FxParams& params (int slot) const noexcept { return ld::ld_engine_controls (engine)->fx[juce::jlimit (0, kNumFx - 1, slot)]; }

    /** "FX 3: Hall Reverb", "FX 3: TAL-Reverb" or "FX 3" for an empty slot. */
    juce::String describe (int slot) const;

private:
    struct Holder;
    Holder* holderAt (int slot) const noexcept;
    /** `rearranged`: `plugin` is a chain of the slot's plugins (added to, moved, removed from), so
        a faulted (auto-bypassed) slot stays faulted until the fault is cleared. */
    void install (int slot, std::shared_ptr<juce::AudioPluginInstance> plugin, bool rearranged = false);
    /** The slot's plugins as chain members (wrapping a plugin on its own). */
    std::vector<PluginChain::MemberPtr> members (int slot) const;

    ld::Engine* engine;
};

} // namespace livedaw
