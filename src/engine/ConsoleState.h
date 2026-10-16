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
    Name    = 1u << 0, // name, colour
    Routing = 1u << 1, // mono/stereo, input patch, outputs, main assign (+ monitor setup on the master)
    Preamp  = 1u << 2, // trim, polarity, HPF
    Gate    = 1u << 3,
    Eq      = 1u << 4,
    Comp    = 1u << 5,
    Inserts = 1u << 6, // plugin state + bypass (plugins are only instantiated when loading a show)
    Sends   = 1u << 7,
    Fader   = 1u << 8, // fader, pan, mute

    AllSections = 0x1ffu
};

constexpr int kNumSections = 9;
Section sectionAt (int index) noexcept;
const char* sectionName (Section) noexcept;

/** Stable reference to a strip that survives save/load. */
struct StripRef
{
    StripKind kind = StripKind::Input;
    int id = 0;

    bool operator== (const StripRef& o) const noexcept { return kind == o.kind && id == o.id; }
    juce::String toString() const;
    static StripRef fromString (const juce::String&);
    static StripRef of (const Strip&) noexcept;

    ld::StripRefC toC() const noexcept { return { static_cast<int32_t> (kind), id }; }
    static StripRef fromC (ld::StripRefC r) noexcept;
};

std::vector<ld::StripRefC> toC (const std::vector<StripRef>&);

Strip* findStrip (MixerEngine&, const StripRef&) noexcept;

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
