#pragma once

#include "engine/Parameters.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>

namespace livedaw
{

/** The 16 FX slots of the EFFECTS screen. Each slot can hold a VST/AU/LV2
    plugin (the WING's "EXT" device); strips use a slot through their PRE or
    POST insert. The plugin instances live here (JUCE); the Rust engine calls
    them from the audio thread through a callback per slot, checks their output
    and auto-bypasses a plugin that throws or emits NaN/inf. A slot runs for one
    strip per block: two inserts pointing at the same slot are flagged as
    "double assigned" and the second one passes its signal through.

    Replaced plugins are destroyed on the message thread once the engine can no
    longer call them (the engine invokes the release callback from
    ld_engine_collect). Plugins run in stereo: a mono strip feeds both inputs and
    takes the left output back.

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

    bool isLoaded (int slot) const noexcept { return ld::ld_fx_state (engine, slot, 0); }
    bool isFaulted (int slot) const noexcept { return ld::ld_fx_state (engine, slot, 1); }
    bool isDoubleAssigned (int slot) const noexcept { return ld::ld_fx_state (engine, slot, 2); }
    void clearFlags (int slot) noexcept { ld::ld_fx_clear_flags (engine, slot); }
    /** Share of the block time the plugin took (smoothed). */
    float getLoad (int slot) const noexcept { return ld::ld_fx_load (engine, slot); }
    /** Number of strip inserts using the slot. */
    int getNumUsers (int slot) const noexcept { return ld::ld_fx_num_users (engine, slot); }

    FxParams& params (int slot) const noexcept { return ld::ld_engine_controls (engine)->fx[juce::jlimit (0, kNumFx - 1, slot)]; }

    /** "FX 3: TAL-Reverb" or "FX 3" for an empty slot. */
    juce::String describe (int slot) const;

private:
    struct Holder;
    Holder* holderAt (int slot) const noexcept;

    ld::Engine* engine;
};

} // namespace livedaw
