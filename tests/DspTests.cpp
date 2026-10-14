#include "dsp/Compressor.h"
#include "dsp/DspUtils.h"
#include "dsp/NoiseGate.h"
#include "dsp/ParametricEq.h"

#include <juce_core/juce_core.h>

#include <vector>

using namespace livedaw::dsp;

namespace
{
constexpr double kSr = 48000.0;

std::vector<float> sine (double freq, float amplitude, int numSamples)
{
    std::vector<float> v ((size_t) numSamples);
    for (int i = 0; i < numSamples; ++i)
        v[(size_t) i] = amplitude * static_cast<float> (std::sin (2.0 * kPi * freq * i / kSr));
    return v;
}

float rms (const std::vector<float>& v, int from)
{
    double sum = 0.0;
    for (size_t i = (size_t) from; i < v.size(); ++i)
        sum += v[i] * v[i];
    return static_cast<float> (std::sqrt (sum / static_cast<double> (v.size() - (size_t) from)));
}

/** Measured gain in dB of the EQ at `freq` (after it settles). */
float measureEqGain (ParametricEq& eq, double freq)
{
    eq.reset();
    auto signal = sine (freq, 0.25f, 24000);
    const float before = rms (signal, 12000);
    float* chans[] = { signal.data() };

    for (int pos = 0; pos < static_cast<int> (signal.size()); pos += 256)
    {
        float* block[] = { chans[0] + pos };
        eq.process (block, 1, std::min (256, static_cast<int> (signal.size()) - pos));
    }

    return gainToDb (rms (signal, 12000) / before);
}
} // namespace

class DspTests final : public juce::UnitTest
{
public:
    DspTests() : juce::UnitTest ("DSP", "LiveDAW") {}

    void runTest() override
    {
        beginTest ("dB conversions");
        expectWithinAbsoluteError (dbToGain (0.0f), 1.0f, 1.0e-6f);
        expectWithinAbsoluteError (dbToGain (-6.0206f), 0.5f, 1.0e-4f);
        expectEquals (dbToGain (kMinusInfDb), 0.0f);
        expectWithinAbsoluteError (gainToDb (0.5f), -6.0206f, 1.0e-3f);

        beginTest ("Pan laws");
        {
            float l, r;
            constantPowerPan (0.0f, l, r);
            expectWithinAbsoluteError (l, 0.7071f, 1.0e-3f);
            expectWithinAbsoluteError (r, 0.7071f, 1.0e-3f);
            constantPowerPan (-1.0f, l, r);
            expectWithinAbsoluteError (l, 1.0f, 1.0e-6f);
            expectWithinAbsoluteError (r, 0.0f, 1.0e-6f);
            stereoBalance (0.0f, l, r);
            expectEquals (l, 1.0f);
            expectEquals (r, 1.0f);
            stereoBalance (0.5f, l, r);
            expectWithinAbsoluteError (l, 0.5f, 1.0e-6f);
            expectEquals (r, 1.0f);
        }

        beginTest ("Biquad design matches requested gain");
        {
            const auto c = BiquadCoeffs::peak (kSr, 1000.0, 1.0, 6.0);
            expectWithinAbsoluteError (gainToDb (static_cast<float> (c.magnitudeAt (1000.0, kSr))), 6.0f, 0.05f);
            const auto hp = BiquadCoeffs::highPass (kSr, 100.0, 0.7071);
            expectWithinAbsoluteError (gainToDb (static_cast<float> (hp.magnitudeAt (100.0, kSr))), -3.01f, 0.05f);
            expect (hp.magnitudeAt (20.0, kSr) < 0.05);
        }

        beginTest ("Flat EQ is transparent");
        {
            ParametricEq eq;
            eq.prepare (kSr);
            expectWithinAbsoluteError (measureEqGain (eq, 1000.0), 0.0f, 0.01f);
        }

        beginTest ("EQ bell boosts and cuts at its centre frequency");
        {
            ParametricEq eq;
            eq.prepare (kSr);
            ParametricEq::Settings s;
            s.bands[1] = { ParametricEq::BandType::Bell, 1000.0f, 9.0f, 1.0f, true };
            s.bands[2] = { ParametricEq::BandType::Bell, 5000.0f, -12.0f, 2.0f, true };
            eq.setSettings (s);

            // Bands overlap slightly, so compare against the analytic combined response
            // and check that it lands near the nominal band gains.
            for (double f : { 1000.0, 5000.0 })
                expectWithinAbsoluteError (measureEqGain (eq, f),
                                           static_cast<float> (ParametricEq::responseDb (s, f, kSr)), 0.1f);

            expectWithinAbsoluteError (static_cast<float> (ParametricEq::responseDb (s, 1000.0, kSr)), 9.0f, 0.5f);
            expectWithinAbsoluteError (static_cast<float> (ParametricEq::responseDb (s, 5000.0, kSr)), -12.0f, 0.5f);
        }

        beginTest ("EQ shelves");
        {
            ParametricEq eq;
            eq.prepare (kSr);
            ParametricEq::Settings s;
            s.bands[0].gainDb = 6.0f;   // low shelf @100 Hz
            s.bands[3].gainDb = -6.0f;  // high shelf @8 kHz
            eq.setSettings (s);

            expectWithinAbsoluteError (measureEqGain (eq, 30.0), 6.0f, 0.5f);
            expectWithinAbsoluteError (measureEqGain (eq, 1000.0), 0.0f, 0.5f);
            expectWithinAbsoluteError (measureEqGain (eq, 18000.0), -6.0f, 0.5f);
        }

        beginTest ("Disabled EQ bypasses");
        {
            ParametricEq eq;
            eq.prepare (kSr);
            ParametricEq::Settings s;
            s.enabled = false;
            s.bands[1].gainDb = 12.0f;
            eq.setSettings (s);
            expectWithinAbsoluteError (measureEqGain (eq, 400.0), 0.0f, 0.01f);
        }

        beginTest ("Compressor static curve");
        {
            Compressor::Settings s;
            s.thresholdDb = -20.0f;
            s.ratio = 4.0f;
            s.kneeDb = 0.0f;
            expectWithinAbsoluteError (Compressor::computeGainReductionDb (s, -30.0f), 0.0f, 1.0e-6f);
            expectWithinAbsoluteError (Compressor::computeGainReductionDb (s, -8.0f), 9.0f, 1.0e-4f);

            s.kneeDb = 6.0f;
            expectWithinAbsoluteError (Compressor::computeGainReductionDb (s, -20.0f), 0.5625f, 1.0e-4f);
            expectWithinAbsoluteError (Compressor::computeGainReductionDb (s, -10.0f), 7.5f, 1.0e-4f);
        }

        beginTest ("Compressor reduces loud signals");
        {
            Compressor comp;
            Compressor::Settings s;
            s.enabled = true;
            s.thresholdDb = -20.0f;
            s.ratio = 4.0f;
            s.kneeDb = 0.0f;
            s.attackMs = 1.0f;
            comp.setSettings (s);
            comp.prepare (kSr);

            auto signal = sine (1000.0, 1.0f, 48000); // 0 dBFS peak
            float* ch[] = { signal.data() };
            comp.process (ch, 1, static_cast<int> (signal.size()));

            // Peak detector on a 0 dBFS sine -> 20 dB over -> 15 dB reduction.
            expectWithinAbsoluteError (comp.getLastBlockGainReductionDb(), 15.0f, 0.5f);
            expect (peakLevel (signal.data() + 40000, 8000) < dbToGain (-14.0f));
        }

        beginTest ("Gate closes on quiet signals and opens on loud ones");
        {
            NoiseGate gate;
            NoiseGate::Settings s;
            s.enabled = true;
            s.thresholdDb = -40.0f;
            s.rangeDb = -60.0f;
            s.releaseMs = 20.0f;
            s.holdMs = 10.0f;
            gate.setSettings (s);
            gate.prepare (kSr);

            auto quiet = sine (500.0, dbToGain (-60.0f), 24000);
            float* q[] = { quiet.data() };
            gate.process (q, 1, static_cast<int> (quiet.size()));
            expect (! gate.isOpen());
            expect (peakLevel (quiet.data() + 20000, 4000) < 3.2e-6f); // -60 dB signal, -60 dB range => ~-120 dB

            auto loud = sine (500.0, dbToGain (-10.0f), 24000);
            float* l[] = { loud.data() };
            gate.process (l, 1, static_cast<int> (loud.size()));
            expect (gate.isOpen());
            expectWithinAbsoluteError (peakLevel (loud.data() + 20000, 4000), dbToGain (-10.0f), 0.01f);
        }

        beginTest ("Disabled gate and compressor are transparent");
        {
            NoiseGate gate;
            gate.prepare (kSr);
            Compressor comp;
            comp.prepare (kSr);

            auto signal = sine (440.0, 0.5f, 4800);
            const auto original = signal;
            float* ch[] = { signal.data() };
            gate.process (ch, 1, 4800);
            comp.process (ch, 1, 4800);
            expect (signal == original);
        }
    }
};

static DspTests dspTests;
