#include "engine/Layers.h"

namespace livedaw
{

namespace
{
    void addBanks (std::vector<Layer>& layers, const std::vector<state::StripRef>& refs, int bankSize,
                   const juce::String& idPrefix, const juce::String& namePrefix)
    {
        const int count = static_cast<int> (refs.size());

        for (int start = 0, bank = 1; start < count; start += bankSize, ++bank)
        {
            const int end = juce::jmin (count, start + bankSize);
            Layer layer;
            layer.id = idPrefix + "-" + juce::String (bank);
            layer.name = count <= bankSize ? namePrefix
                                           : namePrefix + " " + juce::String (start + 1) + "-" + juce::String (end);
            layer.strips.assign (refs.begin() + start, refs.begin() + end);
            layers.push_back (std::move (layer));
        }
    }
} // namespace

std::vector<Layer> LayerSet::getDefaultLayers (const MixerEngine& engine) const
{
    std::vector<state::StripRef> inputs, auxes, groups, all;

    for (auto& s : engine.getInputs())
        inputs.push_back (state::StripRef::of (*s));

    for (auto& b : engine.getBuses())
        (b->getKind() == StripKind::Aux ? auxes : groups).push_back (state::StripRef::of (*b));

    std::vector<Layer> layers;

    if (inputs.empty())
        layers.push_back ({ "in-1", "Inputs", {}, false });
    else
        addBanks (layers, inputs, bankSize, "in", "Ch");

    addBanks (layers, auxes, bankSize, "aux", "Aux");
    addBanks (layers, groups, bankSize, "groups", "Groups");

    all = inputs;
    all.insert (all.end(), auxes.begin(), auxes.end());
    all.insert (all.end(), groups.begin(), groups.end());
    layers.push_back ({ "all", "All", std::move (all), false });

    return layers;
}

std::vector<Layer> LayerSet::getAllLayers (const MixerEngine& engine) const
{
    auto layers = getDefaultLayers (engine);
    layers.insert (layers.end(), userLayers.begin(), userLayers.end());
    return layers;
}

Layer LayerSet::findLayer (const MixerEngine& engine, const juce::String& id) const
{
    auto layers = getAllLayers (engine);

    for (auto& l : layers)
        if (l.id == id)
            return l;

    return layers.front();
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
Layer* LayerSet::getUserLayer (const juce::String& id) noexcept
{
    for (auto& l : userLayers)
        if (l.id == id)
            return &l;

    return nullptr;
}

juce::String LayerSet::addUserLayer (const juce::String& name, std::vector<state::StripRef> strips)
{
    Layer layer;
    layer.id = "user-" + juce::Uuid().toString();
    layer.name = name;
    layer.strips = std::move (strips);
    layer.user = true;
    userLayers.push_back (std::move (layer));
    return userLayers.back().id;
}

void LayerSet::removeUserLayer (const juce::String& id)
{
    userLayers.erase (std::remove_if (userLayers.begin(), userLayers.end(), [&] (const Layer& l) { return l.id == id; }),
                      userLayers.end());
}

void LayerSet::moveUserLayer (const juce::String& id, int delta)
{
    for (size_t i = 0; i < userLayers.size(); ++i)
    {
        if (userLayers[i].id != id)
            continue;

        const auto target = static_cast<int> (i) + delta;
        if (juce::isPositiveAndBelow (target, static_cast<int> (userLayers.size())))
            std::swap (userLayers[i], userLayers[(size_t) target]);
        return;
    }
}

void LayerSet::addToUserLayer (const juce::String& layerId, const state::StripRef& ref)
{
    if (auto* l = getUserLayer (layerId))
        if (std::find (l->strips.begin(), l->strips.end(), ref) == l->strips.end())
            l->strips.push_back (ref);
}

void LayerSet::removeFromUserLayer (const juce::String& layerId, const state::StripRef& ref)
{
    if (auto* l = getUserLayer (layerId))
        l->strips.erase (std::remove (l->strips.begin(), l->strips.end(), ref), l->strips.end());
}

void LayerSet::moveWithinUserLayer (const juce::String& layerId, int index, int delta)
{
    if (auto* l = getUserLayer (layerId))
    {
        const int target = index + delta;
        const int size = static_cast<int> (l->strips.size());
        if (juce::isPositiveAndBelow (index, size) && juce::isPositiveAndBelow (target, size))
            std::swap (l->strips[(size_t) index], l->strips[(size_t) target]);
    }
}

//==============================================================================
std::unique_ptr<juce::XmlElement> LayerSet::toXml() const
{
    auto xml = std::make_unique<juce::XmlElement> ("Layers");
    xml->setAttribute ("bankSize", bankSize);
    xml->setAttribute ("selected", selectedLayerId);

    for (auto& l : userLayers)
    {
        auto* e = xml->createNewChildElement ("Layer");
        e->setAttribute ("id", l.id);
        e->setAttribute ("name", l.name);

        juce::StringArray refs;
        for (auto& r : l.strips)
            refs.add (r.toString());
        e->setAttribute ("strips", refs.joinIntoString (","));
    }

    return xml;
}

void LayerSet::fromXml (const juce::XmlElement& xml)
{
    reset();
    setBankSize (xml.getIntAttribute ("bankSize", kDefaultBankSize));
    selectedLayerId = xml.getStringAttribute ("selected", selectedLayerId);

    for (auto* e : xml.getChildWithTagNameIterator ("Layer"))
    {
        Layer layer;
        layer.id = e->getStringAttribute ("id");
        if (! layer.id.startsWith ("user-"))
            layer.id = "user-" + juce::Uuid().toString();
        layer.name = e->getStringAttribute ("name", "User");
        layer.user = true;

        for (auto& part : juce::StringArray::fromTokens (e->getStringAttribute ("strips"), ",", ""))
            if (part.isNotEmpty())
                layer.strips.push_back (state::StripRef::fromString (part));

        userLayers.push_back (std::move (layer));
    }
}

void LayerSet::reset()
{
    userLayers.clear();
    bankSize = kDefaultBankSize;
    selectedLayerId = "in-1";
}

} // namespace livedaw
