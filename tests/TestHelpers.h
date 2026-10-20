#pragma once

#include "engine/MixerEngine.h"

#include <juce_core/juce_core.h>

#include <vector>

namespace livedaw::test
{
/** The fixed strips (aux, buses, mains, matrices) and the DCAs at 0 dB with
    their EQ on, buses not feeding the mains: the console the audio tests were
    written for (a factory WING has the faders down and the EQs off). */
inline void unityConsole (MixerEngine& engine)
{
    for (auto kind : { StripKind::Aux, StripKind::Bus, StripKind::Main, StripKind::Matrix })
        for (auto& s : engine.getStrips (kind))
        {
            s->params.faderDb = 0.0f;
            s->params.eq.on = true;
            if (kind == StripKind::Bus)
                s->params.mains[0].on = false;
        }
    for (int i = 0; i < kNumDcas; ++i)
        engine.getDca (i).faderDb = 0.0f;
}

/** Sets the number of channels; new channels get their fader at 0 dB (a new
    channel's fader is down, as on the WING). */
inline void setChannels (MixerEngine& engine, int count)
{
    const int before = (int) engine.getChannels().size();
    engine.setNumChannels (count);
    const auto& channels = engine.getChannels();
    for (int i = before; i < (int) channels.size(); ++i)
        channels[(size_t) i]->params.faderDb = 0.0f;
}

/** Drives a MixerEngine with constant (DC) inputs and captures its outputs. */
struct OfflineRig
{
    static constexpr int kBlock = 128;

    OfflineRig (MixerEngine& e, int numIns, int numOuts) : engine (e)
    {
        unityConsole (engine);
        engine.prepareOffline (48000.0, kBlock, numIns, numOuts);
        inputs.assign ((size_t) numIns, std::vector<float> (kBlock, 0.0f));
        outputs.assign ((size_t) numOuts, std::vector<float> (kBlock, 0.0f));
    }

    void setInput (int ch, float value) { std::fill (inputs[(size_t) ch].begin(), inputs[(size_t) ch].end(), value); }

    void run (int numBlocks = 20)
    {
        std::vector<const float*> in;
        std::vector<float*> out;
        for (auto& v : inputs)
            in.push_back (v.data());
        for (auto& v : outputs)
            out.push_back (v.data());

        for (int b = 0; b < numBlocks; ++b)
            engine.processBlock (in.data(), (int) in.size(), out.data(), (int) out.size(), kBlock);
    }

    float out (int ch) const { return outputs[(size_t) ch].back(); }

    MixerEngine& engine;
    std::vector<std::vector<float>> inputs, outputs;
};

inline constexpr float kCentre = 0.70710678f; // constant-power pan at centre
} // namespace livedaw::test

#include <juce_audio_processors/juce_audio_processors.h>

namespace livedaw::test
{
/** A minimal in-process "plugin" for tests: a gain stage that can be told to
    misbehave (emit NaN or throw). Its gain is its saved state. */
class FakePlugin final : public juce::AudioPluginInstance
{
public:
    enum class Mode
    {
        Gain,
        EmitNaN,
        Throw
    };

    explicit FakePlugin (float g = 1.0f, Mode m = Mode::Gain, juce::String id = "fake-gain")
        : juce::AudioPluginInstance (BusesProperties()
                                         .withInput ("In", juce::AudioChannelSet::stereo())
                                         .withOutput ("Out", juce::AudioChannelSet::stereo())),
          gain (g), mode (m), identifier (std::move (id))
    {
    }

    std::atomic<float> gain;
    std::atomic<Mode> mode;

    void fillInPluginDescription (juce::PluginDescription& d) const override
    {
        d.name = "Fake " + identifier;
        d.pluginFormatName = "Fake";
        d.fileOrIdentifier = identifier;
        d.uniqueId = d.deprecatedUid = identifier.hashCode();
        d.manufacturerName = "Tests";
        d.numInputChannels = d.numOutputChannels = 2;
    }

    const juce::String getName() const override { return "Fake " + identifier; }
    void prepareToPlay (double, int) override {}
    void releaseResources() override {}

    using juce::AudioPluginInstance::processBlock;

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        switch (mode.load())
        {
            case Mode::Gain:
                buffer.applyGain (gain.load());
                break;
            case Mode::EmitNaN:
                buffer.setSample (0, 0, std::numeric_limits<float>::quiet_NaN());
                break;
            case Mode::Throw:
                throw std::runtime_error ("plugin bug");
        }
    }

    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& data) override
    {
        const float g = gain.load();
        data.replaceAll (&g, sizeof (g));
    }

    void setStateInformation (const void* data, int size) override
    {
        if (size == sizeof (float))
            gain.store (*static_cast<const float*> (data));
    }

private:
    juce::String identifier;
};
} // namespace livedaw::test
