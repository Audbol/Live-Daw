#include "TestHelpers.h"
#include "engine/Models.h"
#include "ui/MeterBank.h"
#include "ui/SurfaceLock.h"
#include "ui/TapTempo.h"

using namespace livedaw;
using livedaw::test::OfflineRig;

/** WING behaviours of the surface and the screens kept apart from their
    components: KEY SOURCE of aux, bus, main and matrix dynamics, SEL DBL
    CLICK = BUSFX, MAIN METER SOURCE, the surface lock and tap tempo. */
class ConsoleRulesTests final : public juce::UnitTest
{
public:
    ConsoleRulesTests() : juce::UnitTest ("Console rules", "LiveDAW") {}

    void runTest() override
    {
        beginTest ("KEY SOURCE: aux, buses, mains and matrices key from buses, mains, matrices and aux");
        {
            // The WING's /bus/n/dynsc/src list: BUS.1-16, MAIN.1-4, MTX.1-8, AUX.1-8.
            expectEquals (MixerEngine::kNumOutputKeySources, 36);
            expectEquals (MixerEngine::keySourceName (MixerEngine::outputKeySource (0)), juce::String ("BUS.1"));
            expectEquals (MixerEngine::keySourceName (MixerEngine::outputKeySource (16)), juce::String ("MAIN.1"));
            expectEquals (MixerEngine::keySourceName (MixerEngine::outputKeySource (27)), juce::String ("MTX.8"));
            expectEquals (MixerEngine::keySourceName (MixerEngine::outputKeySource (35)), juce::String ("AUX.8"));
            expectEquals (MixerEngine::keySourceName (-1), juce::String ("SELF"));
            expectEquals (MixerEngine::keySourceName (4), juce::String ("CH.5"));
            for (int n = 0; n < MixerEngine::kNumOutputKeySources; ++n)
                expectEquals (MixerEngine::outputKeySourceIndex (MixerEngine::outputKeySource (n)), n);
            expectEquals (MixerEngine::outputKeySourceIndex (3), -1);
            expectEquals (MixerEngine::outputKeyTapNames().size(), ld::kNumOutputKeyTaps);

            // Bus 2's dynamics keyed from Bus 1 (BUS tap), through the engine.
            MixerEngine engine (0);
            OfflineRig rig (engine, 2, 2);
            test::setChannels (engine, 1);
            auto& ch = *engine.getChannels()[0];
            auto& send = ch.sends[0];
            send.enabled = true;
            send.levelDb = 0.0f;
            auto& bus2 = *engine.getBuses()[1];
            bus2.setModel (ModelBlock::Dynamics, models::findModel (models::Dynamics, "GATE"));
            bus2.params.dynamics.on = true;
            bus2.params.dynamics.scSource = MixerEngine::outputKeySource (0);
            bus2.params.dynamics.scTap = 0;
            engine.getGarbage().collect(); // plans Bus 1's key tap
            rig.run (10);
            expect (bus2.meters.dynKeyDb.load() < -99.0f, juce::String (bus2.meters.dynKeyDb.load()));
            rig.setInput (0, 0.5f);
            rig.run (10);
            expect (bus2.meters.dynKeyDb.load() > -30.0f, juce::String (bus2.meters.dynKeyDb.load()));
        }

        beginTest ("SEL DBL CLICK = BUSFX: INS 1 on buses, mains and matrices, INS 2 when INS 1 is unused");
        {
            MixerEngine engine (0);
            test::setChannels (engine, 1);
            expectEquals (engine.getChannels()[0]->busFxInsert(), 0);
            expectEquals (engine.getAux()[0]->busFxInsert(), 0);
            for (auto* s : { engine.getBuses()[0].get(), engine.getMains()[0].get(), engine.getMatrices()[0].get() })
            {
                s->params.preInsert.fx = -1;
                expectEquals (s->busFxInsert(), 2);
                s->params.preInsert.fx = 3;
                expectEquals (s->busFxInsert(), 1);
            }
        }

        beginTest ("MAIN METER SOURCE: mains, matrices, phones, speakers, selected channel");
        {
            using namespace ui::mainmeter;
            const auto names = ui::mainmeter::names();
            expectEquals (names.size(), 15);
            expectEquals (names[0], juce::String ("MAIN 1"));
            expectEquals (names[4], juce::String ("MTX 1"));
            expectEquals (names[12], juce::String ("PHONES"));
            expectEquals (names[13], juce::String ("SPEAKERS"));
            expectEquals (names[14], juce::String ("SEL CH"));
            // The older settings keep their meaning: 0 the speakers, 1-4 MAIN n.
            expectEquals (settingAt (13), kSpeakers);
            expectEquals (settingAt (0), 1);
            for (int i = 0; i < kNumSources; ++i)
                expectEquals (listIndex (settingAt (i)), i);
        }

        beginTest ("Surface lock: hold HOME, with other keys as the password");
        {
            using ui::SurfaceLock;
            expectEquals (SurfaceLock::homeOnly(), juce::String ("1000000000000000000"));
            // HOME, ROUTING and the 7th key (the protocol's example).
            std::array<bool, SurfaceLock::kLeftKeys> left {};
            left[3] = left[6] = true;
            const auto code = SurfaceLock::code (left, false, true);
            expectEquals (code, juce::String ("1001001000000000001"));
            SurfaceLock lock;
            expect (lock.holdHome (code) == SurfaceLock::Result::Locked);
            expect (lock.isLocked());
            expectEquals (lock.getCode(), code);
            expect (lock.holdHome (SurfaceLock::homeOnly()) == SurfaceLock::Result::WrongCode);
            expect (lock.isLocked());
            expect (lock.holdHome (code) == SurfaceLock::Result::Unlocked);
            expect (! lock.isLocked() && lock.getCode().isEmpty());
        }

        beginTest ("Tap tempo: the delay TIME, the taps and the blinking LED");
        {
            using ui::TapTempo;
            expect (TapTempo::timeParam (models::findModel (models::Fx, "ST-DL")) >= 0, "a delay has a TIME");
            expectEquals (TapTempo::timeParam (models::findModel (models::Fx, "PLATE")), -1);
            // Every delay taps, the pitch devices' DELAY doesn't; the dials take the tap as their time.
            for (auto* token : { "TAP-DL", "TAPE-DL", "DEL/REV", "OILCAN", "BBD-DL" })
                expect (TapTempo::timeParam (models::findModel (models::Fx, token)) >= 0, token);
            expectEquals (TapTempo::timeParam (models::findModel (models::Fx, "PITCH")), -1);
            for (auto [token, lo, hi] : { std::tuple { "OILCAN", 60.0f, 480.0f }, { "BBD-DL", 20.0f, 600.0f } })
            {
                const int m = models::findModel (models::Fx, token);
                const int p = TapTempo::timeParam (m);
                const auto& spec = models::model (models::Fx, m).params[(size_t) p];
                expectWithinAbsoluteError (TapTempo::valueForMs (m, p, lo), spec.min, 1.0e-3f);
                expectWithinAbsoluteError (TapTempo::valueForMs (m, p, hi), spec.max, 1.0e-3f);
                expectWithinAbsoluteError (TapTempo::msOf (m, p, TapTempo::valueForMs (m, p, 250.0f)), 250.0f, 0.05f);
            }
            {
                const int m = models::findModel (models::Fx, "TAPE-DL");
                expectWithinAbsoluteError (TapTempo::valueForMs (m, TapTempo::timeParam (m), 400.0f), 400.0f, 1.0e-3f);
            }
            TapTempo t;
            expect (! t.tap (10000.0).has_value(), "the first tap counts in");
            expectWithinAbsoluteError (t.tap (10500.0).value_or (0.0f), 500.0f, 0.01f);
            expect (! t.tap (14000.0).has_value(), "longer than 3 s starts again");
            // Blinking at 500 ms from the last tap: lit 250 ms, dark 250 ms.
            expect (t.ledLit (14000.0 + 100.0, 500.0f));
            expect (! t.ledLit (14000.0 + 300.0, 500.0f));
            expect (t.ledLit (14000.0 + 510.0, 500.0f));
        }
    }
};

static ConsoleRulesTests consoleRulesTests;
