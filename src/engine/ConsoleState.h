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
    Routing  = 1u << 1, // input patch, mono/stereo, outputs, monitor bus (+ monitor setup, solo PFL/AFL per type)
    Preamp   = 1u << 2, // INPUT block (trim, balance, polarity, delay) and FILTER
    Gate     = 1u << 3,
    Eq       = 1u << 4,
    Dynamics = 1u << 5,
    Inserts  = 1u << 6, // insert points, process order, tap (+ FX slot states, automix X/Y enables; plugins are only created by show loads)
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

/** A fader on the surface: a strip, a DCA, a send level (from `strip` to the
    bus, main or matrix `to`) or an FX slot's parameter. */
struct FaderRef
{
    bool dca = false;
    StripRef strip; // the strip, or a send's source
    int dcaIndex = 0;
    bool send = false;
    StripRef to;    // a send's destination
    bool fxParam = false;
    int fxSlot = 0, param = 0;
    bool midiCc = false;
    int midiChannel = 0, cc = 0; // 0-15, 0-127

    static FaderRef of (const StripRef& r) noexcept
    {
        FaderRef f;
        f.strip = r;
        return f;
    }
    static FaderRef ofDca (int index) noexcept
    {
        FaderRef f;
        f.dca = true;
        f.dcaIndex = index;
        return f;
    }
    static FaderRef ofSend (const StripRef& from, const StripRef& dest) noexcept
    {
        FaderRef f;
        f.strip = from;
        f.send = true;
        f.to = dest;
        return f;
    }
    static FaderRef ofFxParam (int slot, int parameter) noexcept
    {
        FaderRef f;
        f.fxParam = true;
        f.fxSlot = slot;
        f.param = parameter;
        return f;
    }
    /** A MIDI CC fader (user layers): CC `controller` on MIDI channel `channel`
        (0-15), sent out of EXTERNAL MIDI CONTROL. */
    static FaderRef ofMidiCc (int channel, int controller) noexcept
    {
        FaderRef f;
        f.midiCc = true;
        f.midiChannel = juce::jlimit (0, 15, channel);
        f.cc = juce::jlimit (0, 127, controller);
        return f;
    }

    /** A strip's own fader (not a DCA, send, FX parameter or MIDI CC). */
    bool isStrip() const noexcept { return ! dca && ! send && ! fxParam && ! midiCc; }

    bool operator== (const FaderRef& o) const noexcept
    {
        if (dca != o.dca || send != o.send || fxParam != o.fxParam || midiCc != o.midiCc)
            return false;
        if (dca)
            return dcaIndex == o.dcaIndex;
        if (fxParam)
            return fxSlot == o.fxSlot && param == o.param;
        if (midiCc)
            return midiChannel == o.midiChannel && cc == o.cc;
        return strip == o.strip && (! send || to == o.to);
    }

    ld::FaderRefC toC() const noexcept;
    static FaderRef fromC (ld::FaderRefC) noexcept;
};

/** What a recall, a partial update or a snippet touches (the LIBRARY's SCOPE
    window): strip sections, strips (allStrips: every one), DCAs, mute groups
    and FX slots (bit n = item n+1, recalled with FADER, GROUPS and INSERTS)
    and the console-wide settings (monitor setup and INPUT SELECT with Routing,
    automix enables with Inserts). Also holds the GLOBAL safes beyond strips. */
struct Scope
{
    static constexpr juce::uint32 kAllDcas = 0xffffu, kAllMuteGroups = 0xffu, kAllFx = 0xffffu;

    juce::uint32 sections = AllSections;
    bool allStrips = true;
    std::vector<StripRef> strips;
    juce::uint32 dcas = kAllDcas, muteGroups = kAllMuteGroups, fx = kAllFx;
    bool console = true;

    /** The whole console within `sections`. */
    static Scope everything (juce::uint32 sections = AllSections) { Scope s; s.sections = sections; return s; }
    /** Only `sections` of `strips`: no DCAs, mute groups, FX slots or console settings. */
    static Scope only (juce::uint32 sections, std::vector<StripRef> strips);
    /** Nothing selected. */
    static Scope none() { return only (0, {}); }

    bool includes (const StripRef&) const;
    bool operator== (const Scope&) const;

    /** For the C ABI: `strips` must outlive the returned struct. */
    ld::ScopeC toC (const std::vector<ld::StripRefC>& cStrips) const noexcept;
    static Scope fromC (const ld::ScopeC&, const std::vector<ld::StripRefC>& strips);
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
