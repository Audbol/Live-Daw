#pragma once

#include "engine/MixerEngine.h"

#include <juce_events/juce_events.h>

#include <array>
#include <unordered_map>

namespace livedaw::ui
{

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

    float getMonitor (int channel) const noexcept { return monitor[(size_t) juce::jlimit (0, 1, channel)]; }

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
            monitor[ch] = juce::jmax (engine.monitorPeak[ch].exchange (0.0f), monitor[ch] * 0.6f);
    }

    MixerEngine& engine;
    std::unordered_map<ld::Strip*, std::array<float, numLevels>> levels;
    std::array<float, 2> monitor {};
    int version = -1;
};

} // namespace livedaw::ui
