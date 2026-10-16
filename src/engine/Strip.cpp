#include "engine/Strip.h"

namespace livedaw
{

const char* toString (StripKind kind) noexcept
{
    switch (kind)
    {
        case StripKind::Input:  return "input";
        case StripKind::Aux:    return "aux";
        case StripKind::Group:  return "group";
        case StripKind::Master: return "master";
    }
    return "input";
}

StripKind stripKindFromString (const juce::String& s) noexcept
{
    if (s == "aux")    return StripKind::Aux;
    if (s == "group")  return StripKind::Group;
    if (s == "master") return StripKind::Master;
    return StripKind::Input;
}

Strip::Strip (ld::Engine* engine, ld::Strip* h)
    : params (*ld::ld_strip_params (h)),
      sends (h),
      inserts (engine, h),
      inputPeak (ld::ld_strip_meters (h)->inputPeak),
      outputPeak (ld::ld_strip_meters (h)->outputPeak),
      gateGain (ld::ld_strip_meters (h)->gateGain),
      compReductionDb (ld::ld_strip_meters (h)->compReductionDb),
      currentWidth (ld::ld_strip_meters (h)->currentWidth),
      dspFaults (ld::ld_strip_meters (h)->dspFaults),
      handle (h),
      kind (static_cast<StripKind> (ld::ld_strip_kind (h))),
      id (ld::ld_strip_id (h)),
      meters (*ld::ld_strip_meters (h))
{
}

} // namespace livedaw
