#include "engine/Layers.h"

namespace livedaw
{

namespace layerslots
{
    state::FaderRef get (const std::vector<state::FaderRef>& faders, int i)
    {
        return juce::isPositiveAndBelow (i, (int) faders.size()) ? faders[(size_t) i] : state::FaderRef::blank();
    }

    void trim (std::vector<state::FaderRef>& faders)
    {
        while (! faders.empty() && faders.back().isEmpty())
            faders.pop_back();
    }

    void set (std::vector<state::FaderRef>& faders, int i, const state::FaderRef& f)
    {
        if (i < 0)
            return;
        if ((int) faders.size() <= i)
            faders.resize ((size_t) i + 1, state::FaderRef::blank());
        faders[(size_t) i] = f;
        trim (faders);
    }

    void clear (std::vector<state::FaderRef>& faders, int i) { set (faders, i, state::FaderRef::blank()); }

    void swap (std::vector<state::FaderRef>& faders, int a, int b)
    {
        if (a < 0 || b < 0 || a == b)
            return;
        const auto fa = get (faders, a), fb = get (faders, b);
        set (faders, a, fb);
        set (faders, b, fa);
    }

    void place (std::vector<state::FaderRef>& faders, const std::vector<state::FaderRef>& items, int index)
    {
        if (index < 0)
            return;
        int slot = index;
        for (const auto& item : items)
        {
            if (item.isEmpty())
                continue;
            // Already elsewhere in the layer: it moves (its old slot turns blank).
            for (int i = 0; i < (int) faders.size(); ++i)
                if (i != slot && faders[(size_t) i] == item)
                    faders[(size_t) i] = state::FaderRef::blank();
            set (faders, slot++, item);
        }
        trim (faders);
    }
} // namespace layerslots

std::optional<state::FaderRef> LayerSet::surfaceTarget (const Layer& layer, int index)
{
    const auto f = layerslots::get (layer.faders, index);
    return f.isEmpty() ? std::nullopt : std::optional<state::FaderRef> (f);
}

LayerSet::LayerSet() : handle (ld::ld_layers_create()), owned (true) {}

LayerSet::~LayerSet()
{
    if (owned)
        ld::ld_layers_destroy (handle);
}

std::vector<Layer> LayerSet::list (const MixerEngine* engine, Section section, int which, const juce::String& id) const
{
    const Utf8 u (id);
    auto* l = ld::ld_layers_list (handle, engine != nullptr ? engine->getHandle() : nullptr, section, which, u.data, u.size);

    std::vector<Layer> layers;
    const int n = ld::ld_layer_list_len (l);
    layers.reserve ((size_t) n);

    std::vector<ld::FaderRefC> refs;
    for (int i = 0; i < n; ++i)
    {
        Layer layer;
        layer.id = takeString (ld::ld_layer_list_id (l, i));
        layer.name = takeString (ld::ld_layer_list_name (l, i));
        layer.user = ld::ld_layer_list_is_user (l, i);

        refs.resize ((size_t) ld::ld_layer_list_faders (l, i, nullptr, 0));
        ld::ld_layer_list_faders (l, i, refs.data(), (int32_t) refs.size());
        for (auto r : refs)
            layer.faders.push_back (state::FaderRef::fromC (r));

        layers.push_back (std::move (layer));
    }

    ld::ld_layer_list_free (l);
    return layers;
}

juce::String LayerSet::findDefaultLayerContaining (const MixerEngine& engine, Section s, const state::FaderRef& ref) const
{
    return takeString (ld::ld_layers_find_default_containing (handle, engine.getHandle(), s, ref.toC()));
}

std::vector<Layer> LayerSet::getDefaultLayers (const MixerEngine& e, Section s) const { return list (&e, s, 0); }
std::vector<Layer> LayerSet::getAllLayers (const MixerEngine& e, Section s) const     { return list (&e, s, 1); }

Layer LayerSet::findLayer (const MixerEngine& engine, Section s, const juce::String& id) const
{
    auto layers = list (&engine, s, 3, id);
    return layers.empty() ? Layer {} : std::move (layers.front());
}

juce::String LayerSet::keyLabel (const Layer& layer)
{
    if (layer.id.startsWith ("in-"))
    {
        // "CH 37-40 · AUX 1-8" -> "37-40" / "AUX IN"
        juce::StringArray lines;
        for (auto part : juce::StringArray::fromTokens (layer.name, juce::String::fromUTF8 ("\xc2\xb7"), {}))
        {
            part = part.trim();
            if (part.startsWith ("CH "))
                lines.add (part.fromFirstOccurrenceOf ("CH ", false, false));
            else if (part.startsWith ("AUX"))
                lines.add ("AUX IN");
        }
        return lines.isEmpty() ? juce::String ("INPUTS") : lines.joinIntoString ("\n");
    }

    static const std::pair<const char*, const char*> keys[] = {
        { "bus-1", "BUS\nMASTER" }, { "dca-1", "DCA" },       { "out-1", "MAIN\nMATRIX" }, { "aux", "AUX IN" },
        { "inputs", "INPUTS" },     { "user-1", "USER 1" },   { "user-2", "USER 2" },
    };
    for (auto [id, label] : keys)
        if (layer.id == id)
            return label;
    return layer.name.toUpperCase();
}

void LayerSet::startSpill (Section s, const Spill& spill)
{
    ld::ld_layers_start_spill (handle, s, ld::SpillC { (int32_t) spill.kind, spill.index, spill.strip.toC() });
}

std::optional<LayerSet::Spill> LayerSet::getSpill (Section s) const
{
    ld::SpillC c {};
    if (! ld::ld_layers_spill (handle, s, &c))
        return std::nullopt;
    return Spill { static_cast<Spill::Kind> (c.kind), c.index, state::StripRef::fromC (c.strip) };
}

std::optional<LayerSet::Section> LayerSet::findSpill (const Spill& spill) const
{
    for (int i = 0; i < kNumSections; ++i)
        if (const auto s = getSpill (static_cast<Section> (i)); s && *s == spill)
            return static_cast<Section> (i);
    return std::nullopt;
}

void LayerSet::setSelectedLayerId (Section s, const juce::String& id)
{
    const Utf8 u (id);
    ld::ld_layers_set_selected (handle, s, u.data, u.size);
}

//==============================================================================
const std::vector<Layer>& LayerSet::getUserLayers() const
{
    userSnapshot = list (nullptr, Left, 2);
    return userSnapshot;
}

const Layer* LayerSet::getUserLayer (const juce::String& id) const
{
    for (auto& l : getUserLayers())
        if (l.id == id)
            return &l;

    return nullptr;
}

juce::String LayerSet::addUserLayer (const juce::String& name, const std::vector<state::FaderRef>& faders)
{
    const Utf8 u (name);
    std::vector<ld::FaderRefC> refs;
    for (auto& f : faders)
        refs.push_back (f.toC());
    return takeString (ld::ld_layers_add_user (handle, u.data, u.size, refs.data(), (int32_t) refs.size()));
}

void LayerSet::removeUserLayer (const juce::String& id)
{
    const Utf8 u (id);
    ld::ld_layers_remove_user (handle, u.data, u.size);
}

void LayerSet::renameUserLayer (const juce::String& id, const juce::String& name)
{
    const Utf8 u (id), n (name);
    ld::ld_layers_rename_user (handle, u.data, u.size, n.data, n.size);
}

void LayerSet::moveUserLayer (const juce::String& id, int delta)
{
    const Utf8 u (id);
    ld::ld_layers_move_user (handle, u.data, u.size, delta);
}

void LayerSet::addToUserLayer (const juce::String& layerId, const state::FaderRef& ref)
{
    const Utf8 u (layerId);
    ld::ld_layers_add_to_user (handle, u.data, u.size, ref.toC());
}

void LayerSet::removeFromUserLayer (const juce::String& layerId, const state::FaderRef& ref)
{
    const Utf8 u (layerId);
    ld::ld_layers_remove_from_user (handle, u.data, u.size, ref.toC());
}

void LayerSet::moveWithinUserLayer (const juce::String& layerId, int index, int delta)
{
    const Utf8 u (layerId);
    ld::ld_layers_move_within_user (handle, u.data, u.size, index, delta);
}

void LayerSet::setUserLayerFaders (const juce::String& layerId, const std::vector<state::FaderRef>& faders)
{
    const Utf8 u (layerId);
    std::vector<ld::FaderRefC> refs;
    for (auto& f : faders)
        refs.push_back (f.toC());
    ld::ld_layers_set_user_faders (handle, u.data, u.size, refs.data(), (int32_t) refs.size());
}

void LayerSet::setUserLayerSlot (const juce::String& layerId, int index, const state::FaderRef& ref)
{
    const Utf8 u (layerId);
    ld::ld_layers_set_user_slot (handle, u.data, u.size, index, ref.toC());
}

void LayerSet::clearUserLayerSlot (const juce::String& layerId, int index)
{
    const Utf8 u (layerId);
    ld::ld_layers_clear_user_slot (handle, u.data, u.size, index);
}

void LayerSet::swapUserLayerSlots (const juce::String& layerId, int a, int b)
{
    const Utf8 u (layerId);
    ld::ld_layers_swap_user_slots (handle, u.data, u.size, a, b);
}

void LayerSet::setDefaultOverride (Section s, const juce::String& id, const std::vector<state::FaderRef>& faders)
{
    const Utf8 u (id);
    std::vector<ld::FaderRefC> refs;
    for (auto& f : faders)
        refs.push_back (f.toC());
    ld::ld_layers_set_override (handle, s, u.data, u.size, refs.data(), (int32_t) refs.size());
}

void LayerSet::clearDefaultOverride (Section s, const juce::String& id)
{
    const Utf8 u (id);
    ld::ld_layers_clear_override (handle, s, u.data, u.size);
}

bool LayerSet::hasDefaultOverride (Section s, const juce::String& id) const
{
    const Utf8 u (id);
    return ld::ld_layers_has_override (handle, s, u.data, u.size);
}

} // namespace livedaw
