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
        case Preamp:      return "Preamp";
        case Gate:        return "Gate";
        case Eq:          return "EQ";
        case Comp:        return "Comp";
        case Inserts:     return "Inserts";
        case Sends:       return "Sends";
        case Fader:       return "Fader";
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

Strip* findStrip (MixerEngine& engine, const StripRef& ref) noexcept
{
    switch (ref.kind)
    {
        case StripKind::Master:
            return &engine.getMaster();

        case StripKind::Aux:
        case StripKind::Group:
        {
            auto* bus = engine.getBusInSlot (ref.id);
            return bus != nullptr && bus->getKind() == ref.kind ? bus : nullptr;
        }

        case StripKind::Input:
            for (auto& s : engine.getInputs())
                if (s->getId() == ref.id)
                    return s.get();
            return nullptr;
    }

    return nullptr;
}

//==============================================================================
StripRef StripRef::fromC (ld::StripRefC r) noexcept
{
    const auto kind = juce::isPositiveAndBelow (r.kind, 4) ? static_cast<StripKind> (r.kind) : StripKind::Input;
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
