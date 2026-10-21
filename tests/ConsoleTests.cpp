#include "TestHelpers.h"
#include "engine/Show.h"
#include "ui/ConsoleLook.h"

using namespace livedaw;
using livedaw::test::OfflineRig;

/** The console-level features through the C++ facade: SOLO MODE, MUTEGROUP/SIP
    OVERRIDE, the talk keys, the PHONES path, MON DCAs and sources; and the
    console look (the surface finish) kept in the app settings. */
class ConsoleTests final : public juce::UnitTest
{
public:
    ConsoleTests() : juce::UnitTest ("Console level", "LiveDAW") {}

    void runTest() override
    {
        beginTest ("SIP mutes the unsoloed inputs; MUTE flashes and OVERRIDE unmutes");
        {
            MixerEngine engine (0);
            OfflineRig rig (engine, 2, 2);
            test::setChannels (engine, 2);
            auto& a = *engine.getChannels()[0];
            auto& b = *engine.getChannels()[1];
            b.params.inputLeft = 1;
            a.params.pan = -100.0f;
            b.params.pan = -100.0f;
            rig.setInput (0, 0.5f);
            rig.setInput (1, 0.25f);
            expectEquals ((int) engine.soloMode.load(), (int) ld::kSoloStudio);

            engine.soloMode = ld::kSoloSip;
            a.params.solo = true;
            rig.run (4);
            expect (engine.sipActive.load());
            expectEquals (b.getMuteState(), (int) ld::kMuteGroup);
            expectWithinAbsoluteError (rig.out (0), 0.5f, 1.0e-5f);

            // Without OVERRIDE, MUTE mutes the strip itself; with it, MUTE unmutes.
            engine.pressMute (b);
            expect (b.params.mute.load());
            engine.pressMute (b);
            engine.muteOverride = true;
            rig.run (2);
            engine.pressMute (b);
            rig.run (2);
            expectEquals (b.getMuteState(), (int) ld::kMuteOverridden);
            expectWithinAbsoluteError (rig.out (0), 0.75f, 1.0e-5f);

            a.params.solo = false;
            rig.run (2);
            expect (! engine.sipActive.load());
            expectEquals (b.getMuteState(), (int) ld::kMuteOff);
        }

        beginTest ("Talk keys: PUSH, LATCH and AUTO; talk reaches its buses");
        {
            MixerEngine engine (0);
            OfflineRig rig (engine, 2, 4);
            test::setChannels (engine, 40);
            auto* talkCh = engine.getChannels()[39].get();
            talkCh->params.inputLeft = 1;
            talkCh->params.mute = true;
            rig.setInput (1, 0.5f);
            expect (engine.getTalkStrip() == nullptr);
            engine.setTalkStrip (talkCh);
            expect (engine.getTalkStrip() == talkCh);
            expectEquals ((int) engine.talk.assignIndex.load(), 39);

            auto& pathA = engine.talk.paths[0];
            pathA.buses = 1u << 2; // BUS 3
            auto& bus3 = *engine.getBusInSlot (2);
            bus3.params.outputLeft = 2;
            rig.run (2);
            expectWithinAbsoluteError (rig.out (2), 0.0f, 1.0e-7f);

            pathA.mode = ld::kTalkPush;
            engine.talkKey (0, true);
            expect (engine.isTalking());
            rig.run (2);
            expectWithinAbsoluteError (rig.out (2), 0.5f, 1.0e-5f);
            engine.talkKey (0, false);
            expect (! engine.isTalkOn (0));

            pathA.mode = ld::kTalkLatch;
            engine.talkKey (0, true);
            engine.talkKey (0, false);
            expect (engine.isTalkOn (0));
            engine.talkKey (0, true);
            expect (! engine.isTalkOn (0));
            engine.talkKey (0, false);

            // AUTO: a short press latches, the next press ends it; held, it's a push.
            pathA.mode = ld::kTalkAuto;
            engine.talkKey (0, true);
            engine.talkKey (0, false);
            expect (engine.isTalkOn (0));
            engine.talkKey (0, true);
            engine.talkKey (0, false);
            expect (! engine.isTalkOn (0));
            engine.talkKey (0, true);
            juce::Thread::sleep ((int) MixerEngine::kTalkHoldMs + 50);
            engine.talkKey (0, false);
            expect (! engine.isTalkOn (0));

            engine.setTalkStrip (nullptr);
            expectEquals ((int) engine.talk.assignKind.load(), (int) ld::kTalkOff);
        }

        beginTest ("The PHONES path, the solo bus and MON DCAs");
        {
            MixerEngine engine (0);
            OfflineRig rig (engine, 1, 6);
            test::setChannels (engine, 1);
            auto& ch = *engine.getChannels()[0];
            ch.params.pan = -100.0f;
            rig.setInput (0, 0.5f);
            engine.monitorOutputLeft = 2;
            engine.phones.outputLeft = 4;
            rig.run (4);
            expectWithinAbsoluteError (rig.out (4), 0.5f, 1.0e-5f);

            engine.setSoloBus (1); // B: SPEAKERS
            ch.params.solo = true;
            ch.params.faderDb = -100.0f;
            rig.run (4);
            expectWithinAbsoluteError (rig.out (2), 0.5f, 1.0e-5f); // PFL
            expectWithinAbsoluteError (rig.out (4), 0.0f, 1.0e-6f); // Main 1, fader down
            ch.params.solo = false;
            ch.params.faderDb = 0.0f;

            engine.getDca (0).faderDb = -6.0206f;
            engine.setDcaName (0, "MON A"); // the phones
            expectEquals ((int) engine.getDcaMonitorPaths (0), 2);
            rig.run (4);
            expectWithinAbsoluteError (rig.out (2), 0.5f, 1.0e-5f);
            expectWithinAbsoluteError (rig.out (4), 0.25f, 1.0e-4f);

            expectEquals (MixerEngine::monitorSourceName (0), juce::String ("MAIN.1"));
            expectEquals (MixerEngine::monitorSourceName (4 + 8 + 15), juce::String ("BUS.16"));
            expectEquals (MixerEngine::monitorSourceName (ld::kNumMonitorSources - 1), juce::String ("AUX.8"));
        }

        beginTest ("Sources process the ports, pair, and survive a show");
        {
            MixerEngine engine (0);
            PluginHost host (nullptr);
            Show show (engine, host);
            OfflineRig rig (engine, 2, 2);
            test::setChannels (engine, 1);
            auto& ch = *engine.getChannels()[0];
            ch.params.pan = -100.0f;
            rig.setInput (0, 0.25f);
            expectEquals (engine.getNumSources(), ld::kNumSources);
            const int index = MixerEngine::sourceIndex (0);
            auto* src = engine.getSource (index);
            expect (src != nullptr);
            if (src == nullptr)
                return;
            src->gainDb = 6.0206f;
            src->invert = true;
            rig.run (4);
            expectWithinAbsoluteError (rig.out (0), -0.5f, 1.0e-4f);
            engine.setSourceName (index, "Kick In");
            engine.setSourceMode (index, ld::kSourceStereo);
            expectEquals ((int) engine.getSource (1)->mode.load(), (int) ld::kSourceStereo);
            expectEquals (engine.getSource (1)->gainDb.load(), 6.0206f);
            ch.setTalkTags (ld::kTalkAOn | ld::kTalkBSolo);

            auto xml = show.toXml();
            engine.initializeSource (index);
            expect (engine.getSourceName (index).isEmpty());
            expectEquals (engine.getSource (0)->gainDb.load(), 0.0f);

            MixerEngine restoredEngine (0);
            Show restored (restoredEngine, host);
            const auto result = restored.loadFromXml (*xml);
            expect (result.wasOk(), result.getErrorMessage());
            expectEquals (restoredEngine.getSourceName (index), juce::String ("Kick In"));
            expectEquals (restoredEngine.getSource (1)->gainDb.load(), 6.0206f);
            expect (restoredEngine.getSource (0)->invert.load());
            expectEquals ((int) restoredEngine.getChannels()[0]->getTalkTags(), (int) (ld::kTalkAOn | ld::kTalkBSolo));
            expectEquals (MixerEngine::sourcePort (MixerEngine::sourceIndex (ld::kPlaybackSourceBase + 3)), ld::kPlaybackSourceBase + 3);
        }

        beginTest ("Oscillator: the OSC sources play on a channel and are saved with the show");
        {
            MixerEngine engine (0);
            PluginHost host (nullptr);
            Show show (engine, host);
            OfflineRig rig (engine, 2, 2);
            test::setChannels (engine, 1);
            auto& ch = *engine.getChannels()[0];
            ch.params.pan = -100.0f;
            ch.params.inputLeft = MixerEngine::oscSource (1);
            expectEquals (MixerEngine::sourceIndex (MixerEngine::oscSource (1)), ld::kNumSources - 1);
            auto* osc = engine.getOscillator (1);
            expect (osc != nullptr && engine.getOscillator (2) == nullptr);
            if (osc == nullptr)
                return;
            osc->levelDb = -12.0f;
            osc->freqHz = 250.0f;
            rig.run (40);
            float peak = 0.0f;
            for (int b = 0; b < 40; ++b)
            {
                rig.run (1);
                for (float v : rig.outputs[0])
                    peak = juce::jmax (peak, std::abs (v));
            }
            expectWithinAbsoluteError (peak, juce::Decibels::decibelsToGain (-12.0f), 0.005f);

            auto xml = show.toXml();
            MixerEngine restoredEngine (0);
            Show restored (restoredEngine, host);
            const auto result = restored.loadFromXml (*xml);
            expect (result.wasOk(), result.getErrorMessage());
            expectEquals (restoredEngine.getOscillator (1)->freqHz.load(), 250.0f);
            expectEquals (restoredEngine.getOscillator (1)->levelDb.load(), -12.0f);
            expectEquals (restoredEngine.getChannels()[0]->getInputLeft(), MixerEngine::oscSource (1));
        }

        beginTest ("Monitor paths: DIRECT IN, EQ curve, INVERT, LIMITER; SOURCE SOLO");
        {
            MixerEngine engine (0);
            PluginHost host (nullptr);
            Show show (engine, host);
            OfflineRig rig (engine, 4, 6);
            test::setChannels (engine, 40);
            engine.phones.outputLeft = 4;
            for (auto& ch : engine.getChannels())
            {
                ch->params.pan = -100.0f;
                ch->params.inputLeft = -1;
            }
            auto& ch1 = *engine.getChannels()[0];
            auto& ch2 = *engine.getChannels()[1];
            ch1.params.inputLeft = 0;
            ch2.params.inputLeft = 1;
            ch2.params.mains[0].on = false;
            rig.setInput (0, 0.25f);
            rig.setInput (1, 0.125f);
            rig.setInput (2, 0.5f);
            rig.run (4);
            expectWithinAbsoluteError (rig.out (4), 0.25f, 1.0e-5f);

            // DIRECT IN CH.2 on the PHONES, then INVERT.
            auto& phones = engine.monitorPath (true);
            expect (&phones == &engine.monitor.paths[1]);
            phones.directSource = 1;
            expectEquals (MixerEngine::directSourceName (1), juce::String ("CH.2"));
            expectEquals (MixerEngine::directSourceName (-1), juce::String ("OFF"));
            rig.run (4);
            expectWithinAbsoluteError (rig.out (4), 0.375f, 1.0e-5f);
            phones.invert = true;
            rig.run (4);
            expectWithinAbsoluteError (rig.out (4), -0.375f, 1.0e-5f);
            phones.invert = false;
            phones.limiterDb = -12.0f;
            rig.run (8);
            expect (rig.out (4) <= juce::Decibels::decibelsToGain (-12.0f) + 1.0e-5f);
            expect (phones.limiterGrDb.exchange (0.0f) > 1.0f);
            phones.limiterDb = 0.0f;
            rig.run (400); // its release

            // The EQ curve for the graph.
            phones.eqOn = true;
            phones.eq[3].freq = 1000.0f;
            phones.eq[3].gainDb = 6.0f;
            const float freqs[] = { 1000.0f };
            float db = 0.0f;
            engine.getMonitorEqResponse (true, -1, freqs, &db, 1);
            expectWithinAbsoluteError (db, 6.0f, 0.1f);
            expectWithinAbsoluteError (MixerEngine::monitorDelayMs (3.43f), 10.0f, 1.0e-3f);

            // SOURCE SOLO: nothing while OFF; CH39 hosts it (its SOLO), the solo follows the source.
            engine.setSoloBus (0); // PHONES
            engine.setSourceSolo (2, true);
            expect (! engine.isSourceSoloed (2));
            expect (engine.getSourceSoloHost() == nullptr);
            engine.monitor.sourceSolo = ld::kSourceSoloCh39;
            expect (engine.getSourceSoloHost() == engine.getChannels()[38].get());
            expect (engine.isSourceSoloed (2) && engine.isSourceSoloActive());
            engine.monitor.sourceSoloOn = false;
            engine.setSourceSolo (2, true);
            expect (engine.getChannels()[38]->params.solo.load(), "through the host's SOLO");
            expect (engine.isSourceSoloed (2));
            phones.eqOn = false;
            phones.directSource = -1;
            rig.run (8);
            expectWithinAbsoluteError (rig.out (4), 0.5f, 1.0e-4f);
            engine.setSourceSolo (1, true);
            expect (engine.isSourceSoloed (1) && ! engine.isSourceSoloed (2));
            rig.run (8);
            expectWithinAbsoluteError (rig.out (4), 0.125f, 1.0e-4f);
            engine.setSourceSolo (2, false); // not the soloed one: nothing changes
            expect (engine.isSourceSoloed (1));
            engine.setSourceSoloPort (2);
            expect (engine.isSourceSoloed (2));
            engine.setSourceSoloPort (1);
            engine.clearSourceSolo();
            expect (! engine.getChannels()[38]->params.solo.load());
            rig.run (8);
            expectWithinAbsoluteError (rig.out (4), 0.25f, 1.0e-4f);

            // Source tags: a mute group mutes the source.
            engine.getSource (0)->muteGroups = 1u << 1;
            engine.muteGroups = 1u << 1;
            rig.run (4);
            expectWithinAbsoluteError (rig.out (4), 0.0f, 1.0e-6f);
            engine.muteGroups = 0;

            // All of it survives a show.
            phones.delayOn = true;
            phones.delayM = 20.0f;
            auto xml = show.toXml();
            MixerEngine restoredEngine (0);
            Show restored (restoredEngine, host);
            expect (restored.loadFromXml (*xml).wasOk());
            expect (restoredEngine.monitorPath (true).delayOn.load());
            expectEquals (restoredEngine.monitorPath (true).delayM.load(), 20.0f);
            expectEquals ((int) restoredEngine.monitor.sourceSolo.load(), (int) ld::kSourceSoloCh39);
            expectEquals ((int) restoredEngine.getSource (0)->muteGroups.load(), 2);
        }

        beginTest ("The surface finish persists in the app settings");
        {
            juce::TemporaryFile file (".settings");
            juce::PropertiesFile::Options o;
            o.applicationName = "looktest";
            {
                juce::PropertiesFile props (file.getFile(), o);
                expect (ui::ConsoleLook::load (props).finish == ui::SurfaceFinish::Silver); // the default
                ui::ConsoleLook look;
                look.finish = ui::SurfaceFinish::Black;
                look.save (props);
                expectEquals (props.getValue (ui::ConsoleLook::finishKey), juce::String ("BLACK"));
            }
            juce::PropertiesFile reopened (file.getFile(), o);
            expect (ui::ConsoleLook::load (reopened).finish == ui::SurfaceFinish::Black);
            reopened.setValue (ui::ConsoleLook::finishKey, "GOLD"); // unknown: silver
            expect (ui::ConsoleLook::load (reopened).finish == ui::SurfaceFinish::Silver);
        }
    }
};

static ConsoleTests consoleTests;
