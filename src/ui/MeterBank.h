#pragma once

#include "engine/MixerEngine.h"
#include "ui/MeterBallistics.h"

#include <juce_events/juce_events.h>

#include <array>
#include <unordered_map>
#include <vector>

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
    "max since last read", so only one reader may take them) and moves a meter
    per signal on with the shared ballistics (MeterBallistics.h: the full peak
    at once, a 24 dB/s fall, a 1 s peak hold), so every view of a signal reads
    the same: the raw input, after trim (the INPUT screen's meter), pre-fader
    (the METERS screen, "PRE" meter settings) and the output ("POST"), the
    monitor paths and the ROUTING sources. Message thread only. */
class MeterBank final : private juce::Timer
{
public:
    /** How often the meters move on (the views read them at their own rate). */
    static constexpr int kRateHz = 40;

    explicit MeterBank (MixerEngine& e) : engine (e) { startTimerHz (kRateHz); }

    /** Linear level of a strip at a meter point (channel 0/1). */
    float get (const Strip* s, MeterPoint point, int channel) const noexcept
    {
        const auto* m = find (s, point, channel);
        return m != nullptr ? dsp::dbToGain (m->levelDb) : 0.0f;
    }

    /** Linear level of a strip's input (raw, before trim) or output (channel 0/1). */
    float get (const Strip* s, bool input, int channel) const noexcept
    {
        return get (s, input ? MeterPoint::Input : MeterPoint::Output, channel);
    }

    /** Linear peak-hold level of a strip at a meter point (channel 0/1). */
    float getHold (const Strip* s, MeterPoint point, int channel) const noexcept
    {
        const auto* m = find (s, point, channel);
        return m != nullptr ? dsp::dbToGain (m->holdDb) : 0.0f;
    }

    /** The SPEAKERS monitor path's output (channel 0/1). */
    float getMonitor (int channel) const noexcept { return dsp::dbToGain (monitor[side (channel)].levelDb); }
    /** The PHONES monitor path's output (channel 0/1). */
    float getPhones (int channel) const noexcept { return dsp::dbToGain (phones[side (channel)].levelDb); }

    /** A ROUTING > SOURCES source's level (MixerEngine::getSource index), linear. */
    float getSource (int index) const noexcept
    {
        return juce::isPositiveAndBelow (index, (int) sources.size()) ? dsp::dbToGain (sources[(size_t) index].levelDb) : 0.0f;
    }

    /** Takes the engine's peaks and moves every meter on by `seconds` (the
        timer does this with the time that passed; tests drive it). */
    void advance (float seconds)
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
                        l[index (point, ch)].advance (s->peakMeter (point, ch).exchange (0.0f), seconds);
            }
        }

        for (size_t ch = 0; ch < 2; ++ch)
        {
            monitor[ch].advance (engine.monitorPeak[ch].exchange (0.0f), seconds);
            phones[ch].advance (engine.phones.peak[ch].exchange (0.0f), seconds);
        }

        sources.resize ((size_t) juce::jmax (0, engine.getNumSources()));
        for (size_t i = 0; i < sources.size(); ++i)
            if (auto* src = engine.getSource ((int) i))
                sources[i].advance (src->peak.exchange (0.0f), seconds);
    }

private:
    static constexpr MeterPoint points[] = { MeterPoint::Input, MeterPoint::Trim, MeterPoint::PreFader, MeterPoint::Output };
    static constexpr size_t numLevels = std::size (points) * 2;

    static size_t index (MeterPoint point, int channel) noexcept
    {
        return (size_t) point * 2 + (size_t) juce::jlimit (0, 1, channel);
    }
    static size_t side (int channel) noexcept { return (size_t) juce::jlimit (0, 1, channel); }

    const meters::Ballistics* find (const Strip* s, MeterPoint point, int channel) const noexcept
    {
        if (s == nullptr)
            return nullptr;
        const auto it = levels.find (s->getHandle());
        return it == levels.end() ? nullptr : &it->second[index (point, channel)];
    }

    void timerCallback() override
    {
        // The time that really passed: a busy message thread fires late, the meters still fall at their rate.
        const double now = juce::Time::getMillisecondCounterHiRes();
        const auto seconds = lastTick > 0.0 ? (float) juce::jlimit (0.0, 0.5, (now - lastTick) * 0.001) : 1.0f / (float) kRateHz;
        lastTick = now;
        advance (seconds);
    }

    MixerEngine& engine;
    std::unordered_map<ld::Strip*, std::array<meters::Ballistics, numLevels>> levels;
    std::array<meters::Ballistics, 2> monitor {}, phones {};
    std::vector<meters::Ballistics> sources;
    int version = -1;
    double lastTick = 0.0;
};

} // namespace livedaw::ui
