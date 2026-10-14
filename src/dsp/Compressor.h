#pragma once

namespace livedaw::dsp
{

/** Stereo-linked feed-forward compressor with a soft knee (log-domain detector). */
class Compressor
{
public:
    struct Settings
    {
        bool enabled = false;
        float thresholdDb = -20.0f;
        float ratio = 4.0f;       // 1 .. 100 (100 ~ limiting)
        float attackMs = 10.0f;
        float releaseMs = 120.0f;
        float kneeDb = 6.0f;
        float makeupDb = 0.0f;
    };

    void prepare (double sampleRate) noexcept;
    void reset() noexcept;
    void setSettings (const Settings& s) noexcept;
    const Settings& getSettings() const noexcept { return settings; }

    void process (float* const* channels, int numChannels, int numSamples) noexcept;

    /** Static gain computer: gain reduction in dB (>= 0) for an input level in dB. */
    static float computeGainReductionDb (const Settings& s, float inputDb) noexcept;

    /** Largest gain reduction (dB, >= 0) during the last processed block. */
    float getLastBlockGainReductionDb() const noexcept { return lastMaxReduction; }

private:
    Settings settings;
    double sampleRate = 48000.0;
    float attackCoeff = 0.0f, releaseCoeff = 0.0f;
    float reductionDb = 0.0f, lastMaxReduction = 0.0f;
};

} // namespace livedaw::dsp
