#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace livedaw
{

/** A plugin whose main input or output is mono (one that has no stereo layout,
    such as an LSP "Mono" plugin) in Sidecar's stereo slots. Its buffer's channel
    1 isn't its right side: it's a side-chain input or not used at all, so
    left as it was the right side would feed a side chain or come back
    silent. Before processing, a mono input gets both sides (half each) and
    channel 1 is cleared; afterwards a mono output goes to both sides.

    Shared by FxSlots, PluginChain and the sandbox helper. Audio thread: no
    allocation. */
struct MonoBuses
{
    bool input = false, output = false;

    /** Message thread, once the plugin's layout is set (it's prepared). */
    static MonoBuses of (const juce::AudioProcessor& p)
    {
        MonoBuses m;
        m.input = p.getBusCount (true) > 0 && p.getChannelCountOfBus (true, 0) == 1;
        m.output = p.getBusCount (false) > 0 && p.getChannelCountOfBus (false, 0) == 1;
        return m;
    }

    /** `buffer` holds the left and right sides in channels 0 and 1. */
    void beforeProcessing (juce::AudioBuffer<float>& buffer) const noexcept
    {
        if (! input || buffer.getNumChannels() < 2)
            return;
        const int n = buffer.getNumSamples();
        buffer.addFrom (0, 0, buffer, 1, 0, n);
        buffer.applyGain (0, 0, n, 0.5f);
        buffer.clear (1, 0, n);
    }

    void afterProcessing (juce::AudioBuffer<float>& buffer) const noexcept
    {
        if (output && buffer.getNumChannels() >= 2)
            buffer.copyFrom (1, 0, buffer, 0, 0, buffer.getNumSamples());
    }
};

} // namespace livedaw
