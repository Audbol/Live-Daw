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
    /** Bank size 0 = Auto: each default layer holds as many strips as fit the
        mixer (the UI reports that number with setFittedBankSize()). */
    static constexpr int kAutoBankSize = 0;
    static constexpr int kFallbackBankSize = 16;

    int getBankSize() const noexcept { return bankSize; }
    void setBankSize (int size) noexcept { bankSize = size <= 0 ? kAutoBankSize : juce::jlimit (4, 256, size); }
    bool isAutoBankSize() const noexcept { return bankSize == kAutoBankSize; }

    /** How many strips fit the mixer right now (used when the bank size is Auto). */
    void setFittedBankSize (int size) noexcept { fittedBankSize = juce::jlimit (4, 256, size); }
    int getEffectiveBankSize() const noexcept { return isAutoBankSize() ? fittedBankSize : bankSize; }

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
    int bankSize = kAutoBankSize;
    int fittedBankSize = kFallbackBankSize;
    std::vector<Layer> userLayers;
};

} // namespace livedaw
