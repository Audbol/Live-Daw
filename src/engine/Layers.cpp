#include "engine/Layers.h"

namespace livedaw
{

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

} // namespace livedaw
