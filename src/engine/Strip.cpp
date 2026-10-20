#include "engine/Strip.h"

namespace livedaw
{

const char* toString (StripKind kind) noexcept
{
    switch (kind)
    {
        case StripKind::Channel: return "ch";
        case StripKind::Aux:     return "aux";
        case StripKind::Bus:     return "bus";
        case StripKind::Main:    return "main";
        case StripKind::Matrix:  return "mtx";
    }
    return "ch";
}

StripKind stripKindFromString (const juce::String& s) noexcept
{
    if (s == "aux")  return StripKind::Aux;
    if (s == "bus")  return StripKind::Bus;
    if (s == "main") return StripKind::Main;
    if (s == "mtx")  return StripKind::Matrix;
    return StripKind::Channel;
}

const char* label (StripKind kind) noexcept
{
    switch (kind)
    {
        case StripKind::Channel: return "CH";
        case StripKind::Aux:     return "AUX";
        case StripKind::Bus:     return "BUS";
        case StripKind::Main:    return "MAIN";
        case StripKind::Matrix:  return "MTX";
    }
    return "CH";
}

juce::Colour wingColour (int index) noexcept
{
    return juce::Colour (0xff000000u | ld::ld_colour_rgb (index));
}

juce::String wingColourName (int index)
{
    return takeString (ld::ld_list_item (11, index - 1));
}

Strip::Strip (ld::Engine* engine, ld::Strip* h)
    : params (*ld::ld_strip_params (h)),
      sends (h),
      meters (*ld::ld_strip_meters (h)),
      controls (ld::ld_engine_controls (engine)),
      engineHandle (engine),
      handle (h),
      kind (static_cast<StripKind> (ld::ld_strip_kind (h))),
      id (ld::ld_strip_id (h))
{
    sources = ld::ld_engine_sources (engine, &numSources);
}

int Strip::linkedSourceIndex() const noexcept
{
    if (! isInput() || ! params.custLink.load())
        return -1;
    const int port = getActiveInputLeft();
    const int index = port >= 0 ? ld::ld_source_index (port) : -1;
    return juce::isPositiveAndBelow (index, numSources) ? index : -1;
}

ld::SourceParams* Strip::linkedSource() const noexcept
{
    const int index = linkedSourceIndex();
    return index >= 0 ? sources + index : nullptr;
}

juce::String Strip::getName() const
{
    const int index = linkedSourceIndex();
    return index >= 0 ? takeString (ld::ld_source_name (engineHandle, index)) : getOwnName();
}

int Strip::getColourIndex() const noexcept
{
    const auto* src = linkedSource();
    return src != nullptr ? src->colour.load() : params.colour.load();
}

int Strip::getIcon() const noexcept
{
    const auto* src = linkedSource();
    return src != nullptr ? src->icon.load() : params.icon.load();
}

} // namespace livedaw
