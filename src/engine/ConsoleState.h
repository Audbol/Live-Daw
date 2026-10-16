#pragma once

#include "engine/MixerEngine.h"

#include <juce_core/juce_core.h>

#include <vector>

namespace livedaw::state
{

/** Parameter groups of a strip. Scenes and snippets recall a chosen set of these
    (their "scope"). */
enum Section : juce::uint32
{
    Name     = 1u << 0, // name, colour, icon, scribble light
    Routing  = 1u << 1, // input patch, mono/stereo, outputs, monitor bus (+ monitor setup)
    Preamp   = 1u << 2, // INPUT block (trim, balance, polarity, delay) and FILTER
    Gate     = 1u << 3,
    Eq       = 1u << 4,
    Dynamics = 1u << 5,
    Inserts  = 1u << 6, // insert points, process order, tap (+ FX slot states; plugins are only created by show loads)
    Sends    = 1u << 7, // bus, main and matrix sends, matrix direct input
    Fader    = 1u << 8, // fader, pan, width, mute (+ DCA levels)
    Groups   = 1u << 9, // DCA and mute group assignments (+ mute group states)

    AllSections = 0x3ffu
};

constexpr int kNumSections = 10;
Section sectionAt (int index) noexcept;
const char* sectionName (Section) noexcept;

/** Stable reference to a strip that survives save/load. */
struct StripRef
{
    StripKind kind = StripKind::Channel;
    int id = 0;

    bool operator== (const StripRef& o) const noexcept { return kind == o.kind && id == o.id; }
    juce::String toString() const;
    static StripRef fromString (const juce::String&);
    static StripRef of (const Strip&) noexcept;

    ld::StripRefC toC() const noexcept { return { static_cast<int32_t> (kind), id }; }
    static StripRef fromC (ld::StripRefC r) noexcept;
};

std::vector<ld::StripRefC> toC (const std::vector<StripRef>&);

Strip* findStrip (const MixerEngine&, const StripRef&) noexcept;

/** A fader on the surface: a strip or a DCA. */
struct FaderRef
{
    bool dca = false;
    StripRef strip;
    int dcaIndex = 0;

    static FaderRef of (const StripRef& r) noexcept { return { false, r, 0 }; }
    static FaderRef ofDca (int index) noexcept { return { true, {}, index }; }

    bool operator== (const FaderRef& o) const noexcept
    {
        return dca == o.dca && (dca ? dcaIndex == o.dcaIndex : strip == o.strip);
    }

    ld::FaderRefC toC() const noexcept;
    static FaderRef fromC (ld::FaderRefC) noexcept;
};

struct ApplyResult
{
    int stripsApplied = 0;
    int stripsSkippedSafe = 0;
    int stripsMissing = 0;
    juce::StringArray warnings;

    /** Converts (and frees) a result from the Rust session layer. */
    static ApplyResult fromC (ld::ApplyResultC&);
};

// Capturing and applying console state (per section), saving and loading the
// full console, and show files are implemented in Rust (crate livedaw-session);
// see Show for the C++ interface.

} // namespace livedaw::state
