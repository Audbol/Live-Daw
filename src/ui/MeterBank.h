#pragma once

#include "engine/MixerEngine.h"

#include <juce_events/juce_events.h>

#include <array>
#include <unordered_map>

namespace livedaw::ui
{

/** MAIN METER SOURCE (manual 5.5 p. 47: "all the Main buses, Matrixes,
    Phones, Speakers or the selected channel"; WING `/cfg/mtr/mainmtr` =
    MAIN.1-4, MTX.1-8, MON.PH, MON.SPK, SEL_CH). The setting (ui.mainMeter)
    keeps its older values: 0 the speakers (once MONITOR), 1-4 MAIN n, then
    5-12 MTX 1-8, 13 the phones, 14 the selected channel. */
namespace mainmeter
{
    constexpr int kSpeakers = 0, kFirstMain = 1, kFirstMatrix = kFirstMain + kNumMains, kPhones = kFirstMatrix + kNumMatrices,
                  kSelectedChannel = kPhones + 1, kNumSources = kSelectedChannel + 1;

    /** The setting at position `i` of the WING's list. */
    constexpr int settingAt (int i) noexcept
    {
        return i < kNumMains + kNumMatrices ? i + 1 : i == kNumMains + kNumMatrices ? kPhones : i == kNumMains + kNumMatrices + 1 ? kSpeakers : kSelectedChannel;
    }
    /** The position of setting `s` in the WING's list. */
    constexpr int listIndex (int s) noexcept
    {
        for (int i = 0; i < kNumSources; ++i)
            if (settingAt (i) == s)
                return i;
        return 0;
    }
    /** The WING's list, by Sidecar's names. */
    inline juce::StringArray names()
    {
        juce::StringArray out;
        for (int i = 0; i < kNumSources; ++i)
        {
            const int s = settingAt (i);
            out.add (s == kSpeakers ? juce::String ("SPEAKERS")
                     : s == kPhones ? juce::String ("PHONES")
                     : s == kSelectedChannel ? juce::String ("SEL CH")
                     : s < kFirstMatrix ? "MAIN " + juce::String (s - kFirstMain + 1)
                                        : "MTX " + juce::String (s - kFirstMatrix + 1));
        }
        return out;
    }
} // namespace mainmeter

/** Reads every strip's peak meters once per frame (the engine's peaks are
    "max since last read", so only one reader may take them) and holds a
    short-decay level that any number of views can show: the raw input, after
    trim (the INPUT screen's meter), pre-fader (the METERS screen, "PRE" meter
    settings) and the output ("POST"). Message thread only. */
class MeterBank final : private juce::Timer
{
public:
    explicit MeterBank (MixerEngine& e) : engine (e) { startTimerHz (40); }

    /** Linear peak of a strip at a meter point (channel 0/1). */
    float get (const Strip* s, MeterPoint point, int channel) const noexcept
    {
        if (s == nullptr)
            return 0.0f;
        const auto it = levels.find (s->getHandle());
        return it == levels.end() ? 0.0f : it->second[index (point, channel)];
    }

    /** Linear peak of a strip's input (raw, before trim) or output (channel 0/1). */
    float get (const Strip* s, bool input, int channel) const noexcept
    {
        return get (s, input ? MeterPoint::Input : MeterPoint::Output, channel);
    }

    /** The SPEAKERS monitor path's output (channel 0/1). */
    float getMonitor (int channel) const noexcept { return monitor[(size_t) juce::jlimit (0, 1, channel)]; }
    /** The PHONES monitor path's output (channel 0/1). */
    float getPhones (int channel) const noexcept { return phones[(size_t) juce::jlimit (0, 1, channel)]; }

private:
    static constexpr MeterPoint points[] = { MeterPoint::Input, MeterPoint::Trim, MeterPoint::PreFader, MeterPoint::Output };
    static constexpr size_t numLevels = std::size (points) * 2;

    static size_t index (MeterPoint point, int channel) noexcept
    {
        return (size_t) point * 2 + (size_t) juce::jlimit (0, 1, channel);
    }

    void timerCallback() override
    {
        if (engine.getStructureVersion() != version)
        {
            levels.clear();
            version = engine.getStructureVersion();
        }

        for (int k = 0; k < kNumStripKinds; ++k)
        {
            for (auto& s : engine.getStrips (static_cast<StripKind> (k)))
            {
                auto& l = levels[s->getHandle()];
                for (auto point : points)
                    for (int ch = 0; ch < 2; ++ch)
                    {
                        auto& level = l[index (point, ch)];
                        level = juce::jmax (s->peakMeter (point, ch).exchange (0.0f), level * 0.6f);
                    }
            }
        }

        for (size_t ch = 0; ch < 2; ++ch)
        {
            monitor[ch] = juce::jmax (engine.monitorPeak[ch].exchange (0.0f), monitor[ch] * 0.6f);
            phones[ch] = juce::jmax (engine.phones.peak[ch].exchange (0.0f), phones[ch] * 0.6f);
        }
    }

    MixerEngine& engine;
    std::unordered_map<ld::Strip*, std::array<float, numLevels>> levels;
    std::array<float, 2> monitor {}, phones {};
    int version = -1;
};

} // namespace livedaw::ui
