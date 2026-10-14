#pragma once

#include "dsp/Biquad.h"
#include "dsp/Compressor.h"
#include "dsp/NoiseGate.h"
#include "dsp/ParametricEq.h"
#include "engine/Parameters.h"
#include "engine/PluginChain.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>

namespace livedaw
{

enum class StripKind
{
    Input,  // fed by hardware inputs
    Aux,    // fed by channel sends, usually sent to a hardware output (monitor mix, FX return)
    Group,  // fed by channel sends, usually routed to the master (subgroup)
    Master  // main L/R
};

const char* toString (StripKind kind) noexcept;
StripKind stripKindFromString (const juce::String& s) noexcept;

/** One console strip: trim -> polarity -> HPF -> gate -> 4-band EQ -> compressor
    -> plugin inserts -> mute -> fader -> pan.

    The same class is used for input channels, aux/group buses and the master;
    the engine decides what feeds it and where its output goes.

    Thread model: params are atomics written from the UI. Everything else in the
    "audio thread" section is touched by exactly one audio task at a time.
*/
class Strip
{
public:
    Strip (StripKind kind, int id, RtGarbage& garbage);

    StripKind getKind() const noexcept { return kind; }
    bool isBus() const noexcept { return kind == StripKind::Aux || kind == StripKind::Group; }

    /** Inputs: a stable id. Buses: the bus slot (0..kMaxBuses-1) that sends refer to. */
    int getId() const noexcept { return id; }

    // Message thread ------------------------------------------------------------
    juce::String name;
    juce::Colour colour;

    StripParams params;
    PluginChain inserts;

    void prepare (double sampleRate, int maxBlockSize);

    // Peak meters: written by the audio thread as "max since last read"; one UI
    // reader each (exchange with 0). Dynamics meters hold the latest block's value.
    std::array<std::atomic<float>, 2> inputPeak {}, outputPeak {};
    std::atomic<float> gateGain { 1.0f }, compReductionDb { 0.0f };
    std::atomic<int> currentWidth { 1 };

    // Audio thread --------------------------------------------------------------
    /** Number of channels this block (1 = mono, 2 = stereo). */
    int getNumChannels() const noexcept { return numChannels; }

    /** Inputs: pull from the hardware inputs (indexed by hardware channel; nullptr = inactive). */
    void readHardwareInputs (const float* const* hwInputs, int numHwInputs, int numSamples) noexcept;

    /** Buses/master: clear the work buffer before sums are accumulated into it. */
    void beginSummedBlock (int numSamples) noexcept;
    float* const* getWorkBuffers() noexcept { return work.getArrayOfWritePointers(); }

    /** Runs the processing chain and output stage on whatever is in the work buffer. */
    void process (int numSamples) noexcept;

    // Signal taps, valid after process():
    const float* const* getPostChain() const noexcept { return work.getArrayOfReadPointers(); }   // PFL / direct out
    const float* const* getPreFader() const noexcept { return pre.getArrayOfReadPointers(); }     // post-mute
    const float* const* getPostFader() const noexcept { return post.getArrayOfReadPointers(); }   // pre-pan
    const float* const* getMainOut() const noexcept { return main.getArrayOfReadPointers(); }     // stereo, panned

    float getPanLeft() const noexcept { return panLeft; }
    float getPanRight() const noexcept { return panRight; }

    /** Per-destination gain ramp state, owned by whichever task sums this strip. */
    std::array<float, kMaxBuses> lastSendGain {};
    float lastMainAssignGain = 0.0f;
    float lastSoloGain = 0.0f;

private:
    void runChain (int numSamples) noexcept;
    void runOutputStage (int numSamples) noexcept;

    const StripKind kind;
    const int id;

    double sampleRate = 48000.0;
    int maxBlock = 512;
    int numChannels = 1;

    juce::AudioBuffer<float> work, pre, post, main;

    dsp::Biquad hpf;
    float lastHpfFreq = -1.0f;
    bool hpfWasOn = false;
    dsp::NoiseGate gate;
    dsp::ParametricEq eq;
    dsp::Compressor comp;

    float lastTrim = 1.0f, lastMute = 1.0f, lastFader = 1.0f;
    float panLeft = 1.0f, panRight = 1.0f, lastPanLeft = 1.0f, lastPanRight = 1.0f;

    JUCE_DECLARE_NON_COPYABLE (Strip)
};

} // namespace livedaw
