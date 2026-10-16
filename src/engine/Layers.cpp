#include "engine/Layers.h"

namespace livedaw
{

LayerSet::LayerSet() : handle (ld::ld_layers_create()), owned (true) {}

LayerSet::~LayerSet()
{
    if (owned)
        ld::ld_layers_destroy (handle);
}

std::vector<Layer> LayerSet::list (const MixerEngine* engine, int which, const juce::String& id) const
{
    const Utf8 u (id);
    auto* l = ld::ld_layers_list (handle, engine != nullptr ? engine->getHandle() : nullptr, which, u.data, u.size);

    std::vector<Layer> layers;
    const int n = ld::ld_layer_list_len (l);
    layers.reserve ((size_t) n);

    std::vector<ld::StripRefC> refs;
    for (int i = 0; i < n; ++i)
    {
        Layer layer;
        layer.id = takeString (ld::ld_layer_list_id (l, i));
        layer.name = takeString (ld::ld_layer_list_name (l, i));
        layer.user = ld::ld_layer_list_is_user (l, i);

        refs.resize ((size_t) ld::ld_layer_list_strips (l, i, nullptr, 0));
        ld::ld_layer_list_strips (l, i, refs.data(), (int32_t) refs.size());
        for (auto r : refs)
            layer.strips.push_back (state::StripRef::fromC (r));

        layers.push_back (std::move (layer));
    }

    ld::ld_layer_list_free (l);
    return layers;
}

juce::String LayerSet::findDefaultLayerContaining (const MixerEngine& engine, const state::StripRef& ref) const
{
    return takeString (ld::ld_layers_find_default_containing (handle, engine.getHandle(), ref.toC()));
}

std::vector<Layer> LayerSet::getDefaultLayers (const MixerEngine& engine) const { return list (&engine, 0); }
std::vector<Layer> LayerSet::getAllLayers (const MixerEngine& engine) const     { return list (&engine, 1); }

Layer LayerSet::findLayer (const MixerEngine& engine, const juce::String& id) const
{
    auto layers = list (&engine, 3, id);
    return layers.empty() ? Layer {} : std::move (layers.front());
}

std::vector<Strip*> LayerSet::resolve (MixerEngine& engine, const Layer& layer)
{
    std::vector<Strip*> strips;

    for (auto& ref : layer.strips)
        if (auto* s = state::findStrip (engine, ref))
            strips.push_back (s);

    return strips;
}

//==============================================================================
const std::vector<Layer>& LayerSet::getUserLayers() const
{
    userSnapshot = list (nullptr, 2);
    return userSnapshot;
}

const Layer* LayerSet::getUserLayer (const juce::String& id) const
{
    for (auto& l : getUserLayers())
        if (l.id == id)
            return &l;

    return nullptr;
}

juce::String LayerSet::addUserLayer (const juce::String& name, const std::vector<state::StripRef>& strips)
{
    const Utf8 u (name);
    const auto refs = state::toC (strips);
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

void LayerSet::addToUserLayer (const juce::String& layerId, const state::StripRef& ref)
{
    const Utf8 u (layerId);
    ld::ld_layers_add_to_user (handle, u.data, u.size, ref.toC());
}

void LayerSet::removeFromUserLayer (const juce::String& layerId, const state::StripRef& ref)
{
    const Utf8 u (layerId);
    ld::ld_layers_remove_from_user (handle, u.data, u.size, ref.toC());
}

void LayerSet::moveWithinUserLayer (const juce::String& layerId, int index, int delta)
{
    const Utf8 u (layerId);
    ld::ld_layers_move_within_user (handle, u.data, u.size, index, delta);
}

void LayerSet::setSelectedLayerId (const juce::String& id)
{
    const Utf8 u (id);
    ld::ld_layers_set_selected (handle, u.data, u.size);
}

} // namespace livedaw
