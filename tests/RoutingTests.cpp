#include "TestHelpers.h"
#include "ui/RoutingGroups.h"
#include "ui/UiContext.h"

using namespace livedaw;
using livedaw::test::OfflineRig;
namespace routing = livedaw::ui::routing;
using livedaw::ui::inputPortName;
using livedaw::ui::inputSourceText;

/** ROUTING's port groups: the WING's groups with Sidecar's ports in the ones
    they stand in for, their labels, menus and the default patch. */
class RoutingTests final : public juce::UnitTest
{
public:
    RoutingTests() : juce::UnitTest ("Routing groups", "LiveDAW") {}

    void runTest() override
    {
        using routing::Group;

        beginTest ("LOCAL IN holds the interface's inputs; AES50 B / C the ports past 48");
        {
            expectEquals (routing::sourceAt (Group::LocalIn, 0), 0);
            expectEquals (routing::sourceAt (Group::LocalIn, 47), 47);
            expectEquals (routing::sourceAt (Group::LocalIn, 48), -1);
            expectEquals (routing::sourceAt (Group::Aes50B, 0), 48);
            expectEquals (routing::sourceAt (Group::Aes50C, 0), 96);
            expectEquals (routing::sourceAt (Group::Aes50A, 0), -1); // the remote's name for LCL 1-48
            expectEquals (routing::sourceLabel (0), juce::String ("LCL 1"));
            expectEquals (routing::sourceLabel (50), juce::String ("B 3"));
            expect (routing::sourceLabel (-1).isEmpty());
            expectEquals (routing::pairLabel (0, 1), juce::String ("LCL 1/2"));
            expect (routing::groupOfSource (5) == Group::LocalIn);
            expect (routing::groupOfSource (60) == Group::Aes50B);

            // 24 tiles as on the console, in rows of 8 up to 48 for bigger interfaces.
            expectEquals (routing::tileCount (Group::LocalIn, 2), 24);
            expectEquals (routing::tileCount (Group::LocalIn, 26), 32);
            expectEquals (routing::tileCount (Group::LocalIn, 64), 48);
            expectEquals (routing::tileCount (Group::LocalOut, 2), 8);
            expectEquals (routing::tileCount (Group::LocalOut, 18), 24);
            expectEquals (routing::tileCount (Group::Dante, 2), 64);
        }

        beginTest ("The oscillator, WLIVE PLAY and USB PLAYER groups");
        {
            expectEquals (routing::sourceAt (Group::Oscillator, 1), MixerEngine::oscSource (1));
            expectEquals (routing::sourceAt (Group::Oscillator, 2), -1);
            expectEquals (routing::sourceAt (Group::WlivePlay, 63), Recorder::playbackSource (63));
            expectEquals (routing::sourceAt (Group::UsbPlayer, 0), UsbRecorder::playerSource (0));
            expectEquals (routing::sourceLabel (MixerEngine::oscSource (0)), juce::String ("OSC 1"));
            expectEquals (routing::sourceLabel (Recorder::playbackSource (4)), juce::String ("PLAY 5"));
            expectEquals (routing::sourceLabel (UsbRecorder::playerSource (1)), juce::String ("PLAYER 2"));
            expectEquals (routing::pairLabel (Recorder::playbackSource (2), Recorder::playbackSource (3)), juce::String ("PLAY 3/4"));
            // Groups Sidecar doesn't have can be shown but not patched.
            for (auto g : { Group::AuxIn, Group::AesIn, Group::StConnect, Group::UsbAudio, Group::Dante, Group::Bus, Group::UserSignal })
                expectEquals (routing::sourceAt (g, 0), -1);
        }

        beginTest ("Outputs: LOCAL OUT, AES50 B / C and the RECORDER's taps");
        {
            expectEquals (routing::outputAt (Group::LocalOut, 7), 7);
            expectEquals (routing::outputAt (Group::Aes50B, 1), 49);
            expectEquals (routing::outputAt (Group::Recorder, 3), routing::kRecorderPortBase + 3);
            expectEquals (routing::outputAt (Group::Recorder, 4), -1);
            expectEquals (routing::outputAt (Group::WliveRec, 0), -1);
            expect (routing::groupOfOutput (routing::kRecorderPortBase) == Group::Recorder);
            expect (routing::groupOfOutput (1) == Group::LocalOut);
            expectEquals (routing::outputNumber (routing::kRecorderPortBase + 1), 2);
            expectEquals (routing::outputNumber (50), 3);
        }

        beginTest ("The group menus list the console's groups in its order");
        {
            auto names = [] (const routing::Menu& m)
            {
                juce::StringArray list;
                for (auto& row : m.rows)
                    for (int id : row)
                        if (id >= 0)
                            list.add (routing::info ((Group) id).name);
                return list.joinIntoString (",");
            };
            expectEquals (names (routing::sourceMenu (true)),
                          juce::String ("LOCAL IN,AUX IN,AES/EBU IN,OSCILLATOR,AES50 A,AES50 B,AES50 C,ST CONNECT,USB AUDIO,WLIVE PLAY,DANTE,"
                                        "USB PLAYER,BUS,MAIN,MATRIX,USER SIGNAL"));
            expectEquals (names (routing::sourceMenu (false)),
                          juce::String ("LOCAL IN,AUX IN,AES/EBU IN,OSCILLATOR,AES50 A,AES50 B,AES50 C,ST CONNECT,USB AUDIO,WLIVE PLAY,DANTE,"
                                        "USB PLAYER,USER SIGNAL"));
            expect (names (routing::sourceMenu (true, true)).endsWith ("USER SIGNAL,MONITOR,FX SEND"));
            expectEquals (names (routing::outputMenu()),
                          juce::String ("LOCAL OUT,AUX OUT,AES/EBU OUT,AES50 A,AES50 B,AES50 C,ST CONNECT,USB AUDIO,WLIVE REC,DANTE,RECORDER"));
            expect (routing::sourceMenu (true).spacedRows == std::vector<int> { 3 });

            // The WING COMPACT has no local aux inputs or outputs: no AUX IN / AUX OUT.
            for (bool channels : { true, false })
            {
                const auto compact = names (routing::sourceMenu (channels, ! channels, true));
                expect (! compact.contains ("AUX IN") && compact.startsWith ("LOCAL IN,AES/EBU IN,"), compact);
            }
            expectEquals (names (routing::outputMenu (true)),
                          juce::String ("LOCAL OUT,AES/EBU OUT,AES50 A,AES50 B,AES50 C,ST CONNECT,USB AUDIO,WLIVE REC,DANTE,RECORDER"));
        }

        beginTest ("Default patch: each local input on its channel, MAIN 1 on LOCAL OUT 1-2");
        {
            // The console's init patches channels 1-8 to LCL 1-8 (its eight
            // local inputs); Sidecar patches every input of the interface.
            MixerEngine engine (0);
            OfflineRig rig (engine, 8, 2);
            engine.setNumChannels (40);
            for (int i = 0; i < 8; ++i)
                expectEquals (routing::sourceLabel (engine.getChannels()[(size_t) i]->getInputLeft()), "LCL " + juce::String (i + 1));
            expectEquals (engine.getChannels()[8]->getInputLeft(), -1);
            auto& main = *engine.getStrips (StripKind::Main)[0];
            expectEquals (main.params.outputLeft.load(), routing::outputAt (routing::Group::LocalOut, 0));
            expectEquals (main.params.outputRight.load(), routing::outputAt (routing::Group::LocalOut, 1));
        }

        beginTest ("The ROUTING lock starts locked, and INITIALIZE locks it again");
        {
            MixerEngine engine (0);
            routing::PatchLock lock (engine);
            expect (lock.isLocked(), "a fresh console: locked, as the WING");
            expect (! routing::showsPatchKeys (lock), "locked: no NONE, M, ST, M/S");
            expect (! lock.update(), "nothing initialized yet");
            expect (lock.set (false));
            expect (! lock.isLocked());
            expect (! lock.update(), "unlocked stays unlocked until the console is initialized");
            expect (! lock.isLocked());
            expect (routing::showsPatchKeys (lock), "unlocked: NONE, M, ST, M/S");

            // INITIALIZE CONSOLE (everything: a reset engine) and a partial one.
            engine.reset();
            expect (lock.update() && lock.isLocked());
            lock.set (false);
            engine.markInitialized();
            expect (lock.update() && lock.isLocked());
            expect (! lock.update());
            expect (! routing::showsPatchKeys (lock));
        }

        beginTest ("Local inputs are named LCL as on the WING, AES50 B / C by their group");
        {
            // The HOME source tile, SELECT MAIN INPUT, the ALT list and the
            // scribble strips' source tag use inputPortName; ROUTING and the
            // mute groups' SOURCE tab sourceLabel: both name a port alike.
            for (int port : { 0, 2, 47, 48, 95, 96, 143 })
                expectEquals (inputPortName (port), routing::sourceLabel (port));
            expectEquals (inputPortName (0), juce::String ("LCL 1"));
            expectEquals (inputPortName (49), juce::String ("B 2"));
            expectEquals (inputPortName (100), juce::String ("C 5"));
            expectEquals (inputPortName (2, 2), juce::String ("LCL 03"));
            expectEquals (inputPortName (144), juce::String ("IN 145"));
            expectEquals (inputPortName (Recorder::playbackSource (4)), juce::String ("PLAY 5"));
            expectEquals (inputPortName (MixerEngine::oscSource (0)), juce::String ("OSC 1"));
            expectEquals (inputSourceText (2, 3, true), juce::String ("LCL 3/4"));
            expectEquals (inputSourceText (48, -1, false), juce::String ("B 1"));
            expectEquals (inputSourceText (-1, -1, false), juce::String ("NONE"));

            // A name the user typed stays the source's name.
            MixerEngine engine (0);
            engine.setSourceName (MixerEngine::sourceIndex (0), "Kick In");
            expectEquals (engine.getSourceName (MixerEngine::sourceIndex (0)), juce::String ("Kick In"));
            expectEquals (inputPortName (0), juce::String ("LCL 1"));
        }

        beginTest ("Source GAIN takes the WING's range of the source's group");
        {
            // LOCAL IN (the full-size WING's own preamps): -2.5 .. +45 dB in 2.5 dB steps.
            const auto local = MixerEngine::sourceGainRange (MixerEngine::sourceIndex (0));
            expect (local.hasGain());
            expectEquals (local.min, -2.5f);
            expectEquals (local.max, 45.0f);
            expectEquals (local.step, 2.5f);
            expectEquals (local.steps(), 20);
            expectEquals (local.clamp (60.0f), 45.0f);
            expectEquals (local.clamp (-12.0f), -2.5f);
            expectEquals (local.clamp (6.0f), 5.0f);
            // 0 dB sits near the bottom of the fader, as in the WING's screenshots.
            expectWithinAbsoluteError (local.toProportion (0.0f), 2.5f / 47.5f, 1.0e-5f);
            expectEquals (local.fromProportion (1.0f), 45.0f);
            expectEquals (local.fromProportion (0.0f), -2.5f);

            // AES50 B / C (stage boxes): -3 .. +45.5 dB in 0.5 dB steps.
            const auto aes = MixerEngine::sourceGainRange (MixerEngine::sourceIndex (routing::sourceAt (Group::Aes50B, 0)));
            expectEquals (aes.min, -3.0f);
            expectEquals (aes.max, 45.5f);
            expectEquals (aes.steps(), 98);
            expectEquals (aes.clamp (6.2f), 6.0f);

            // No gain on the WING: the player, WLIVE PLAY and the oscillators.
            for (int port : { UsbRecorder::playerSource (0), Recorder::playbackSource (0), MixerEngine::oscSource (0) })
            {
                const auto none = MixerEngine::sourceGainRange (MixerEngine::sourceIndex (port));
                expect (! none.hasGain());
                expectEquals (none.clamp (12.0f), 0.0f);
            }
            expect (! MixerEngine::sourceGainRange (-1).hasGain());
        }
    }
};

static RoutingTests routingTests;
