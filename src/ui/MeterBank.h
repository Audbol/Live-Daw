#pragma once

#include "engine/MixerEngine.h"

#include <juce_events/juce_events.h>

#include <array>
#include <unordered_map>

namespace livedaw::ui
{

/** Reads every strip's peak meters once per frame (the engine's peaks are
    "max since last read", so only one reader may take them) and holds a
    short-decay level that any number of views can show. Message thread only. */
class MeterBank final : private juce::Timer
{
public:
    explicit MeterBank (MixerEngine& e) : engine (e) { startTimerHz (40); }

    /** Linear peak of a strip's input or output (channel 0/1). */
    float get (const Strip* s, bool input, int channel) const noexcept
    {
        if (s == nullptr)
            return 0.0f;
        const auto it = levels.find (s->getHandle());
        return it == levels.end() ? 0.0f : it->second[(size_t) ((input ? 0 : 2) + juce::jlimit (0, 1, channel))];
    }

    float getMonitor (int channel) const noexcept { return monitor[(size_t) juce::jlimit (0, 1, channel)]; }

private:
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
                auto& m = s->meters;
                const float peaks[4] = { m.inputPeak[0].exchange (0.0f), m.inputPeak[1].exchange (0.0f),
                                         m.outputPeak[0].exchange (0.0f), m.outputPeak[1].exchange (0.0f) };
                for (size_t i = 0; i < 4; ++i)
                    l[i] = juce::jmax (peaks[i], l[i] * 0.6f);
            }
        }

        for (size_t ch = 0; ch < 2; ++ch)
            monitor[ch] = juce::jmax (engine.monitorPeak[ch].exchange (0.0f), monitor[ch] * 0.6f);
    }

    MixerEngine& engine;
    std::unordered_map<ld::Strip*, std::array<float, 4>> levels;
    std::array<float, 2> monitor {};
    int version = -1;
};

} // namespace livedaw::ui
