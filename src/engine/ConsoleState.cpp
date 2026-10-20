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
    if (dca)
        return { ld::kFaderDca, dcaIndex, -1, 0 };
    if (fxParam)
        return { ld::kFaderFxParam, fxSlot, -1, param };
    if (midiCc)
        return { ld::kFaderMidiCc, midiChannel, -1, cc };
    if (send)
        return { static_cast<int32_t> (strip.kind), strip.id, static_cast<int32_t> (to.kind), to.id };
    return { static_cast<int32_t> (strip.kind), strip.id, -1, 0 };
}

FaderRef FaderRef::fromC (ld::FaderRefC r) noexcept
{
    if (r.kind == ld::kFaderDca)
        return ofDca (r.id);
    if (r.kind == ld::kFaderFxParam)
        return ofFxParam (r.id, r.toId);
    if (r.kind == ld::kFaderMidiCc)
        return ofMidiCc (r.id, r.toId);
    const auto from = StripRef::fromC ({ r.kind, r.id });
    if (r.toKind == ld::kBus || r.toKind == ld::kMain || r.toKind == ld::kMatrix)
        return ofSend (from, StripRef::fromC ({ r.toKind, r.toId }));
    return of (from);
}

Scope Scope::only (juce::uint32 sections, std::vector<StripRef> strips)
{
    Scope s;
    s.sections = sections;
    s.allStrips = false;
    s.strips = std::move (strips);
    s.dcas = s.muteGroups = s.fx = 0;
    s.console = false;
    return s;
}

bool Scope::includes (const StripRef& r) const
{
    return allStrips || std::find (strips.begin(), strips.end(), r) != strips.end();
}

bool Scope::operator== (const Scope& o) const
{
    return sections == o.sections && allStrips == o.allStrips && (allStrips || strips == o.strips) && dcas == o.dcas
           && muteGroups == o.muteGroups && fx == o.fx && console == o.console;
}

ld::ScopeC Scope::toC (const std::vector<ld::StripRefC>& cStrips) const noexcept
{
    return { sections, dcas, muteGroups, fx, console, allStrips, cStrips.data(), (int32_t) cStrips.size() };
}

Scope Scope::fromC (const ld::ScopeC& c, const std::vector<ld::StripRefC>& cStrips)
{
    Scope s;
    s.sections = c.sections;
    s.dcas = c.dcas;
    s.muteGroups = c.muteGroups;
    s.fx = c.fx;
    s.console = c.console;
    s.allStrips = c.allStrips;
    for (auto r : cStrips)
        s.strips.push_back (StripRef::fromC (r));
    return s;
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
