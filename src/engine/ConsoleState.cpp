#include "engine/ConsoleState.h"

namespace livedaw::state
{

//==============================================================================
Section sectionAt (int index) noexcept
{
    return static_cast<Section> (1u << juce::jlimit (0, kNumSections - 1, index));
}

const char* sectionName (Section s) noexcept
{
    switch (s)
    {
        case Name:        return "Name";
        case Routing:     return "Routing";
        case Preamp:      return "Input";
        case Gate:        return "Gate";
        case Eq:          return "EQ";
        case Dynamics:    return "Dynamics";
        case Inserts:     return "Inserts";
        case Sends:       return "Sends";
        case Fader:       return "Fader";
        case Groups:      return "DCA/Mute";
        case AllSections: return "All";
    }
    return "";
}

juce::String StripRef::toString() const
{
    return juce::String (livedaw::toString (kind)) + ":" + juce::String (id);
}

StripRef StripRef::fromString (const juce::String& s)
{
    return { stripKindFromString (s.upToFirstOccurrenceOf (":", false, false)),
             s.fromFirstOccurrenceOf (":", false, false).getIntValue() };
}

StripRef StripRef::of (const Strip& s) noexcept
{
    return { s.getKind(), s.getId() };
}

Strip* findStrip (const MixerEngine& engine, const StripRef& ref) noexcept
{
    switch (ref.kind)
    {
        case StripKind::Channel:
            return engine.findChannel (ref.id);

        case StripKind::Bus:
            return engine.getBusInSlot (ref.id);

        case StripKind::Aux:
        case StripKind::Main:
        case StripKind::Matrix:
        {
            const auto& list = engine.getStrips (ref.kind);
            return juce::isPositiveAndBelow (ref.id, (int) list.size()) ? list[(size_t) ref.id].get() : nullptr;
        }
    }

    return nullptr;
}

StripRef StripRef::fromC (ld::StripRefC r) noexcept
{
    const auto kind = juce::isPositiveAndBelow (r.kind, kNumStripKinds) ? static_cast<StripKind> (r.kind) : StripKind::Channel;
    return { kind, r.id };
}

std::vector<ld::StripRefC> toC (const std::vector<StripRef>& refs)
{
    std::vector<ld::StripRefC> out;
    out.reserve (refs.size());
    for (auto& r : refs)
        out.push_back (r.toC());
    return out;
}

ld::FaderRefC FaderRef::toC() const noexcept
{
    return dca ? ld::FaderRefC { ld::kFaderDca, dcaIndex } : ld::FaderRefC { static_cast<int32_t> (strip.kind), strip.id };
}

FaderRef FaderRef::fromC (ld::FaderRefC r) noexcept
{
    if (r.kind == ld::kFaderDca)
        return ofDca (r.id);
    return of (StripRef::fromC ({ r.kind, r.id }));
}

ApplyResult ApplyResult::fromC (ld::ApplyResultC& r)
{
    ApplyResult result;
    result.stripsApplied = r.stripsApplied;
    result.stripsSkippedSafe = r.stripsSkippedSafe;
    result.stripsMissing = r.stripsMissing;
    result.warnings = juce::StringArray::fromLines (takeString (r.warnings));
    result.warnings.removeEmptyStrings();
    r.warnings = nullptr;
    return result;
}

} // namespace livedaw::state
