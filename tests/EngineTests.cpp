#include "TestHelpers.h"
#include "engine/Models.h"

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
        beginTest ("C++ and Rust agree on the shared parameter layout");
        {
            const char* mismatch = ld::verifyLayout();
            expect (mismatch == nullptr, mismatch != nullptr ? juce::String (mismatch) : juce::String());
        }

        beginTest ("The console has the WING layout");
        {
            MixerEngine engine (0);
            engine.setNumChannels (kWingChannels);
            expectEquals ((int) engine.getChannels().size(), 40);
            expectEquals ((int) engine.getAux().size(), 8);
            expectEquals ((int) engine.getBuses().size(), 16);
            expectEquals ((int) engine.getMains().size(), 4);
            expectEquals ((int) engine.getMatrices().size(), 8);
            expectEquals (engine.getChannels()[39]->getLabel(), juce::String ("CH 40"));
            expectEquals (engine.getBuses()[15]->getLabel(), juce::String ("BUS 16"));
            expectEquals (engine.getMain().getLabel(), juce::String ("MAIN 1"));
            expect (engine.getChannels()[0]->params.mains[0].on.load(), "channels feed Main 1");
            expect (! engine.getBuses()[0]->params.mains[0].on.load(), "buses start as aux mixes");
            expect (engine.getChannels()[0]->getColour() == wingColour (1));
            expectEquals (wingColourName (9), juce::String ("Red"));

            // Wrappers survive structure changes; numbering follows position.
            auto* ch5 = engine.getChannels()[4].get();
            engine.removeStrip (engine.getChannels()[0].get());
            expect (engine.getChannels()[3].get() == ch5);
            expectEquals (ch5->getNumber(), 4);
            expectEquals (ch5->getId(), 5);
            engine.getGarbage().collectAllNow();
        }

        beginTest ("Model tables describe the processing for the UI");
        {
            expectEquals ((int) models::table (models::Dynamics).size(), 32);
            expectEquals ((int) models::table (models::ChannelEq).size(), 7);
            const int comp = models::findModel (models::Dynamics, "COMP");
            expect (comp >= 0);
            const auto& m = models::model (models::Dynamics, comp);
            expectEquals (m.name, juce::String ("Wing Compressor"));
            const int ratio = m.find ("ratio");
            expect (ratio >= 0 && m.params[(size_t) ratio].kind == models::ParamKind::Values);
            expectEquals (m.params[(size_t) m.find ("thr")].display (-20.0f), juce::String ("-20.0 dB"));
            expectEquals (models::list (models::TapPoints)[5], juce::String ("PFL"));
            expectEquals (models::list (models::ProcessOrders).size(), 24);

            MixerEngine engine (0);
            engine.setNumChannels (1);
            auto& ch = *engine.getChannels()[0];
            const int la = models::findModel (models::Dynamics, "LA");
            ch.setModel (ModelBlock::Dynamics, la);
            expectEquals ((int) ch.params.dynamics.model.load(), la);
            const auto& laModel = models::model (models::Dynamics, la);
            expectEquals (ch.params.dynamics.values[laModel.find ("peak")].load(), laModel.params[(size_t) laModel.find ("peak")].defaultValue);

            double freqs[] = { 100.0, 1000.0 }, db[2] = { 1.0, 1.0 };
            ld::ld_strip_response (ch.getHandle(), 1, freqs, db, 2, 48000.0);
            expectWithinAbsoluteError (db[1], 0.0, 0.01);
        }

        beginTest ("The compressor's curve, meters and gain agree");
        {
            const int comp = models::findModel (models::Dynamics, "COMP");
            const auto& m = models::model (models::Dynamics, comp);
            std::vector<float> values (ld::kMaxDynParams, 0.0f);
            for (size_t i = 0; i < m.params.size(); ++i)
                values[i] = m.params[i].defaultValue;
            values[(size_t) m.find ("knee")] = 0.0f;
            const float in[] = { -40.0f, -6.0f };
            float out[] = { 0.0f, 0.0f };
            expect (models::dynStaticCurve (comp, values.data(), (int) values.size(), in, out, 2));
            expectWithinAbsoluteError (out[0], -40.0f, 1.0e-4f);
            expectWithinAbsoluteError (out[1], -20.0f + 14.0f / 3.0f, 1.0e-3f); // THR -20, 3:1
            // A level-free model has no curve.
            expect (! models::dynStaticCurve (models::findModel (models::Dynamics, "WAVE"), values.data(), (int) values.size(), in, out, 2));

            MixerEngine engine (0);
            OfflineRig rig (engine, 2, 2);
            engine.setNumChannels (1);
            auto& ch = *engine.getChannels()[0];
            ch.setModel (ModelBlock::Dynamics, comp);
            ch.params.dynamics.values[m.find ("knee")] = 0.0f;
            ch.params.dynamics.on = true;
            ch.params.pan = -100.0f;
            rig.setInput (0, 0.5f); // -6 dB: 14 dB over, 9.3 dB of reduction
            rig.run (200);
            const float gr = ch.meters.dynReductionDb.load();
            expectWithinAbsoluteError (gr, 14.0f * 2.0f / 3.0f, 0.2f);
            expectWithinAbsoluteError (ch.meters.dynKeyDb.load(), -6.02f, 0.2f);
            expectWithinAbsoluteError (20.0f * std::log10 (rig.out (0) / 0.5f), -gr, 0.2f);
        }

        beginTest ("A channel reaches Main 1 through the pan law");
        {
            MixerEngine engine (2);
            OfflineRig rig (engine, 4, 8);
            engine.setNumChannels (2);
            rig.setInput (0, 0.5f);
            rig.run();

            expectWithinAbsoluteError (rig.out (0), 0.5f * kCentre, 1.0e-5f);
            expectWithinAbsoluteError (rig.out (1), 0.5f * kCentre, 1.0e-5f);

            engine.getChannels()[0]->params.pan = -100.0f;
            rig.run();
            expectWithinAbsoluteError (rig.out (0), 0.5f, 1.0e-5f);
            expectWithinAbsoluteError (rig.out (1), 0.0f, 1.0e-5f);
        }

        beginTest ("Mute, fader, DCAs, mute groups and main sends");
        {
            MixerEngine engine (2);
            OfflineRig rig (engine, 2, 2);
            engine.setNumChannels (1);
            auto& p = engine.getChannels()[0]->params;
            rig.setInput (0, 0.5f);

            p.faderDb = -6.0206f;
            rig.run();
            expectWithinAbsoluteError (rig.out (0), 0.25f * kCentre, 1.0e-4f);

            p.dcaMask = 1u << 2;
            engine.getDca (2).faderDb = -6.0206f;
            rig.run();
            expectWithinAbsoluteError (rig.out (0), 0.125f * kCentre, 1.0e-4f);
            engine.getDca (2).mute = true;
            rig.run();
            expectWithinAbsoluteError (rig.out (0), 0.0f, 1.0e-6f);
            engine.getDca (2).mute = false;

            p.muteGroupMask = 1u << 1;
            engine.muteGroups = 1u << 1;
            rig.run();
            expectWithinAbsoluteError (rig.out (0), 0.0f, 1.0e-6f);
            engine.muteGroups = 0u;

            p.mute = true;
            rig.run();
            expectWithinAbsoluteError (rig.out (0), 0.0f, 1.0e-6f);

            p.mute = false;
            p.mains[0].on = false;
            rig.run();
            expectWithinAbsoluteError (rig.out (0), 0.0f, 1.0e-6f);
        }

        beginTest ("Stereo channel uses both inputs and its width");
        {
            MixerEngine engine (1);
            OfflineRig rig (engine, 2, 2);
            engine.setNumChannels (1);
            auto& p = engine.getChannels()[0]->params;
            p.stereo = true;
            p.inputLeft = 0;
            p.inputRight = 1;
            rig.setInput (0, 0.25f);
            rig.setInput (1, -0.5f);
            rig.run();
            expectWithinAbsoluteError (rig.out (0), 0.25f, 1.0e-5f);
            expectWithinAbsoluteError (rig.out (1), -0.5f, 1.0e-5f);

            p.width = 0.0f;
            rig.run();
            expectWithinAbsoluteError (rig.out (0), -0.125f, 1.0e-5f);
        }

        beginTest ("Bus sends: PRE, POST and GRP");
        {
            MixerEngine engine (2);
            OfflineRig rig (engine, 2, 6);
            engine.setNumChannels (1);
            auto& ch = *engine.getChannels()[0];
            auto& bus = *engine.getBusInSlot (2);
            bus.params.outputLeft = 2;
            bus.params.outputRight = 3;
            auto& send = ch.sends[2];
            send.enabled = true;

            rig.setInput (0, 0.5f);
            ch.params.faderDb = -100.0f; // PRE: the fader doesn't matter
            rig.run();
            expectWithinAbsoluteError (rig.out (2), 0.5f * kCentre, 1.0e-5f);

            send.mode = (int32_t) SendMode::Post;
            rig.run();
            expectWithinAbsoluteError (rig.out (2), 0.0f, 1.0e-4f);

            ch.params.faderDb = 0.0f;
            send.mode = (int32_t) SendMode::Group;
            ch.params.mains[0].on = false;
            bus.params.mains[0].on = true;
            rig.run();
            expectWithinAbsoluteError (rig.out (0), 0.5f * kCentre, 1.0e-5f);
        }

        beginTest ("Matrix: mains plus a mix-minus direct input");
        {
            MixerEngine engine (2);
            OfflineRig rig (engine, 2, 6);
            engine.setNumChannels (2);
            engine.getChannels()[1]->params.inputLeft = 1;
            rig.setInput (0, 0.5f);
            rig.setInput (1, 0.25f);
            engine.getMain().params.matrix[0].on = true;
            auto& mtx = *engine.getMatrices()[0];
            mtx.params.outputLeft = 4;
            rig.run();
            expectWithinAbsoluteError (rig.out (4), 0.75f * kCentre, 1.0e-4f);

            mtx.params.directOn = true;
            mtx.params.directSource = 1;
            mtx.params.directInvert = true;
            rig.run();
            expectWithinAbsoluteError (rig.out (4), 0.5f * kCentre, 1.0e-4f);
        }

        beginTest ("Buses beyond the WING's 16");
        {
            MixerEngine engine (2);
            OfflineRig rig (engine, 1, 4);
            engine.setNumChannels (1);

            Strip* last = nullptr;
            for (int i = 0; i < 100; ++i)
                last = engine.addBus();

            expect (last != nullptr);
            expectEquals ((int) engine.getBuses().size(), 116);

            last->params.outputLeft = 3;
            auto& send = engine.getChannels()[0]->sends[(size_t) last->getId()];
            send.enabled = true;

            rig.setInput (0, 0.5f);
            rig.run();
            expectWithinAbsoluteError (rig.out (3), 0.5f * kCentre, 1.0e-5f);

            // Removing a bus frees its slot for reuse, with the old send cleared.
            const int slot = last->getId();
            engine.removeStrip (last);
            auto* reused = engine.addBus();
            expectEquals (reused->getId(), slot);
            expect (! engine.getChannels()[0]->sends[(size_t) slot].enabled.load());

            // The console's 16 can't be removed.
            engine.removeStrip (engine.getBuses()[0].get());
            expectEquals ((int) engine.getBuses().size(), 116);
            engine.getGarbage().collectAllNow();
        }

        beginTest ("PFL solo replaces Main 1 on the monitor outputs");
        {
            MixerEngine engine (2);
            OfflineRig rig (engine, 2, 4);
            engine.setNumChannels (2);
            engine.getChannels()[1]->params.inputLeft = 1;
            engine.monitorOutputLeft = 2;
            engine.monitorOutputRight = 3;
            rig.setInput (0, 0.5f);
            rig.setInput (1, 0.25f);

            rig.run();
            expectWithinAbsoluteError (rig.out (2), 0.75f * kCentre, 1.0e-4f);

            engine.getChannels()[1]->params.solo = true;
            engine.getChannels()[1]->params.faderDb = -100.0f;
            rig.run();
            expect (engine.anySoloActive.load());
            expectWithinAbsoluteError (rig.out (2), 0.25f, 1.0e-4f);
            expectWithinAbsoluteError (rig.out (0), 0.5f * kCentre, 1.0e-4f);
        }

        beginTest ("Solo is PFL or AFL per strip type; DIM and MONO act on the monitor only");
        {
            MixerEngine engine (0);
            OfflineRig rig (engine, 1, 4);
            engine.setNumChannels (1);
            engine.monitorOutputLeft = 2;
            engine.monitorOutputRight = 3;
            expect (! engine.isSoloAfl (StripKind::Channel) && engine.isSoloAfl (StripKind::Bus));
            expect (engine.isSoloAfl (StripKind::Main) && engine.isSoloAfl (StripKind::Matrix));

            auto& ch = *engine.getChannels()[0];
            ch.params.pan = -100.0f;
            ch.sends[3].enabled = true;
            auto& bus = *engine.getBusInSlot (3);
            bus.params.faderDb = -100.0f;
            bus.params.solo = true;
            rig.setInput (0, 0.5f);
            rig.run();
            expectWithinAbsoluteError (rig.out (2), 0.0f, 1.0e-6f); // AFL: the bus fader is down

            engine.setSoloAfl (StripKind::Bus, false);
            expect (! engine.busSoloAfl.load());
            rig.run();
            expectWithinAbsoluteError (rig.out (2), 0.5f, 1.0e-5f); // PFL, panned hard left
            bus.params.solo = false;
            rig.run();

            engine.monitorDim = true; // default 20 dB
            rig.run();
            expectWithinAbsoluteError (rig.out (2), 0.05f, 1.0e-5f);
            expectEquals (engine.monitorLevelDb.load(), 0.0f);
            engine.monitorDimDb = 6.0206f;
            rig.run();
            expectWithinAbsoluteError (rig.out (2), 0.25f, 1.0e-4f);
            engine.monitorDim = false;
            engine.monitorMono = true;
            rig.run();
            expectWithinAbsoluteError (rig.out (2), 0.25f, 1.0e-5f);
            expectWithinAbsoluteError (rig.out (3), 0.25f, 1.0e-5f);
            engine.monitorMono = false;
            engine.monitorSwap = true;
            rig.run();
            expectWithinAbsoluteError (rig.out (3), 0.5f, 1.0e-5f);
            engine.monitorMute = true;
            rig.run();
            expectWithinAbsoluteError (rig.out (3), 0.0f, 1.0e-7f);
            expectWithinAbsoluteError (rig.out (0), 0.5f, 1.0e-5f); // Main 1 untouched
        }

        beginTest ("WING strip types: fixed taps, no aux INS 2, post-fader bus EQ, meters");
        {
            MixerEngine engine (0);
            OfflineRig rig (engine, 1, 4);
            engine.setNumChannels (1);
            auto& ch = *engine.getChannels()[0];
            auto& aux = *engine.getAux()[0];
            auto& bus = *engine.getBuses()[0];
            expect (ch.hasSelectableTap() && ch.hasAutomix() && ch.hasPostInsert() && ch.hasPostFaderDelay());
            expect (! aux.hasSelectableTap() && ! aux.hasAutomix() && ! aux.hasPostInsert() && ! aux.hasPostFaderDelay());
            expect (! bus.hasSelectableTap() && bus.hasPostInsert() && bus.hasPostFaderEq() && ! ch.hasPostFaderEq());
            expect (aux.canIgnoreMuteOnSends() && ! bus.canIgnoreMuteOnSends());

            // Automix membership: AUTO X / AUTO Y with INS 2 ON, input channels only.
            expectEquals (ch.automixGroup(), -1);
            ch.params.postInsert.mode = (int) PostInsertMode::AutomixY;
            expectEquals (ch.automixGroup(), -1, "INS 2 off: not a member");
            ch.params.postInsert.on = true;
            expectEquals (ch.automixGroup(), 1);
            ch.params.postInsert.mode = (int) PostInsertMode::AutomixX;
            expectEquals (ch.automixGroup(), 0);
            bus.params.postInsert.mode = (int) PostInsertMode::AutomixX;
            bus.params.postInsert.on = true;
            expectEquals (bus.automixGroup(), -1);
            for (auto* s : { &ch, &bus })
            {
                s->params.postInsert.on = false;
                s->params.postInsert.mode = 0;
            }

            rig.setInput (0, 0.25f);
            ch.params.input.trimDb = 6.0206f;
            ch.params.faderDb = -100.0f;
            rig.run();
            for (auto p : { MeterPoint::Input, MeterPoint::Trim, MeterPoint::PreFader, MeterPoint::Output })
                ch.peakMeter (p, 0).store (0.0f);
            rig.run (2);
            expectWithinAbsoluteError (ch.peakMeter (MeterPoint::Input, 0).load(), 0.25f, 1.0e-5f);
            expectWithinAbsoluteError (ch.peakMeter (MeterPoint::Trim, 0).load(), 0.5f, 1.0e-4f);
            expectWithinAbsoluteError (ch.peakMeter (MeterPoint::PreFader, 0).load(), 0.5f, 1.0e-4f);
            expectWithinAbsoluteError (ch.peakMeter (MeterPoint::Output, 0).load(), 0.0f, 1.0e-6f);
            expect (&ch.peakMeter (MeterPoint::Trim, 1) == &ch.meters.trimPeak[1]);
        }

        beginTest ("TAP sends follow the channel mute unless IGNORE CHANNEL MUTE");
        {
            MixerEngine engine (0);
            OfflineRig rig (engine, 1, 4);
            engine.setNumChannels (1);
            auto& ch = *engine.getChannels()[0];
            auto& send = ch.sends[2];
            send.enabled = true;
            engine.getBusInSlot (2)->params.outputLeft = 2;
            rig.setInput (0, 0.5f);
            rig.run();
            expectWithinAbsoluteError (rig.out (2), 0.5f * kCentre, 1.0e-5f);
            ch.params.mute = true;
            rig.run();
            expectWithinAbsoluteError (rig.out (2), 0.0f, 1.0e-6f);
            send.ignoreMute = true;
            rig.run();
            expectWithinAbsoluteError (rig.out (2), 0.5f * kCentre, 1.0e-5f);

            // GROUP mode has no send level.
            ch.params.mute = false;
            send.mode = (int32_t) SendMode::Group;
            send.levelDb = -100.0f;
            ch.params.mains[0].on = false;
            rig.run();
            expectWithinAbsoluteError (rig.out (2), 0.5f * kCentre, 1.0e-5f);
        }

        beginTest ("Preferences: DCA MUTEGROUPS, MAIN LINK, STARTUP MAIN MUTE, automix enables");
        {
            MixerEngine engine (0);
            OfflineRig rig (engine, 1, 4);
            engine.setNumChannels (1);
            auto& ch = *engine.getChannels()[0];
            rig.setInput (0, 0.5f);
            expect (engine.dcaMuteGroups.load() && engine.automixEnable[0].load() && engine.automixEnable[1].load());
            ch.params.dcaMask = 1u;
            engine.getDca (0).mute = true;
            rig.run();
            expectWithinAbsoluteError (rig.out (0), 0.0f, 1.0e-6f);
            engine.dcaMuteGroups = false;
            rig.run();
            expectWithinAbsoluteError (rig.out (0), 0.5f * kCentre, 1.0e-5f);

            ch.params.mains[1].on = true;
            engine.getMain (1).params.outputLeft = 2;
            engine.getMain().params.faderDb = -6.0206f;
            engine.mainLink = (int32_t) MainLink::To2;
            rig.run();
            expectWithinAbsoluteError (rig.out (2), 0.25f * kCentre, 1.0e-4f);
            expectEquals (models::list (models::MainLinkModes)[3], juce::String ("1 > 2-4"));

            expect (! engine.applyStartupMainMute());
            engine.startupMainMute = true;
            expect (engine.applyStartupMainMute());
            for (auto& m : engine.getMains())
                expect (m->params.mute.load());

            engine.reset();
            expect (! engine.startupMainMute.load() && engine.dcaMuteGroups.load());
            expectEquals (engine.mainLink.load(), (int32_t) MainLink::Off);
        }

        beginTest ("Unpatched channels are skipped and resume cleanly");
        {
            MixerEngine engine (-1);
            constexpr int numCh = 4096;
            OfflineRig rig (engine, 8, 2); // a typical interface: far fewer inputs than channels
            engine.setNumChannels (numCh);
            for (auto& ch : engine.getChannels())
            {
                ch->params.gate.on = true;
                ch->params.dynamics.on = true;
            }

            for (int i = 0; i < 8; ++i)
                rig.setInput (i, 0.01f);

            rig.run (10);
            const auto start = juce::Time::getMillisecondCounterHiRes();
            rig.run (200);
            const double msPerBlock = (juce::Time::getMillisecondCounterHiRes() - start) / 200.0;
            logMessage ("4096 channels, 8 patched: " + juce::String (msPerBlock, 3) + " ms per "
                        + juce::String (1000.0 * OfflineRig::kBlock / 48000.0, 2) + " ms block");

            auto& ch = *engine.getChannels()[100];
            ch.params.gate.on = false;
            ch.params.dynamics.on = false;
            ch.params.inputLeft = 0;
            for (int i = 0; i < 8; ++i)
                engine.getChannels()[(size_t) i]->params.mute = true;
            rig.run();
            expectWithinAbsoluteError (rig.out (0), 0.01f * kCentre, 1.0e-5f);
        }

        beginTest ("Many channels processed in parallel sum correctly");
        {
            MixerEngine engine (3);
            constexpr int numCh = 256;
            OfflineRig rig (engine, numCh, 2);
            engine.setNumChannels (numCh);
            for (int i = 0; i < numCh; ++i)
                rig.setInput (i, 0.001f);
            rig.run();
            expectWithinAbsoluteError (rig.out (0), numCh * 0.001f * kCentre, 1.0e-4f);

            engine.setNumChannels (16);
            rig.run();
            expectWithinAbsoluteError (rig.out (0), 16 * 0.001f * kCentre, 1.0e-5f);
            expectEquals ((int) engine.getChannels().size(), 16);
        }

        beginTest ("Hardware channel maps skip unavailable inputs");
        {
            MixerEngine engine (0);
            OfflineRig rig (engine, 2, 2);
            engine.setNumChannels (3); // channel 3 has no hardware input
            expectEquals (engine.getChannels()[2]->params.inputLeft.load(), -1);
            rig.setInput (0, 0.1f);
            rig.setInput (1, 0.1f);
            rig.run();
            expectWithinAbsoluteError (rig.out (0), 0.2f * kCentre, 1.0e-5f);
        }

        beginTest ("Throughput (informational)");
        {
            // A full WING: 40 channels with gate, EQ and dynamics, 16 bus sends each, 48 kHz / 128 samples.
            MixerEngine engine (-1);
            constexpr int numCh = kWingChannels;
            OfflineRig rig (engine, numCh, 16);
            engine.setNumChannels (numCh);

            juce::Random rng (1);
            for (int i = 0; i < numCh; ++i)
            {
                auto& ch = *engine.getChannels()[(size_t) i];
                ch.params.gate.on = true;
                ch.params.dynamics.on = true;
                ch.params.filter.lowCut = true;
                ch.params.eq.values[4] = 3.0f;
                for (int b = 0; b < kWingBuses; ++b)
                    ch.sends[(size_t) b].enabled = true;
                for (auto& v : rig.inputs[(size_t) i])
                    v = rng.nextFloat() * 0.2f - 0.1f;
            }
            for (auto& b : engine.getBuses())
                b->params.dynamics.on = true;

            rig.run (10);
            const auto start = juce::Time::getMillisecondCounterHiRes();
            constexpr int blocks = 1000;
            rig.run (blocks);
            const double msPerBlock = (juce::Time::getMillisecondCounterHiRes() - start) / blocks;
            const double budget = 1000.0 * OfflineRig::kBlock / 48000.0;
            logMessage ("40 ch + 16 bus: " + juce::String (msPerBlock, 3) + " ms per block of "
                        + juce::String (budget, 2) + " ms (" + juce::String (100.0 * msPerBlock / budget, 1)
                        + "% of the realtime budget, " + juce::String (engine.getNumWorkerThreads() + 1) + " threads)");
            expect (msPerBlock > 0.0);
        }
    }
};

static EngineTests engineTests;

//==============================================================================
/** The analysis facade (RTA, LR vector scope, LISTEN) and TAP WIDTH / TAP EQ
    through the C++ views. */
class AnalysisTests final : public juce::UnitTest
{
public:
    AnalysisTests() : juce::UnitTest ("Analysis", "LiveDAW") {}

    void runTest() override
    {
        beginTest ("The RTA settings, the analysed strip and the bands");
        {
            MixerEngine engine (0);
            engine.setNumChannels (2);
            auto analysis = engine.getAnalysis();
            auto& ch = *engine.getChannels()[1];
            expectEquals ((int) analysis.controls.stripKind.load(), -1);
            analysis.showRta (&ch, ld::kRtaViewEq);
            expect (analysis.isAnalysed (ch) && ! analysis.isAnalysed (*engine.getChannels()[0]));
            expect (analysis.controls.rtaOn.load());
            expectEquals ((int) analysis.settings (ld::kRtaViewHome).display.load(), 2); // FULL
            expectEquals ((int) analysis.settings (ld::kRtaViewEq).display.load(), 1);   // 1/4
            expect (analysis.settings (ld::kRtaViewEq).autogain.load());
            expectWithinAbsoluteError (Analysis::bandHz (68), 1046.5f, 0.1f); // C6
            expect (Analysis::bandLowHz (57) < Analysis::bandHz (57) && Analysis::bandHighHz (56) < Analysis::bandHz (57));
            Analysis::Bands bands;
            analysis.read (bands);
            expect (bands.levels[0] <= -100.0f);
            analysis.setFrozen (true);
            expect (Analysis (ch).isFrozen());
            analysis.hideRta();
            expect (! analysis.controls.rtaOn.load());
        }

        beginTest ("LISTEN replaces the solo bus: on, follow, off");
        {
            MixerEngine engine (0);
            OfflineRig rig (engine, 2, 4);
            engine.setNumChannels (2);
            engine.getChannels()[1]->params.inputLeft = 1;
            engine.monitorOutputLeft = 2;
            engine.monitorOutputRight = 3;
            rig.setInput (0, 0.5f);
            rig.setInput (1, 0.25f);
            auto& a = *engine.getChannels()[0];
            auto& b = *engine.getChannels()[1];
            b.params.solo = true; // PFL: 0.25 on the monitor
            rig.run();
            expectWithinAbsoluteError (rig.out (2), 0.25f, 1.0e-4f);

            // KEY SOLO of channel 1's DYNAMICS slot (no key filter): its own signal.
            Analysis analysis (a);
            expect (analysis.toggleListen (a, ld::kListenDynKey));
            expect (analysis.isListening (a, ld::kListenDynKey) && ! analysis.isListening (b, ld::kListenDynKey));
            rig.run();
            expect (engine.anySoloActive.load());
            expectWithinAbsoluteError (rig.out (2), 0.5f, 1.0e-4f);
            expectWithinAbsoluteError (rig.out (3), 0.5f, 1.0e-4f);

            // BAND SOLO follows the selected band only while it plays.
            analysis.followBand (a, 3);
            expectEquals ((int) analysis.controls.listenBand.load(), 0);
            analysis.listen (a, ld::kListenBand, 1);
            analysis.followBand (a, 3);
            expectEquals ((int) analysis.controls.listenBand.load(), 3);

            expect (! analysis.toggleListen (a, ld::kListenBand));
            expect (! analysis.isListening());
            rig.run();
            expectWithinAbsoluteError (rig.out (2), 0.25f, 1.0e-4f);
        }

        beginTest ("The scope takes L/R after the INPUT block");
        {
            MixerEngine engine (0);
            OfflineRig rig (engine, 2, 2);
            engine.setNumChannels (1);
            auto& ch = *engine.getChannels()[0];
            ch.params.stereo = true;
            ch.params.inputRight = 1;
            ch.params.input.trimDb = -6.0206f;
            rig.setInput (0, 0.8f);
            rig.setInput (1, -0.4f);
            auto analysis = engine.getAnalysis();
            analysis.showScope (&ch);
            rig.run (10);
            std::vector<float> lr (2 * 256);
            const int n = analysis.readScope (lr.data(), 256);
            expectEquals (n, 256);
            expectWithinAbsoluteError (lr[(size_t) (2 * n - 2)], 0.4f, 1.0e-4f);
            expectWithinAbsoluteError (lr[(size_t) (2 * n - 1)], -0.2f, 1.0e-4f);
            analysis.hideScope();
        }

        beginTest ("TAP WIDTH and TAP EQ shape the TAP sends");
        {
            MixerEngine engine (0);
            OfflineRig rig (engine, 2, 6);
            engine.setNumChannels (1);
            auto& ch = *engine.getChannels()[0];
            ch.params.stereo = true;
            ch.params.inputRight = 1;
            ch.params.mains[0].on = false;
            rig.setInput (0, 0.4f);
            auto& bus = *engine.getBusInSlot (0);
            bus.params.outputLeft = 4;
            bus.params.outputRight = 5;
            ch.sends[0].enabled = true; // TAP (PRE) by default
            expectEquals (ch.params.tapWidth.load(), 100.0f);
            rig.run();
            expectWithinAbsoluteError (rig.out (4), 0.4f, 1.0e-4f);
            expectWithinAbsoluteError (rig.out (5), 0.0f, 1.0e-4f);
            ch.params.tapWidth = 0.0f;
            rig.run();
            expectWithinAbsoluteError (rig.out (4), 0.2f, 1.0e-4f);
            expectWithinAbsoluteError (rig.out (5), 0.2f, 1.0e-4f);
            ch.params.tapWidth = 100.0f;

            ch.params.tapEq.on = true;
            ch.params.tapEq.bands[0].gainDb = 6.0f;
            rig.run (60);
            expectWithinAbsoluteError (rig.out (4), 0.4f * 1.9953f, 1.0e-2f);
            double freqs[] = { 30.0, 5000.0 }, db[2] = {};
            ld::ld_strip_response (ch.getHandle(), 4, freqs, db, 2, 48000.0);
            expectWithinAbsoluteError (db[0], 6.0, 0.5);
            expectWithinAbsoluteError (db[1], 0.0, 0.2);
        }
    }
};

static AnalysisTests analysisTests;
