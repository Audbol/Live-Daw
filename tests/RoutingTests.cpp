#include "TestHelpers.h"
#include "ui/RoutingGroups.h"

using namespace livedaw;
using livedaw::test::OfflineRig;
namespace routing = livedaw::ui::routing;

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
    }
};

static RoutingTests routingTests;
