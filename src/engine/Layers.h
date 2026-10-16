#pragma once

#include "engine/ConsoleState.h"

#include <juce_core/juce_core.h>

#include <vector>

namespace livedaw
{

/** A fader layer (bank): an ordered set of faders shown together on a fader section. */
struct Layer
{
    // Stable: "in-1".."in-<n>", "bus-1", "dca-1", "out-1", "aux", "inputs",
    // "user-1", "user-2" (the USER keys) or "user-<uuid>" (a user layer).
    juce::String id;
    juce::String name; // "CH 1-12", "BUS 1-16", "MAIN 1-4 · MTX 1-8", a user layer's name
    std::vector<state::FaderRef> faders;
    bool user = false;
};

/** The WING's three fader sections and their layer keys (implemented in Rust,
    livedaw-session).

    Left section: 12 faders, center: 8, right: 4, each with its own layer
    selection and the WING's seven layer keys (manual 4.2):
      left:   1-12, 13-24, 25-36, 37-40 AUX IN, BUS MASTER, USER 1, USER 2
      center: DCA, MAIN MATRIX, AUX IN, INPUTS, BUS MASTER, USER 1, USER 2
      right:  MAIN MATRIX, DCA, INPUTS, AUX IN, BUS MASTER, USER 1, USER 2
    They're generated from the console whenever they're asked for, so they
    always cover every strip; a layer longer than its section is scrolled with
    the shift keys. USER 1 / USER 2 show the first two user layers, which hold
    any faders (strips and DCAs) and are saved with the show. Layer ids of older
    shows ("bus-2", "dca-2", "all", ...) resolve to the key that holds those
    faders now. A section's width can be "Auto" to fit as many faders as the
    window allows.

    Message thread only.
*/
class LayerSet
{
public:
    enum Section
    {
        Left = 0,
        Center = 1,
        Right = 2
    };

    static constexpr int kNumSections = 3;
    static constexpr int kAutoWidth = 0;
    static constexpr int kWingWidths[kNumSections] = { 12, 8, 4 };

    /** A standalone layer set. */
    LayerSet();
    /** A view of a layer set owned elsewhere (a show's). */
    explicit LayerSet (ld::LayerSet* borrowed) noexcept : handle (borrowed), owned (false) {}
    ~LayerSet();

    /** Width setting of a section (0 = Auto). */
    int getWidth (Section s) const noexcept { return ld::ld_layers_width (handle, s); }
    void setWidth (Section s, int width) noexcept { ld::ld_layers_set_width (handle, s, width); }
    bool isAutoWidth (Section s) const noexcept { return getWidth (s) == kAutoWidth; }
    /** How many faders fit the section right now (used when its width is Auto). */
    void setFittedWidth (Section s, int width) noexcept { ld::ld_layers_set_fitted_width (handle, s, width); }
    int getEffectiveWidth (Section s) const noexcept { return ld::ld_layers_effective_width (handle, s); }

    /** The section's default bank containing `ref` (for "go to strip"). */
    juce::String findDefaultLayerContaining (const MixerEngine&, Section, const state::FaderRef&) const;

    /** The section's layer keys, top to bottom. */
    std::vector<Layer> getDefaultLayers (const MixerEngine&, Section) const;
    /** The layer keys followed by every user layer. */
    std::vector<Layer> getAllLayers (const MixerEngine&, Section) const;
    /** Finds a layer by id (ids of older shows map to their new key; anything
        else falls back to the section's first layer). */
    Layer findLayer (const MixerEngine&, Section, const juce::String& id) const;

    /** The label printed on a layer key, '\n' between its two lines:
        "1-12", "37-40\nAUX IN", "BUS\nMASTER", "MAIN\nMATRIX", "DCA", "USER 1". */
    static juce::String keyLabel (const Layer&);
    Layer getSelectedLayer (const MixerEngine& e, Section s) const { return findLayer (e, s, getSelectedLayerId (s)); }

    juce::String getSelectedLayerId (Section s) const { return takeString (ld::ld_layers_selected (handle, s)); }
    void setSelectedLayerId (Section, const juce::String& id);

    //==============================================================================
    // User layers (shared by all sections)
    /** A snapshot of the user layers (refreshed by every call). */
    const std::vector<Layer>& getUserLayers() const;
    /** Points into the snapshot: valid until the next call to either getter. */
    const Layer* getUserLayer (const juce::String& id) const;

    juce::String addUserLayer (const juce::String& name, const std::vector<state::FaderRef>& faders = {});
    void removeUserLayer (const juce::String& id);
    void renameUserLayer (const juce::String& id, const juce::String& name);
    void moveUserLayer (const juce::String& id, int delta);
    void addToUserLayer (const juce::String& layerId, const state::FaderRef&);
    void removeFromUserLayer (const juce::String& layerId, const state::FaderRef&);
    void moveWithinUserLayer (const juce::String& layerId, int index, int delta);

    void reset() noexcept { ld::ld_layers_reset (handle); }

private:
    std::vector<Layer> list (const MixerEngine*, Section, int which, const juce::String& id = {}) const;

    ld::LayerSet* handle;
    bool owned;
    mutable std::vector<Layer> userSnapshot;

    JUCE_DECLARE_NON_COPYABLE (LayerSet)
};

} // namespace livedaw
