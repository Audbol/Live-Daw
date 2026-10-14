#include "TestHelpers.h"

#include <vector>

using namespace livedaw;

using livedaw::test::OfflineRig;
using livedaw::test::kCentre;

class EngineTests final : public juce::UnitTest
{
public:
    EngineTests() : juce::UnitTest ("Engine", "LiveDAW") {}

    void runTest() override
    {
        beginTest ("Input channel reaches the master through pan law");
        {
            MixerEngine engine (2);
            OfflineRig rig (engine, 4, 8);
            engine.setNumInputs (2);
            rig.setInput (0, 0.5f);
            rig.run();

            expectWithinAbsoluteError (rig.out (0), 0.5f * kCentre, 1.0e-5f);
            expectWithinAbsoluteError (rig.out (1), 0.5f * kCentre, 1.0e-5f);

            engine.getInputs()[0]->params.pan = -1.0f;
            rig.run();
            expectWithinAbsoluteError (rig.out (0), 0.5f, 1.0e-5f);
            expectWithinAbsoluteError (rig.out (1), 0.0f, 1.0e-5f);
        }

        beginTest ("Mute, fader and main assign");
        {
            MixerEngine engine (2);
            OfflineRig rig (engine, 2, 2);
            engine.setNumInputs (1);
            auto& p = engine.getInputs()[0]->params;
            rig.setInput (0, 0.5f);

            p.faderDb = -6.0206f;
            rig.run();
            expectWithinAbsoluteError (rig.out (0), 0.25f * kCentre, 1.0e-4f);

            p.mute = true;
            rig.run();
            expectWithinAbsoluteError (rig.out (0), 0.0f, 1.0e-6f);

            p.mute = false;
            p.mainAssign = false;
            rig.run();
            expectWithinAbsoluteError (rig.out (0), 0.0f, 1.0e-6f);
        }

        beginTest ("Stereo channel uses balance and both inputs");
        {
            MixerEngine engine (1);
            OfflineRig rig (engine, 2, 2);
            engine.setNumInputs (1);
            auto& p = engine.getInputs()[0]->params;
            p.stereo = true;
            p.inputLeft = 0;
            p.inputRight = 1;
            rig.setInput (0, 0.25f);
            rig.setInput (1, -0.5f);
            rig.run();
            expectWithinAbsoluteError (rig.out (0), 0.25f, 1.0e-5f);
            expectWithinAbsoluteError (rig.out (1), -0.5f, 1.0e-5f);
        }

        beginTest ("Pre-fader aux send to a hardware output");
        {
            MixerEngine engine (2);
            OfflineRig rig (engine, 2, 6);
            engine.setNumInputs (2);
            auto* aux = engine.addBus (StripKind::Aux);
            expect (aux != nullptr && ! aux->params.stereo.load());
            aux->params.outputLeft = 4;

            auto& p = engine.getInputs()[1]->params;
            p.inputLeft = 1;
            p.faderDb = -100.0f; // fader down: pre-fader send must still be live
            auto& send = p.sends[(size_t) aux->getId()];
            send.enabled = true;
            send.preFader = true;
            send.levelDb = 0.0f;

            rig.setInput (1, 0.5f);
            rig.run();
            expectWithinAbsoluteError (rig.out (4), 0.5f, 1.0e-5f);
            expectWithinAbsoluteError (rig.out (0), 0.0f, 1.0e-6f); // nothing on master

            send.preFader = false; // post-fader send follows the closed fader
            rig.run();
            expectWithinAbsoluteError (rig.out (4), 0.0f, 1.0e-6f);
        }

        beginTest ("Bus count is not capped at 32");
        {
            MixerEngine engine (2);
            OfflineRig rig (engine, 1, 4);
            engine.setNumInputs (1);

            Strip* last = nullptr;
            for (int i = 0; i < 100; ++i)
                last = engine.addBus (i % 2 == 0 ? StripKind::Aux : StripKind::Group);

            expect (last != nullptr);
            expectEquals ((int) engine.getBuses().size(), 100);

            last->params.stereo = false;
            last->params.mainAssign = false;
            last->params.outputLeft = 3;
            auto& send = engine.getInputs()[0]->params.sends[(size_t) last->getId()];
            send.enabled = true;
            send.preFader = true;

            rig.setInput (0, 0.5f);
            rig.run();
            expectWithinAbsoluteError (rig.out (3), 0.5f, 1.0e-5f);

            // Removing a bus frees its slot for reuse, with the old send cleared.
            const int slot = last->getId();
            engine.removeStrip (last);
            auto* reused = engine.addBus (StripKind::Aux);
            expectEquals (reused->getId(), slot);
            expect (! engine.getInputs()[0]->params.sends[(size_t) slot].enabled.load());
            engine.getGarbage().collectAllNow();
        }

        beginTest ("Group bus routes to master");
        {
            MixerEngine engine (2);
            OfflineRig rig (engine, 1, 2);
            engine.setNumInputs (1);
            auto* group = engine.addBus (StripKind::Group);
            auto& p = engine.getInputs()[0]->params;
            p.mainAssign = false;
            p.sends[(size_t) group->getId()].enabled = true;
            p.sends[(size_t) group->getId()].preFader = false;

            rig.setInput (0, 0.5f);
            rig.run();
            expectWithinAbsoluteError (rig.out (0), 0.5f * kCentre, 1.0e-5f);

            group->params.mute = true;
            rig.run();
            expectWithinAbsoluteError (rig.out (0), 0.0f, 1.0e-6f);
        }

        beginTest ("PFL solo replaces master on the monitor outputs");
        {
            MixerEngine engine (2);
            OfflineRig rig (engine, 2, 4);
            engine.setNumInputs (2);
            engine.getInputs()[1]->params.inputLeft = 1;
            engine.monitorOutputLeft = 2;
            engine.monitorOutputRight = 3;
            rig.setInput (0, 0.5f);
            rig.setInput (1, 0.25f);

            rig.run();
            expectWithinAbsoluteError (rig.out (2), 0.75f * kCentre, 1.0e-5f); // follows master

            engine.getInputs()[1]->params.solo = true;
            engine.getInputs()[1]->params.faderDb = -100.0f;
            rig.run();
            expect (engine.anySoloActive.load());
            expectWithinAbsoluteError (rig.out (2), 0.25f, 1.0e-5f); // PFL ignores the fader
            expectWithinAbsoluteError (rig.out (0), 0.5f * kCentre, 1.0e-5f); // master unaffected
        }

        beginTest ("Many channels processed in parallel sum correctly");
        {
            MixerEngine engine (3);
            constexpr int numCh = 256;
            OfflineRig rig (engine, numCh, 2);
            engine.setNumInputs (numCh);
            for (int i = 0; i < numCh; ++i)
                rig.setInput (i, 0.001f);
            rig.run();
            expectWithinAbsoluteError (rig.out (0), numCh * 0.001f * kCentre, 1.0e-4f);

            // Shrink while "running": the next block must reflect the new topology.
            engine.setNumInputs (16);
            rig.run();
            expectWithinAbsoluteError (rig.out (0), 16 * 0.001f * kCentre, 1.0e-5f);
            expectEquals ((int) engine.getInputs().size(), 16);
        }

        beginTest ("Hardware channel maps skip unavailable inputs");
        {
            MixerEngine engine (0);
            OfflineRig rig (engine, 2, 2);
            engine.setNumInputs (3); // channel 3 has no hardware input
            expectEquals (engine.getInputs()[2]->params.inputLeft.load(), -1);
            rig.setInput (0, 0.1f);
            rig.setInput (1, 0.1f);
            rig.run();
            expectWithinAbsoluteError (rig.out (0), 0.2f * kCentre, 1.0e-5f);
        }

        beginTest ("Throughput (informational)");
        {
            // 128 mono channels with gate, EQ and compressor active, 8 buses, 48 kHz / 128 samples.
            MixerEngine engine (livedaw::WorkerPool::defaultNumWorkers());
            constexpr int numCh = 128;
            OfflineRig rig (engine, numCh, 16);
            engine.setNumInputs (numCh);
            for (int b = 0; b < 8; ++b)
                engine.addBus (StripKind::Aux);

            juce::Random rng (1);
            for (int i = 0; i < numCh; ++i)
            {
                auto& p = engine.getInputs()[(size_t) i]->params;
                p.gate.enabled = true;
                p.comp.enabled = true;
                p.hpfEnabled = true;
                p.eq.bands[1].gainDb = 3.0f;
                p.eq.bands[2].gainDb = -3.0f;
                for (int b = 0; b < 8; ++b)
                    p.sends[(size_t) b].enabled = true;
                for (auto& v : rig.inputs[(size_t) i])
                    v = rng.nextFloat() * 0.2f - 0.1f;
            }

            rig.run (10);
            const auto start = juce::Time::getMillisecondCounterHiRes();
            constexpr int blocks = 1000;
            rig.run (blocks);
            const double msPerBlock = (juce::Time::getMillisecondCounterHiRes() - start) / blocks;
            const double budget = 1000.0 * OfflineRig::kBlock / 48000.0;
            logMessage ("128 ch + 8 aux: " + juce::String (msPerBlock, 3) + " ms per block of "
                        + juce::String (budget, 2) + " ms (" + juce::String (100.0 * msPerBlock / budget, 1)
                        + "% of the realtime budget, " + juce::String (engine.getNumWorkerThreads() + 1) + " threads)");
            expect (msPerBlock > 0.0);
        }
    }
};

static EngineTests engineTests;
