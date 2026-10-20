#pragma once

#include "engine/Parameters.h"

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
    takes the left output back. A sandboxed plugin (SandboxedPlugin) that misses a
    block or loses its helper process is reported like one that throws; its latency
    includes the sandbox's block, so delay compensation covers it.

    Each plugin's latency (getLatencySamples) goes to the engine's plugin
    delay compensation: when the plugin is installed, whenever it reports a
    change (from any thread: only an atomic store) and on updateLatencies().

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
    /** Message thread: the slot's plugin runs in the plugin sandbox (a helper process). */
    bool isSandboxed (int slot) const noexcept;

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
    void clearFlags (int slot) noexcept { ld::ld_fx_clear_flags (engine, slot); }
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

    ld::Engine* engine;
};

} // namespace livedaw
