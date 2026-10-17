#include "TestHelpers.h"
#include "engine/Show.h"

using namespace livedaw;
using livedaw::test::OfflineRig;

/** The console-level features through the C++ facade: SOLO MODE, MUTEGROUP/SIP
    OVERRIDE, the talk keys, the PHONES path, MON DCAs and sources. */
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
            engine.setNumChannels (2);
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
            engine.setNumChannels (40);
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
            engine.setNumChannels (1);
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
            engine.setNumChannels (1);
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
    }
};

static ConsoleTests consoleTests;
