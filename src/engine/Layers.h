#pragma once

#include "engine/ConsoleState.h"

#include <juce_core/juce_core.h>

#include <vector>

namespace livedaw
{

/** A fader layer: an ordered set of strips shown together on the mixer page. */
struct Layer
{
    juce::String id;   // stable: "in-1", "aux-1", "groups-1", "all", "user-<uuid>"
    juce::String name;
    std::vector<state::StripRef> strips;
    bool user = false;
};

/** Fader layers in the style of X32/Wing (implemented in Rust, livedaw-session).

    Default layers are generated from the console whenever they're asked for, so
    they always cover every strip: input banks (Ch 1-16, Ch 17-32, ...), aux
    banks, group banks, and "All". User layers hold any strips of the board
    (inputs, buses, master) in any order and are saved with the show.

    Message thread only.
*/
class LayerSet
{
public:
    /** Bank size 0 = Auto: each default layer holds as many strips as fit the
        mixer (the UI reports that number with setFittedBankSize()). */
    static constexpr int kAutoBankSize = 0;
    static constexpr int kFallbackBankSize = 16;

    /** A standalone layer set. */
    LayerSet();
    /** A view of a layer set owned elsewhere (a show's). */
    explicit LayerSet (ld::LayerSet* borrowed) noexcept : handle (borrowed), owned (false) {}
    ~LayerSet();

    int getBankSize() const noexcept { return ld::ld_layers_bank_size (handle); }
    void setBankSize (int size) noexcept { ld::ld_layers_set_bank_size (handle, size); }
    bool isAutoBankSize() const noexcept { return getBankSize() == kAutoBankSize; }

    /** How many strips fit the mixer right now (used when the bank size is Auto). */
    void setFittedBankSize (int size) noexcept { ld::ld_layers_set_fitted_bank_size (handle, size); }
    int getEffectiveBankSize() const noexcept { return ld::ld_layers_effective_bank_size (handle); }

    /** The default layer of the same family (inputs/aux/groups) that contains
        `ref`, for staying on the same strips when the bank size changes. */
    juce::String findDefaultLayerContaining (const MixerEngine&, const state::StripRef&) const;

    /** Default layers for the current console. */
    std::vector<Layer> getDefaultLayers (const MixerEngine&) const;

    /** Default layers followed by user layers. */
    std::vector<Layer> getAllLayers (const MixerEngine&) const;

    /** Finds a layer by id (falls back to the first layer if it no longer exists). */
    Layer findLayer (const MixerEngine&, const juce::String& id) const;

    /** Strips of a layer that currently exist, in layer order. */
    static std::vector<Strip*> resolve (MixerEngine&, const Layer&);

    //==============================================================================
    // User layers
    /** A snapshot of the user layers (refreshed by every call). */
    const std::vector<Layer>& getUserLayers() const;
    /** Points into the snapshot: valid until the next call to either getter. */
    const Layer* getUserLayer (const juce::String& id) const;

    /** Creates a user layer and returns its id. */
    juce::String addUserLayer (const juce::String& name, const std::vector<state::StripRef>& strips = {});
    void removeUserLayer (const juce::String& id);
    void renameUserLayer (const juce::String& id, const juce::String& name);
    void moveUserLayer (const juce::String& id, int delta);

    /** Adds a strip to a user layer (no duplicates). */
    void addToUserLayer (const juce::String& layerId, const state::StripRef&);
    void removeFromUserLayer (const juce::String& layerId, const state::StripRef&);
    void moveWithinUserLayer (const juce::String& layerId, int index, int delta);

    //==============================================================================
    juce::String getSelectedLayerId() const { return takeString (ld::ld_layers_selected (handle)); }
    void setSelectedLayerId (const juce::String& id);

    void reset() noexcept { ld::ld_layers_reset (handle); }

private:
    std::vector<Layer> list (const MixerEngine*, int which, const juce::String& id = {}) const;

    ld::LayerSet* handle;
    bool owned;
    mutable std::vector<Layer> userSnapshot;

    JUCE_DECLARE_NON_COPYABLE (LayerSet)
};

} // namespace livedaw
