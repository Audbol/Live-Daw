#pragma once

namespace livedaw::dsp
{

/** Stereo-linked noise gate with hold and hysteresis. */
class NoiseGate
{
public:
    struct Settings
    {
        bool enabled = false;
        float thresholdDb = -50.0f;
        float rangeDb = -80.0f;     // attenuation applied while closed
        float attackMs = 0.5f;
        float holdMs = 50.0f;
        float releaseMs = 150.0f;
        float hysteresisDb = 4.0f;  // closes this far below the threshold
    };

    void prepare (double sampleRate) noexcept;
    void reset() noexcept;
    void setSettings (const Settings& s) noexcept;
    const Settings& getSettings() const noexcept { return settings; }

    void process (float* const* channels, int numChannels, int numSamples) noexcept;

    /** Lowest gain (linear, 0..1) applied during the last processed block. */
    float getLastBlockMinGain() const noexcept { return lastMinGain; }
    bool isOpen() const noexcept { return open; }

private:
    void updateCoefficients() noexcept;

    Settings settings;
    double sampleRate = 48000.0;

    float openThreshold = 0.0f, closeThreshold = 0.0f, rangeGain = 0.0f;
    float attackCoeff = 0.0f, releaseCoeff = 0.0f, detectorRelease = 0.0f;
    int holdSamples = 0;

    float envelope = 0.0f, gain = 1.0f, lastMinGain = 1.0f;
    int holdCounter = 0;
    bool open = false;
};

} // namespace livedaw::dsp
