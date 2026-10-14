#pragma once

#include "dsp/Biquad.h"

#include <array>

namespace livedaw::dsp
{

/** Four-band parametric EQ, as found on a digital console channel.

    Band 0 (Low) and band 3 (High) can be switched between shelf and bell, the two
    mid bands are fully parametric bells. Parameter changes are smoothed and
    coefficients are recomputed every kSubBlock samples, so sweeping a band while
    audio is running is click-free.
*/
class ParametricEq
{
public:
    static constexpr int kNumBands = 4;
    static constexpr int kSubBlock = 32;

    enum class BandType
    {
        Bell = 0,
        LowShelf,
        HighShelf,
        LowCut,
        HighCut
    };

    struct Band
    {
        BandType type = BandType::Bell;
        float frequency = 1000.0f; // Hz
        float gainDb = 0.0f;       // -18 .. +18
        float q = 0.707f;
        bool enabled = true;
    };

    struct Settings
    {
        bool enabled = true;
        std::array<Band, kNumBands> bands = defaultBands();

        static std::array<Band, kNumBands> defaultBands() noexcept
        {
            return { { { BandType::LowShelf, 100.0f, 0.0f, 0.707f, true },
                       { BandType::Bell, 400.0f, 0.0f, 1.0f, true },
                       { BandType::Bell, 2500.0f, 0.0f, 1.0f, true },
                       { BandType::HighShelf, 8000.0f, 0.0f, 0.707f, true } } };
        }
    };

    void prepare (double sampleRate) noexcept;
    void reset() noexcept;
    void setSettings (const Settings& s) noexcept { target = s; }
    const Settings& getSettings() const noexcept { return target; }

    /** Processes `numChannels` (1 or 2) channels in place. */
    void process (float* const* channels, int numChannels, int numSamples) noexcept;

    /** Designs the coefficients for one band (also used by the UI to draw the curve). */
    static BiquadCoeffs designBand (const Band& band, double sampleRate) noexcept;

    /** Combined magnitude response in dB of a set of settings at `freq`. */
    static double responseDb (const Settings& settings, double freq, double sampleRate) noexcept;

private:
    struct SmoothedBand
    {
        float freq = 1000.0f, gainDb = 0.0f, q = 0.707f;
    };

    void updateCoefficients (bool snap) noexcept;

    double sampleRate = 48000.0;
    Settings target;
    std::array<SmoothedBand, kNumBands> current {};
    std::array<Band, kNumBands> designed {}; // parameters the filters were last designed with
    std::array<Biquad, kNumBands> filters;
    float smoothingCoeff = 0.0f;
    bool needsSnap = true;
};

} // namespace livedaw::dsp
