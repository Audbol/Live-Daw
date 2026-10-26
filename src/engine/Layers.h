#pragma once

#include "engine/ConsoleState.h"

#include <juce_core/juce_core.h>

#include <optional>
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
    std::vector<state::FaderRef> faders; // blank slots are FaderRef::blank()
    bool user = false;
};

/** Fixed-slot editing of a layer's faders, as on the WING: slot `i` is
    `faders[i]`, or blank past the end. Only insert() moves other slots (to
    the right), and trailing blanks are dropped (the slots past the end are
    blank anyway). A layer can hold the same fader in several slots. The
    VIEW > CONFIG screen edits layers with these. */
namespace layerslots
{
    /** The most slots a layer can have (livedaw-session's MAX_SLOTS). */
    int maxSlots();
    /** The fader in slot `i` (blank past the end or for a negative index). */
    state::FaderRef get (const std::vector<state::FaderRef>&, int i);
    void trim (std::vector<state::FaderRef>&);
    /** Puts `f` in slot `i`, replacing what was there; blanks fill any gap.
        Slots outside 0 to maxSlots() - 1 are ignored. */
    void set (std::vector<state::FaderRef>&, int i, const state::FaderRef& f);
    void clear (std::vector<state::FaderRef>&, int i);
    /** Swaps slots `a` and `b`: a filled slot dropped on a blank one moves
        there, on a filled one the two change places. */
    void swap (std::vector<state::FaderRef>&, int a, int b);
    /** Items picked on the CONFIG screen put in slot `index`: they fill
        `index`, `index + 1`, ..., replacing what's there. An item the layer
        already holds elsewhere stays there too (a second copy). False, with
        nothing changed, when the items would go past maxSlots(). */
    bool place (std::vector<state::FaderRef>&, const std::vector<state::FaderRef>& items, int index);
    /** Inserts `items` (blank ones too) before slot `index`: it and the slots
        after it move `items.size()` places to the right. Blanks pushed past
        maxSlots() are dropped; when a filled slot would be, nothing changes and
        this returns false (the layer is full). */
    bool insert (std::vector<state::FaderRef>&, int index, const std::vector<state::FaderRef>& items);

    /** Where a drop on CONFIG's slots lands: on slot `index` (fill or replace
        it) or, with `insert`, between slots `index - 1` and `index` (insert
        there). index -1: not on the slots. */
    struct DropTarget
    {
        int index = -1;
        bool insert = false;
        bool operator== (const DropTarget& o) const noexcept { return index == o.index && insert == o.insert; }
    };
    /** Items from CONFIG's grid dropped (or tapped) on `target`: place() on a
        slot, insert() between two. False when refused (the layer is full). */
    bool dropItems (std::vector<state::FaderRef>&, const std::vector<state::FaderRef>& items, DropTarget target);
    /** Slot `from` dragged to `target`: onto a slot it swaps (moves into a
        blank one); between two slots it's inserted there and its old slot
        turns blank (next to itself nothing changes). False when refused. */
    bool dropSlot (std::vector<state::FaderRef>&, int from, DropTarget target);
} // namespace layerslots

/** The WING's three fader sections and their layer keys (implemented in Rust,
    livedaw-session).

    Left section: 12 faders, center: 8, right: 4, each with its own layer
    selection and the WING's seven layer keys (manual 4.2):
      left:   1-12, 13-24, 25-36, 37-40 AUX IN, BUS MASTER, USER 1, USER 2
      center: DCA, MAIN MATRIX, AUX IN, BUS MASTER, USER 1, USER 2
      right:  MAIN MATRIX, DCA, INPUTS, AUX IN, BUS MASTER, USER 1, USER 2
    They're generated from the console whenever they're asked for, so they
    always cover every strip; a layer longer than its section is scrolled with
    the shift keys. USER 1 / USER 2 show the first two user layers, which hold
    any faders (strips, DCAs, sends, FX parameters, MIDI CCs) in fixed slots
    (a slot can be blank, a fader can sit in several) and are saved with the show. Layer ids of older
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
    /** What the surface's strip shows for slot `index` of `layer`: nothing
        (an inactive strip) for a blank slot or past the end. */
    static std::optional<state::FaderRef> surfaceTarget (const Layer& layer, int index);

    /** The layer the section shows ("spill" while it spills). */
    juce::String getSelectedLayerId (Section s) const { return takeString (ld::ld_layers_selected (handle, s)); }
    /** Selects a layer key (ends a spill on the section without going back). */
    void setSelectedLayerId (Section, const juce::String& id);

    /** The first fader the section shows (its scroll position, kept here so a
        spill can bring it back). */
    int getOffset (Section s) const noexcept { return ld::ld_layers_offset (handle, s); }
    void setOffset (Section s, int offset) noexcept { ld::ld_layers_set_offset (handle, s, offset); }

    //==============================================================================
    // Spill: a computed layer on a section for a while (a DCA's members, the
    // sources sending to a bus, an FX slot's parameters, an automix group, a
    // strip's sends). Ending it brings back the layer and scroll position.
    struct Spill
    {
        enum class Kind
        {
            Dca = 0,
            Bus = 1,     // index: the bus slot
            Fx = 2,      // index: the FX slot
            Automix = 3, // index: 0 X, 1 Y
            Sends = 4    // strip: whose sends
        };
        Kind kind = Kind::Dca;
        int index = 0;
        state::StripRef strip;

        bool operator== (const Spill& o) const noexcept
        {
            return kind == o.kind && (kind == Kind::Sends ? strip == o.strip : index == o.index);
        }
    };

    static constexpr const char* kSpillId = "spill";

    /** Spill from the right section goes to the center, everything else to the left. */
    static Section spillTarget (std::optional<Section> origin) noexcept
    {
        return static_cast<Section> (ld::ld_layers_spill_target (origin.has_value() ? (int32_t) *origin : -1));
    }
    void startSpill (Section, const Spill&);
    /** False if the section wasn't spilling. */
    bool endSpill (Section s) noexcept { return ld::ld_layers_end_spill (handle, s); }
    std::optional<Spill> getSpill (Section) const;
    /** The section spilling `spill`, if any. */
    std::optional<Section> findSpill (const Spill&) const;

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
    /** Into the first blank slot or after the last (another copy when the
        layer already holds it); false when the layer is full. */
    bool addToUserLayer (const juce::String& layerId, const state::FaderRef&);
    /** Blanks slot `index` if it holds the fader (-1: the first slot holding
        it); other copies stay. */
    void removeFromUserLayer (const juce::String& layerId, const state::FaderRef&, int index = -1);
    void moveWithinUserLayer (const juce::String& layerId, int index, int delta);
    /** Replaces a user layer's faders, blank slots included. */
    void setUserLayerFaders (const juce::String& layerId, const std::vector<state::FaderRef>&);
    /** Puts a fader in slot `index` (replacing what was there); the other slots stay. */
    void setUserLayerSlot (const juce::String& layerId, int index, const state::FaderRef&);
    void clearUserLayerSlot (const juce::String& layerId, int index);
    /** Swaps two slots (a move when one of them is blank). */
    void swapUserLayerSlots (const juce::String& layerId, int a, int b);
    /** Inserts faders (blanks too) before slot `index`, moving the rest right
        (layerslots::insert); false, nothing changed, when the layer is full. */
    bool insertUserLayerSlots (const juce::String& layerId, int index, const std::vector<state::FaderRef>&);

    //==============================================================================
    // Edited default layers ("the default layers are entirely user-assignable"):
    // the faders a section's default key shows instead of the generated ones.
    // Not for USER 1 / USER 2, which show user layers.
    void setDefaultOverride (Section, const juce::String& id, const std::vector<state::FaderRef>&);
    void clearDefaultOverride (Section, const juce::String& id);
    bool hasDefaultOverride (Section, const juce::String& id) const;

    void reset() noexcept { ld::ld_layers_reset (handle); }

private:
    std::vector<Layer> list (const MixerEngine*, Section, int which, const juce::String& id = {}) const;

    ld::LayerSet* handle;
    bool owned;
    mutable std::vector<Layer> userSnapshot;

    JUCE_DECLARE_NON_COPYABLE (LayerSet)
};

} // namespace livedaw
