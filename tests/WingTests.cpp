#include "TestHelpers.h"
#include "engine/PluginHost.h"
#include "engine/Show.h"
#include "engine/WingRemote.h"

#include <bit>
#include <cstring>

using namespace livedaw;

namespace
{
juce::File tempDir (const juce::String& name)
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("livedaw-wing-tests-" + name + "-" + juce::String (juce::Time::currentTimeMillis()));
    dir.deleteRecursively();
    dir.createDirectory();
    return dir;
}

/** OSC 1.0 string: NUL-terminated, padded to 4 bytes. */
void oscString (juce::MemoryBlock& m, const juce::String& s)
{
    m.append (s.toRawUTF8(), s.getNumBytesAsUTF8());
    const size_t pad = 4 - (s.getNumBytesAsUTF8() % 4);
    const char zeros[4] {};
    m.append (zeros, pad);
}

juce::MemoryBlock oscFloat (const juce::String& address, float value)
{
    juce::MemoryBlock m;
    oscString (m, address);
    oscString (m, ",f");
    const auto bits = juce::ByteOrder::swapIfLittleEndian (std::bit_cast<juce::uint32> (value));
    m.append (&bits, 4);
    return m;
}

juce::MemoryBlock oscGet (const juce::String& address)
{
    juce::MemoryBlock m;
    oscString (m, address);
    return m;
}

/** Sends `request` to the OSC port and returns the reply ("" on timeout). */
juce::String ask (juce::DatagramSocket& sock, int port, const juce::MemoryBlock& request)
{
    sock.write ("127.0.0.1", port, request.getData(), (int) request.getSize());
    if (sock.waitUntilReady (true, 2000) != 1)
        return {};
    char buf[2048];
    const int n = sock.read (buf, (int) sizeof (buf), false);
    juce::String text;
    for (int i = 0; i < n; ++i)
        text << (buf[i] >= 32 && buf[i] < 127 ? buf[i] : ' ');
    return text;
}
} // namespace

class WingTests final : public juce::UnitTest
{
public:
    WingTests() : juce::UnitTest ("WING remote", "LiveDAW") {}

    void runTest() override
    {
        beginTest ("Settings persist in the app settings");
        {
            auto dir = tempDir ("settings");
            juce::PropertiesFile::Options o;
            o.applicationName = "wingtest";
            juce::PropertiesFile props (dir.getChildFile ("t.settings"), o);
            auto s = WingRemote::Settings::load (props);
            expect (! s.enabled && s.oscPort == 2223 && s.nativePort == 2222 && s.discoveryPort == 2222);
            s.enabled = true;
            s.name = "FOH";
            s.oscPort = 10023;
            s.native = false;
            s.save (props);
            expect (WingRemote::Settings::load (props) == s);
            expectEquals (props.getValue ("setup.consoleName"), juce::String ("FOH"));
            props.setValue ("wing.oscPort", 70000); // out of range: the default
            expectEquals (WingRemote::Settings::load (props).oscPort, 2223);
            dir.deleteRecursively();
        }

        beginTest ("OSC clients move faders; the console answers with its name");
        {
            MixerEngine engine (0);
            engine.setNumChannels (8);
            auto& wing = engine.getWing();
            WingRemote::Settings s;
            s.enabled = true;
            s.name = "TEST-WING";
            s.discoveryPort = s.oscPort = s.nativePort = 0; // free ports
            expect (wing.apply (s).isEmpty());
            auto st = wing.getStatus();
            expect (st.running && st.oscPort > 0 && st.nativePort > 0 && st.discoveryPort > 0);

            juce::DatagramSocket sock;
            expect (sock.bindToPort (0, "127.0.0.1"));
            expect (ask (sock, st.oscPort, oscGet ("/?")).contains ("TEST-WING"));
            wing.setName ("RENAMED");
            expect (ask (sock, st.oscPort, oscGet ("/?")).contains ("RENAMED"));

            const auto set = oscFloat ("/ch/2/fdr", -12.0f);
            sock.write ("127.0.0.1", st.oscPort, set.getData(), (int) set.getSize());
            const auto reply = ask (sock, st.oscPort, oscGet ("/ch/2/fdr"));
            expect (reply.contains ("/ch/2/fdr") && reply.contains ("-12"), reply);
            expectWithinAbsoluteError (engine.getChannels()[1]->params.faderDb.load(), -12.0f, 0.01f);

            st = wing.getStatus();
            expectEquals ((int) st.clients.size(), 1);
            expect (! st.clients.empty() && ! st.clients[0].native && st.clients[0].address.startsWith ("127.0.0.1:"));
            expect (st.oscMessages >= 4);

            // Same servers, new name: no restart (the ports stay).
            s.name = "OTHER";
            wing.apply (s);
            expectEquals (wing.getStatus().oscPort, st.oscPort);
            s.enabled = false;
            wing.apply (s);
            expect (! wing.isRunning() && wing.getStatus().oscPort == -1);
        }

        beginTest ("WING snapshots export and import; shows import as scenes");
        {
            auto dir = tempDir ("snap");
            MixerEngine engine (0);
            PluginHost host (nullptr);
            Show show (engine, host);
            engine.setNumChannels (4);
            auto* ch = engine.getChannels()[0].get();
            ch->setName ("Vox");
            ch->params.faderDb = -7.5f;

            auto snap = dir.getChildFile ("Test.snap");
            expect (engine.getWing().exportSnapshot (snap).wasOk());
            expect (engine.getWing().exportSnapshot (dir.getChildFile ("new folder/x.snap")).wasOk(), "creates the folder");
            expect (snap.loadFileAsString().contains ("\"snapshot.11\""));

            ch->setName ("Changed");
            ch->params.faderDb = 0.0f;
            juce::String report;
            expect (engine.getWing().importSnapshot (snap, report).wasOk(), report);
            expect (report.startsWith ("WING snapshot"), report);
            expectEquals (engine.getChannels()[0]->getName(), juce::String ("Vox"));
            expectWithinAbsoluteError (engine.getChannels()[0]->params.faderDb.load(), -7.5f, 0.01f);
            expect (! engine.getWing().importSnapshot (dir.getChildFile ("missing.snap"), report).wasOk());

            dir.getChildFile ("_scenes").createDirectory();
            expect (snap.copyFileTo (dir.getChildFile ("_scenes/One.snap")));
            dir.getChildFile ("Test.show").replaceWithText (R"({"type": "showfile.1", "scenes": {"count": 2,
                "1": {"name": "Opener", "type": "SNAP", "file": "_scenes/One.snap", "tag": "#2", "skip": false, "link": false, "info": ""},
                "2": {"name": "Mutes", "type": "SNIP", "file": "_scenes/Mutes.snip", "tag": "", "skip": false, "link": false, "info": ""}}})");
            const int before = show.getNumScenes();
            expect (engine.getWing().importShow (show, dir.getChildFile ("Test.show"), report).wasOk(), report);
            expectEquals (show.getNumScenes(), before + 1);
            auto scene = show.getScene (before);
            expect (scene.has_value() && scene->name == "Opener" && scene->tag == 2);
            expect (report.contains ("Mutes"), report);
            dir.deleteRecursively();
        }

        beginTest ("Remote clients reach the GLOBAL safes and the USB recorder through the message thread");
        {
            auto dir = tempDir ("usb");
            MixerEngine engine (0);
            PluginHost host (nullptr);
            Show show (engine, host);
            engine.setNumChannels (4);
            engine.prepareOffline (48000.0, 256, 2, 2);
            auto& usb = engine.getUsbRecorder();
            usb.setStorageFolder (dir);
            dir.getChildFile ("LIVE").createDirectory();
            auto& wing = engine.getWing();
            wing.attachShow (&show);
            auto run = [&wing] (const char* nodeString)
            {
                ld::LdString* out = nullptr;
                const bool ok = ld::ld_wing_sysex (wing.getHandle(), 2, nodeString, std::strlen (nodeString), &out);
                takeString (out);
                return ok;
            };
            expect (run ("/$ctl.safes.dca=5"));
            expect (run ("/$ctl.safes.ch=2"));
            expect (run ("/rec.channels=4,resolution=16,$path=LIVE"));
            wing.pump();
            expectEquals ((int) show.getGlobalSafe().dcas, 1 << 4);
            expect (engine.getChannels()[1]->params.recallSafe.load(), "a strip's SAFE key");
            expect (usb.getRecChannels() == 4 && usb.getRecBits() == 16);
            expect (usb.getRecPath() == dir.getChildFile ("LIVE"), usb.getRecPath().getFullPathName());

            // REC from STOP: the host names the file in REC PATH.
            expect (run ("/rec.$action=REC"));
            wing.pump();
            expect (usb.isRecording());
            expect (usb.getRecFile().getParentDirectory() == dir.getChildFile ("LIVE"), usb.getRecFile().getFullPathName());
            usb.stopRecording();
            usb.waitForDisk();

            // The show's safes changed on the console reach the clients.
            auto safe = show.getGlobalSafe();
            safe.fx = 0b11;
            show.setGlobalSafe (safe);
            wing.pump();
            ld::LdString* dump = nullptr;
            expect (ld::ld_wing_sysex (wing.getHandle(), 3, "/$ctl.safes.fx", std::strlen ("/$ctl.safes.fx"), &dump));
            const auto text = takeString (dump);
            expect (text.contains ("1,2"), text);
            wing.attachShow (nullptr);
            dir.deleteRecursively();
        }

        beginTest ("The coverage report counts the mapped tree");
        expect (WingRemote::coverageReport().startsWith ("WING tree coverage"));
    }
};

static WingTests wingTests;
