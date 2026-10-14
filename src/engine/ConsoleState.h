#pragma once

#include "engine/MixerEngine.h"
#include "engine/PluginHost.h"

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
};

Strip* findStrip (MixerEngine&, const StripRef&) noexcept;

/** Values that should glide (fader, pan, send levels) instead of jumping on recall. */
struct FadeTarget
{
    std::atomic<float>* param;
    float target;
};

struct ApplyResult
{
    int stripsApplied = 0;
    int stripsSkippedSafe = 0;
    int stripsMissing = 0;
    juce::StringArray warnings;
};

/** Captures `sections` of every strip (or only `only`) into a <State> element. */
std::unique_ptr<juce::XmlElement> capture (const MixerEngine&, juce::uint32 sections,
                                           const std::vector<StripRef>* only = nullptr);

/** Applies a <State> element. Only the intersection of `sections` and what was
    captured is touched. Plugins are never created or destroyed here: inserts are
    recalled only when the same plugin is already loaded in that slot. If `fades`
    is given, continuous values are appended to it instead of being set. */
ApplyResult apply (MixerEngine&, const juce::XmlElement& state, juce::uint32 sections,
                   const std::vector<StripRef>* only, bool respectRecallSafe,
                   std::vector<FadeTarget>* fades = nullptr);

/** Full console, including its structure and plugin instances (used by show files). */
std::unique_ptr<juce::XmlElement> saveConsole (const MixerEngine&);
juce::Result loadConsole (MixerEngine&, const juce::XmlElement& console, PluginHost&, bool loadPlugins = true);

} // namespace livedaw::state
