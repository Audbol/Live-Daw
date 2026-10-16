#pragma once

#include "engine/ConsoleState.h"

#include <juce_core/juce_core.h>

#include <vector>

namespace livedaw
{

/** A fader layer (bank): an ordered set of faders shown together on a fader section. */
struct Layer
{
    juce::String id;   // stable: "in-1", "bus-1", "dca-1", "out-1", "all", "user-<uuid>"
    juce::String name; // "CH 1-12", "BUS 1-8", ...
    std::vector<state::FaderRef> faders;
    bool user = false;
};

/** The WING's three fader sections and their banks (implemented in Rust,
    livedaw-session).

    Left section: 12 faders, center: 8, right: 4, each with its own bank
    selection. Default banks are generated from the console whenever they're
    asked for, so they always cover every strip: CH 1-12 ... CH 37-40 · AUX 1-8,
    BUS, DCA, MAIN · MTX and ALL, cut to the section's width. User banks hold
    any faders (strips and DCAs) and are saved with the show. A section's width
    can be "Auto" to fit as many faders as the window allows.

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

    std::vector<Layer> getDefaultLayers (const MixerEngine&, Section) const;
    /** Default banks followed by user banks. */
    std::vector<Layer> getAllLayers (const MixerEngine&, Section) const;
    /** Finds a layer by id (falls back to the section's first layer if it no longer exists). */
    Layer findLayer (const MixerEngine&, Section, const juce::String& id) const;
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
