#pragma once

#include <array>
#include <cstddef>

namespace livedaw::dsp
{

/** Normalised biquad coefficients (a0 == 1). Designs follow the RBJ Audio EQ Cookbook. */
struct BiquadCoeffs
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;

    static BiquadCoeffs identity() noexcept { return {}; }
    static BiquadCoeffs peak (double sampleRate, double freq, double q, double gainDb) noexcept;
    static BiquadCoeffs lowShelf (double sampleRate, double freq, double q, double gainDb) noexcept;
    static BiquadCoeffs highShelf (double sampleRate, double freq, double q, double gainDb) noexcept;
    static BiquadCoeffs highPass (double sampleRate, double freq, double q) noexcept;
    static BiquadCoeffs lowPass (double sampleRate, double freq, double q) noexcept;

    /** Linear magnitude response at `freq` (used for drawing EQ curves). */
    double magnitudeAt (double freq, double sampleRate) const noexcept;
};

/** Transposed direct form II biquad for up to two channels. */
class Biquad
{
public:
    static constexpr int kMaxChannels = 2;

    void setCoefficients (const BiquadCoeffs& c) noexcept { coeffs = c; }
    const BiquadCoeffs& getCoefficients() const noexcept { return coeffs; }

    void reset() noexcept
    {
        z1.fill (0.0);
        z2.fill (0.0);
    }

    inline float processSample (int channel, float x) noexcept
    {
        const double in = x;
        const double out = coeffs.b0 * in + z1[(std::size_t) channel];
        z1[(std::size_t) channel] = coeffs.b1 * in - coeffs.a1 * out + z2[(std::size_t) channel];
        z2[(std::size_t) channel] = coeffs.b2 * in - coeffs.a2 * out;
        return static_cast<float> (out);
    }

    void process (float* data, int channel, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
            data[i] = processSample (channel, data[i]);
    }

private:
    BiquadCoeffs coeffs;
    std::array<double, kMaxChannels> z1 {}, z2 {};
};

} // namespace livedaw::dsp
