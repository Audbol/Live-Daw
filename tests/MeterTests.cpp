#include "TestHelpers.h"
#include "ui/MeterBank.h"

using namespace livedaw;
using livedaw::test::OfflineRig;
namespace meters = livedaw::ui::meters;

/** Meter ballistics (MeterBallistics.h) and the meter bank every view reads:
    the attack, the release in dB/s, the peak hold, the same reading on every
    view of a signal. Time is driven explicitly (no sleeps). */
class MeterTests final : public juce::UnitTest
{
public:
    MeterTests() : juce::UnitTest ("Meters", "LiveDAW") {}

    void runTest() override
    {
        constexpr float frame = 1.0f / (float) ui::MeterBank::kRateHz;
        const float half = dsp::dbToGain (-6.0206f);

        beginTest ("Attack: a peak shows in full on the next frame, however short");
        {
            meters::Ballistics b;
            b.advance (half, frame); // e.g. a 1 ms click caught in this frame's peak
            expectWithinAbsoluteError (b.levelDb, -6.02f, 0.01f);
            expectWithinAbsoluteError (b.holdDb, -6.02f, 0.01f);
        }

        beginTest ("Release: 24 dB/s, whatever the frame rate");
        {
            for (float rate : { 20.0f, 30.0f, 40.0f, 60.0f })
            {
                meters::Ballistics b;
                b.advance (1.0f, 1.0f / rate);
                const int frames = (int) std::lround (rate * 0.5f); // half a second of silence
                for (int i = 0; i < frames; ++i)
                    b.advance (0.0f, 1.0f / rate);
                expectWithinAbsoluteError (b.levelDb, -meters::kReleaseDbPerSecond * 0.5f, 0.01f);
            }
            expectEquals (meters::kReleaseDbPerSecond, 24.0f);
            // From full scale to the bottom of the screens' scale (-60 dB) in 2.5 s.
            meters::Ballistics b;
            b.advance (1.0f, frame);
            int frames = 0;
            while (b.levelDb > -60.0f && frames < 1000)
            {
                b.advance (0.0f, frame);
                ++frames;
            }
            expectWithinAbsoluteError ((float) frames * frame, 2.5f, 1.5f * frame);
            // A louder peak takes over at once; a quieter one waits for the fall.
            b.reset();
            b.advance (dsp::dbToGain (-20.0f), frame);
            b.advance (dsp::dbToGain (-30.0f), frame);
            expectWithinAbsoluteError (b.levelDb, -20.0f - meters::kReleaseDbPerSecond * frame, 0.01f);
            b.advance (dsp::dbToGain (-3.0f), frame);
            expectWithinAbsoluteError (b.levelDb, -3.0f, 0.01f);
            // Silence ends at the floor.
            b.advance (0.0f, 30.0f);
            expectEquals (b.levelDb, meters::kFloorDb);
        }

        beginTest ("Peak hold: 1 s, then it falls at the release rate, never below the level");
        {
            meters::Ballistics b;
            b.advance (1.0f, frame);
            float t = 0.0f;
            while (t < meters::kHoldSeconds - 0.5f * frame)
            {
                b.advance (0.0f, frame);
                t += frame;
                expectEquals (b.holdDb, 0.0f); // held
            }
            b.advance (0.0f, 0.5f);
            expectWithinAbsoluteError (b.holdDb, -meters::kReleaseDbPerSecond * 0.5f, 0.05f);
            expect (b.holdDb >= b.levelDb);
            // A new peak restarts the hold; a steady level keeps the mark on it.
            b.reset();
            for (int i = 0; i < 200; ++i)
                b.advance (half, frame);
            expectWithinAbsoluteError (b.holdDb, -6.02f, 0.01f);
            expectWithinAbsoluteError (b.levelDb, -6.02f, 0.01f);
        }

        beginTest ("The screens' scale: CLIP, -6, -12, -24, -36, -48, -60 evenly spaced");
        {
            const float marks[] = { -60.0f, -48.0f, -36.0f, -24.0f, -12.0f, -6.0f, 0.0f };
            for (int i = 0; i < 7; ++i)
                expectWithinAbsoluteError (meters::scalePos (marks[i]), (float) i / 6.0f, 1.0e-6f);
            expectEquals (meters::scalePos (-80.0f), 0.0f);
            expectEquals (meters::scalePos (3.0f), 1.0f);
            expectWithinAbsoluteError (meters::scalePos (-18.0f), 3.5f / 6.0f, 1.0e-6f);
        }

        beginTest ("The meter bank: every point and view of a signal reads the same ballistics");
        {
            MixerEngine engine (0);
            OfflineRig rig (engine, 2, 2);
            test::setChannels (engine, 1);
            auto& ch = *engine.getChannels()[0];
            rig.setInput (0, 0.5f);
            rig.run (8);
            ui::MeterBank bank (engine);
            bank.advance (frame);
            const auto level = [&] (MeterPoint p) { return dsp::gainToDb (bank.get (&ch, p, 0)); };
            expectWithinAbsoluteError (level (MeterPoint::Input), -6.02f, 0.01f);
            expectWithinAbsoluteError (level (MeterPoint::PreFader), -6.02f, 0.01f);
            expectWithinAbsoluteError (dsp::gainToDb (bank.getHold (&ch, MeterPoint::Input, 0)), -6.02f, 0.01f);
            // Silence: the level falls 24 dB/s on every point; the hold stays 1 s.
            rig.setInput (0, 0.0f);
            rig.run (8);
            bank.advance (frame); // silent blocks only: the first frame of the fall
            for (int i = 0; i < 20; ++i)
            {
                rig.run (1);
                bank.advance (frame);
            }
            const float expected = -6.02f - meters::kReleaseDbPerSecond * 21.0f * frame;
            expectWithinAbsoluteError (level (MeterPoint::Input), expected, 0.05f);
            expectWithinAbsoluteError (level (MeterPoint::PreFader), expected, 0.05f);
            expectWithinAbsoluteError (dsp::gainToDb (bank.getHold (&ch, MeterPoint::Input, 0)), -6.02f, 0.01f);
            // Unknown strips read silence.
            expectEquals (bank.get (nullptr, MeterPoint::Input, 0), 0.0f);
        }
    }
};

static MeterTests meterTests;
