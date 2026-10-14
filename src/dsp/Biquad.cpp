#include "dsp/Biquad.h"
#include "dsp/DspUtils.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace livedaw::dsp
{

namespace
{
    struct Prewarp
    {
        double cosw, sinw, alpha;
    };

    Prewarp prewarp (double sampleRate, double freq, double q) noexcept
    {
        freq = std::clamp (freq, 10.0, sampleRate * 0.49);
        q = std::max (q, 0.025);
        const double w0 = 2.0 * kPi * freq / sampleRate;
        return { std::cos (w0), std::sin (w0), std::sin (w0) / (2.0 * q) };
    }

    BiquadCoeffs normalise (double b0, double b1, double b2, double a0, double a1, double a2) noexcept
    {
        return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
    }
} // namespace

BiquadCoeffs BiquadCoeffs::peak (double sr, double freq, double q, double gainDb) noexcept
{
    const auto p = prewarp (sr, freq, q);
    const double A = std::pow (10.0, gainDb / 40.0);

    return normalise (1.0 + p.alpha * A, -2.0 * p.cosw, 1.0 - p.alpha * A,
                      1.0 + p.alpha / A, -2.0 * p.cosw, 1.0 - p.alpha / A);
}

BiquadCoeffs BiquadCoeffs::lowShelf (double sr, double freq, double q, double gainDb) noexcept
{
    const auto p = prewarp (sr, freq, q);
    const double A = std::pow (10.0, gainDb / 40.0);
    const double twoSqrtAalpha = 2.0 * std::sqrt (A) * p.alpha;

    return normalise (A * ((A + 1.0) - (A - 1.0) * p.cosw + twoSqrtAalpha),
                      2.0 * A * ((A - 1.0) - (A + 1.0) * p.cosw),
                      A * ((A + 1.0) - (A - 1.0) * p.cosw - twoSqrtAalpha),
                      (A + 1.0) + (A - 1.0) * p.cosw + twoSqrtAalpha,
                      -2.0 * ((A - 1.0) + (A + 1.0) * p.cosw),
                      (A + 1.0) + (A - 1.0) * p.cosw - twoSqrtAalpha);
}

BiquadCoeffs BiquadCoeffs::highShelf (double sr, double freq, double q, double gainDb) noexcept
{
    const auto p = prewarp (sr, freq, q);
    const double A = std::pow (10.0, gainDb / 40.0);
    const double twoSqrtAalpha = 2.0 * std::sqrt (A) * p.alpha;

    return normalise (A * ((A + 1.0) + (A - 1.0) * p.cosw + twoSqrtAalpha),
                      -2.0 * A * ((A - 1.0) + (A + 1.0) * p.cosw),
                      A * ((A + 1.0) + (A - 1.0) * p.cosw - twoSqrtAalpha),
                      (A + 1.0) - (A - 1.0) * p.cosw + twoSqrtAalpha,
                      2.0 * ((A - 1.0) - (A + 1.0) * p.cosw),
                      (A + 1.0) - (A - 1.0) * p.cosw - twoSqrtAalpha);
}

BiquadCoeffs BiquadCoeffs::highPass (double sr, double freq, double q) noexcept
{
    const auto p = prewarp (sr, freq, q);

    return normalise ((1.0 + p.cosw) * 0.5, -(1.0 + p.cosw), (1.0 + p.cosw) * 0.5,
                      1.0 + p.alpha, -2.0 * p.cosw, 1.0 - p.alpha);
}

BiquadCoeffs BiquadCoeffs::lowPass (double sr, double freq, double q) noexcept
{
    const auto p = prewarp (sr, freq, q);

    return normalise ((1.0 - p.cosw) * 0.5, 1.0 - p.cosw, (1.0 - p.cosw) * 0.5,
                      1.0 + p.alpha, -2.0 * p.cosw, 1.0 - p.alpha);
}

double BiquadCoeffs::magnitudeAt (double freq, double sampleRate) const noexcept
{
    const double w = 2.0 * kPi * freq / sampleRate;
    const std::complex<double> z1 = std::polar (1.0, -w);
    const std::complex<double> z2 = z1 * z1;

    const auto num = b0 + b1 * z1 + b2 * z2;
    const auto den = 1.0 + a1 * z1 + a2 * z2;
    return std::abs (num / den);
}

} // namespace livedaw::dsp
