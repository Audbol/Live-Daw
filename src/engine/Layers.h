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

/** Fader layers in the style of X32/Wing.

    Default layers are generated from the console whenever it's asked, so they
    always cover every strip: input banks (Ch 1-16, Ch 17-32, ...), aux banks,
    group banks, and "All". User layers hold any strips of the board (inputs,
    buses, master) in any order and are saved with the show.

    Message thread only.
*/
class LayerSet
{
public:
    static constexpr int kDefaultBankSize = 16;

    int getBankSize() const noexcept { return bankSize; }
    void setBankSize (int size) noexcept { bankSize = juce::jlimit (4, 64, size); }

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
    const std::vector<Layer>& getUserLayers() const noexcept { return userLayers; }
    Layer* getUserLayer (const juce::String& id) noexcept;

    /** Creates a user layer and returns its id. */
    juce::String addUserLayer (const juce::String& name, std::vector<state::StripRef> strips = {});
    void removeUserLayer (const juce::String& id);
    void moveUserLayer (const juce::String& id, int delta);

    /** Adds a strip to a user layer (no duplicates). */
    void addToUserLayer (const juce::String& layerId, const state::StripRef&);
    void removeFromUserLayer (const juce::String& layerId, const state::StripRef&);
    void moveWithinUserLayer (const juce::String& layerId, int index, int delta);

    //==============================================================================
    juce::String selectedLayerId { "in-1" };

    std::unique_ptr<juce::XmlElement> toXml() const;
    void fromXml (const juce::XmlElement&);
    void reset();

private:
    int bankSize = kDefaultBankSize;
    std::vector<Layer> userLayers;
};

} // namespace livedaw
