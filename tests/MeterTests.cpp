#include "TestHelpers.h"
#include "ui/MeterBank.h"
#include "ui/MeterComponent.h"
#include "ui/ScreenWidgets.h"

using namespace livedaw;
using livedaw::test::OfflineRig;
namespace meters = livedaw::ui::meters;

/** Meter ballistics (MeterBallistics.h) and the meter bank every view reads:
    the attack, the release in dB/s, the METERS screen's peak hold, the same
    reading on every view of a signal, the PRE tap (after TRIM, before the EQ)
    and the LED colours. Time is driven explicitly (no sleeps). The numbers are
    a WING owner's: strip LEDs dark from -6 dB in under 1 s, no hold on the
    strips, CLIP gone at once on the surface and held 1 s on METERS. */
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

        beginTest ("Release: 60 dB/s, whatever the frame rate");
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
            expectEquals (meters::kReleaseDbPerSecond, 60.0f);
            // A steady -6 dB tone pulled out: below the strips' -60 dB LED (dark) in 0.9 s,
            // and from full scale in 1 s.
            const auto secondsToDark = [&] (float fromGain)
            {
                meters::Ballistics d;
                d.advance (fromGain, frame);
                int frames = 0;
                while (d.levelDb >= -60.0f && frames < 1000)
                {
                    d.advance (0.0f, frame);
                    ++frames;
                }
                return (float) frames * frame;
            };
            expectWithinAbsoluteError (secondsToDark (half), 0.9f, 1.5f * frame);
            expect (secondsToDark (half) <= 1.0f);
            expectWithinAbsoluteError (secondsToDark (1.0f), 1.0f, 1.5f * frame);
            // A full-scale peak (CLIP, the -0.1 dB LED) is gone after one frame on the surface.
            {
                meters::Ballistics c;
                c.advance (1.0f, frame);
                expect (c.levelDb >= -0.1f);
                c.advance (0.0f, frame);
                expect (c.levelDb < -0.1f);
            }
            meters::Ballistics b;
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

        beginTest ("Peak hold (the METERS screen's only): 1 s, then it falls at the release rate, never below the level");
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
            // Silence: the level falls 60 dB/s on every point; the hold stays 1 s.
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

        beginTest ("The PRE meter tap: after TRIM, before the EQ (TRIM moves it, the EQ doesn't); POST after the fader");
        {
            // Every PRE / POST setting (SETUP > SURFACE > METERING, METERS > SETTINGS,
            // MAIN METER TAP) measures here.
            expect (ui::prefs::tapPoint (true) == MeterPoint::Trim);
            expect (ui::prefs::tapPoint (false) == MeterPoint::Output);
            expectEquals (ui::prefs::meterTapIndex (StripKind::Channel), 0);
            expectEquals (ui::prefs::meterTapIndex (StripKind::Aux), 0);
            expectEquals (ui::prefs::meterTapIndex (StripKind::Bus), 1);
            expectEquals (ui::prefs::meterTapIndex (StripKind::Main), 2);
            expectEquals (ui::prefs::meterTapIndex (StripKind::Matrix), 3);

            MixerEngine engine (0);
            OfflineRig rig (engine, 2, 2);
            test::setChannels (engine, 1);
            auto& ch = *engine.getChannels()[0];
            ch.params.faderDb = 0.0f;
            // A 750 Hz tone at -12 dB: two periods a block.
            for (int i = 0; i < OfflineRig::kBlock; ++i)
                rig.inputs[0][(size_t) i] = 0.25f * std::sin (juce::MathConstants<float>::twoPi * 2.0f * (float) i / (float) OfflineRig::kBlock);
            ui::MeterBank bank (engine);
            const auto settle = [&]
            {
                rig.run (200);
                bank.advance (frame); // takes the peaks of the change
                bank.advance (10.0f); // the levels fall to the floor: the next frame shows the settled peaks
                rig.run (4);
                bank.advance (frame);
            };
            const auto pre = [&] { return dsp::gainToDb (bank.get (&ch, ui::prefs::tapPoint (true), 0)); };
            const auto pfl = [&] { return dsp::gainToDb (bank.get (&ch, MeterPoint::PreFader, 0)); };
            const auto post = [&] { return dsp::gainToDb (bank.get (&ch, ui::prefs::tapPoint (false), 0)); };
            settle();
            const float pre0 = pre(), pfl0 = pfl(), post0 = post();
            expectWithinAbsoluteError (pre0, -12.04f, 0.1f);
            // POST follows the fader, PRE doesn't.
            ch.params.faderDb = -10.0f;
            settle();
            expectWithinAbsoluteError (post(), post0 - 10.0f, 0.3f);
            expectWithinAbsoluteError (pre(), pre0, 0.05f);
            ch.params.faderDb = 0.0f;
            // +12 dB of EQ on the tone: PRE stays, the pre-fader level rises.
            ch.params.eq.on = true;
            ch.params.eq.values[8] = 750.0f; // STD 2f
            ch.params.eq.values[7] = 12.0f;  // STD 2g
            settle();
            expectWithinAbsoluteError (pre(), pre0, 0.05f);
            expect (pfl() > pfl0 + 9.0f, "the EQ reaches the pre-fader point: " + juce::String (pfl()));
            // +6 dB of TRIM moves PRE.
            ch.params.input.trimDb = 6.0206f;
            settle();
            expectWithinAbsoluteError (pre(), pre0 + 6.02f, 0.1f);
        }

        beginTest ("LED colours: red CLIP, orange from -18 dB, green below; no yellow");
        {
            // The strips' LEDs (CLIP, -6, -12, -18, -30, -60), the channel strip's LEVEL
            // and the main meter's (CLIP, -1 .. -60) all take leds::meterColour.
            expect (ui::leds::meterColour (-0.1f) == ui::Colours::meterRed);
            for (float db : { -1.0f, -2.0f, -3.0f, -4.0f, -6.0f, -8.0f, -12.0f, -15.0f, -18.0f })
                expect (ui::leds::meterColour (db) == ui::Colours::meterOrange, juce::String (db));
            for (float db : { -24.0f, -30.0f, -36.0f, -48.0f, -60.0f })
                expect (ui::leds::meterColour (db) == ui::Colours::meterGreen, juce::String (db));
            for (float db = -60.0f; db <= 0.0f; db += 0.5f)
                expect (ui::leds::meterColour (db) != ui::Colours::meterYellow);
        }
    }
};

static MeterTests meterTests;
