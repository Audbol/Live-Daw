#include "TestHelpers.h"
#include "engine/Show.h"

#include <thread>

using namespace livedaw;
using livedaw::test::FakePlugin;
using livedaw::test::OfflineRig;
using livedaw::test::kCentre;

class StabilityTests final : public juce::UnitTest
{
public:
    StabilityTests() : juce::UnitTest ("Stability", "LiveDAW") {}

    void runTest() override
    {
        beginTest ("An FX slot plugin that outputs NaN is auto-bypassed");
        {
            MixerEngine engine (1);
            OfflineRig rig (engine, 1, 2);
            test::setChannels (engine, 1);
            auto& strip = *engine.getChannels()[0];
            engine.getFx().setPlugin (0, std::make_unique<FakePlugin> (1.0f, FakePlugin::Mode::EmitNaN));
            strip.params.preInsert.on = true;
            strip.params.preInsert.fx = 0;
            rig.setInput (0, 0.5f);
            rig.run();

            expect (engine.getFx().isFaulted (0));
            expectWithinAbsoluteError (rig.out (0), 0.5f * kCentre, 1.0e-5f); // dry signal passes
            expect (std::isfinite (rig.out (0)));
            engine.getGarbage().collectAllNow();
        }

        beginTest ("An FX slot plugin that throws is auto-bypassed");
        {
            MixerEngine engine (1);
            OfflineRig rig (engine, 1, 2);
            test::setChannels (engine, 1);
            auto& strip = *engine.getChannels()[0];
            auto& fx = engine.getFx();
            fx.setPlugin (0, std::make_unique<FakePlugin> (1.0f, FakePlugin::Mode::Throw));
            fx.setPlugin (1, std::make_unique<FakePlugin> (0.5f));
            strip.params.preInsert.on = true;
            strip.params.preInsert.fx = 0;
            strip.params.postInsert.on = true;
            strip.params.postInsert.fx = 1;
            rig.setInput (0, 0.5f);
            rig.run();

            expect (fx.isFaulted (0));
            expect (! fx.isFaulted (1));
            expectWithinAbsoluteError (rig.out (0), 0.25f * kCentre, 1.0e-5f); // the healthy plugin still runs
            expectEquals (engine.getNumFaultedFx(), 1);
            expectEquals (fx.getNumUsers (1), 1);
            expectEquals (fx.describe (1), juce::String ("FX 2: Fake fake-gain"));

            fx.clearFlags (0);
            expect (! fx.isFaulted (0));
            engine.getGarbage().collectAllNow();
        }

        beginTest ("Two inserts on one FX slot never run it concurrently");
        {
            MixerEngine engine (2);
            OfflineRig rig (engine, 2, 2);
            test::setChannels (engine, 2);
            engine.getChannels()[1]->params.inputLeft = 1;
            engine.getFx().setPlugin (3, std::make_unique<FakePlugin> (0.5f));
            for (auto& ch : engine.getChannels())
            {
                ch->params.preInsert.on = true;
                ch->params.preInsert.fx = 3;
            }
            rig.setInput (0, 0.5f);
            rig.setInput (1, 0.5f);
            rig.run();
            expect (engine.getFx().isDoubleAssigned (3));
            expectEquals (engine.getFx().getNumUsers (3), 2);
            expect (std::isfinite (rig.out (0)));
            engine.getGarbage().collectAllNow();
        }

        beginTest ("NaN from an input never reaches buses or outputs");
        {
            MixerEngine engine (1);
            OfflineRig rig (engine, 2, 2);
            test::setChannels (engine, 2);
            engine.getChannels()[1]->params.inputLeft = 1;
            engine.getChannels()[0]->params.eq.values[4] = 6.0f; // filter state that NaN would poison
            rig.setInput (0, std::numeric_limits<float>::quiet_NaN());
            rig.setInput (1, 0.25f);
            rig.run (4);

            expect (engine.getChannels()[0]->meters.dspFaults.load() > 0);
            expectWithinAbsoluteError (rig.out (0), 0.25f * kCentre, 1.0e-5f);

            // The faulty channel recovers as soon as its input is sane again.
            rig.setInput (0, 0.25f);
            rig.run (40);
            expect (std::isfinite (rig.out (0)));
            expect (rig.out (0) > 0.4f * kCentre);
        }

        beginTest ("Output protection clips at the ceiling");
        {
            MixerEngine engine (0);
            OfflineRig rig (engine, 1, 2);
            test::setChannels (engine, 1);
            engine.getChannels()[0]->params.faderDb = 10.0f;
            rig.setInput (0, 0.9f);
            rig.run();
            expectWithinAbsoluteError (rig.out (0), 1.0f, 1.0e-6f);
            expect (engine.outputClipEvents.load() > 0);

            engine.outputCeilingDb = -6.0206f;
            rig.run();
            expectWithinAbsoluteError (rig.out (0), 0.5f, 1.0e-4f);

            engine.outputProtection = false;
            rig.run();
            expect (rig.out (0) > 2.0f);
        }

        beginTest ("Stereo channels can be switched live");
        {
            MixerEngine engine (0);
            OfflineRig rig (engine, 2, 2);
            test::setChannels (engine, 1);
            auto& p = engine.getChannels()[0]->params;
            p.inputLeft = 0;
            rig.setInput (0, 0.5f);
            rig.setInput (1, 0.25f);

            rig.run();
            expectEquals (engine.getChannels()[0]->getNumChannels(), 1);
            expectWithinAbsoluteError (rig.out (1), 0.5f * kCentre, 1.0e-5f);

            p.stereo = true; // right input defaults to the next hardware input
            rig.run();
            expectEquals (engine.getChannels()[0]->getNumChannels(), 2);
            expectWithinAbsoluteError (rig.out (0), 0.5f, 1.0e-5f);
            expectWithinAbsoluteError (rig.out (1), 0.25f, 1.0e-5f);
        }

        beginTest ("Stress: structural changes while audio runs on another thread");
        {
            MixerEngine engine (2);
            PluginHost host (nullptr);
            Show show (engine, host);
            OfflineRig rig (engine, 8, 8);
            for (int i = 0; i < 8; ++i)
                rig.setInput (i, 0.01f * static_cast<float> (i + 1));

            test::setChannels (engine, 8);
            show.addScene ("A");
            engine.getGarbage().setDeviceRunning (true); // garbage must now wait for callbacks

            std::atomic<bool> running { true }, bad { false };
            std::atomic<int> blocks { 0 };

            std::thread audio ([&]
            {
                std::vector<const float*> in;
                std::vector<float*> out;
                for (auto& v : rig.inputs)
                    in.push_back (v.data());
                for (auto& v : rig.outputs)
                    out.push_back (v.data());

                while (running.load())
                {
                    engine.processBlock (in.data(), (int) in.size(), out.data(), (int) out.size(), OfflineRig::kBlock);
                    for (auto& o : rig.outputs)
                        if (! std::isfinite (o[0]))
                            bad = true;
                    ++blocks;
                }
            });

            juce::Random rng (42);
            const auto endTime = juce::Time::getMillisecondCounter() + 2500;
            int operations = 0;

            while (juce::Time::getMillisecondCounter() < endTime)
            {
                switch (rng.nextInt (7))
                {
                    case 0: test::setChannels (engine, rng.nextInt (64)); break;
                    case 1: engine.addBus(); break;
                    case 2:
                        if (! engine.getBuses().empty())
                            engine.removeStrip (engine.getBuses()[(size_t) rng.nextInt ((int) engine.getBuses().size())].get());
                        break;
                    case 3:
                    {
                        const int slot = rng.nextInt (kNumFx);
                        if (rng.nextBool())
                            engine.getFx().setPlugin (slot, std::make_unique<FakePlugin> (0.5f));
                        else
                            engine.getFx().setPlugin (slot, nullptr);

                        if (! engine.getChannels().empty())
                        {
                            auto& s = *engine.getChannels()[(size_t) rng.nextInt ((int) engine.getChannels().size())];
                            s.params.preInsert.on = rng.nextBool();
                            s.params.preInsert.fx = rng.nextInt (kNumFx);
                            s.params.processOrder = rng.nextInt (24);
                            s.params.stereo = rng.nextBool();
                            s.sends[(size_t) rng.nextInt (128)].enabled = rng.nextBool();
                        }
                        break;
                    }
                    case 4: show.addScene ("S" + juce::String (operations)); break;
                    case 5: show.recallScene (rng.nextInt (juce::jmax (1, show.getNumScenes()))); break;
                    case 6:
                    {
                        auto xml = show.toXml();
                        show.loadFromXml (*xml, false);
                        break;
                    }
                }

                engine.getGarbage().collect();
                ++operations;
            }

            running = false;
            audio.join();
            engine.getGarbage().setDeviceRunning (false);
            engine.getGarbage().collect();

            logMessage (juce::String (operations) + " structural operations during " + juce::String (blocks.load())
                        + " audio blocks");
            expect (! bad.load());
            expect (blocks.load() > 100);
            expectEquals ((int) engine.getGarbage().numPending(), 0);
        }
    }
};

static StabilityTests stabilityTests;
