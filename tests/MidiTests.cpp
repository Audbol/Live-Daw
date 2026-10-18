#include "TestHelpers.h"
#include "engine/MidiRemote.h"
#include "engine/Models.h"
#include "engine/PluginHost.h"
#include "engine/Show.h"

using namespace livedaw;

namespace
{
juce::MidiMessage cc (int channel, int controller, int value)
{
    return juce::MidiMessage::controllerEvent (channel, controller, value);
}

juce::MidiMessage sysex (std::initializer_list<int> bytes)
{
    juce::MemoryBlock b;
    for (int v : bytes)
    {
        const auto c = (juce::uint8) v;
        b.append (&c, 1);
    }
    return juce::MidiMessage::createSysExMessage (b.getData(), (int) b.getSize());
}

juce::MidiMessage sysexText (std::initializer_list<int> header, const juce::String& text)
{
    juce::MemoryBlock b;
    for (int v : header)
    {
        const auto c = (juce::uint8) v;
        b.append (&c, 1);
    }
    b.append (text.toRawUTF8(), text.getNumBytesAsUTF8());
    return juce::MidiMessage::createSysExMessage (b.getData(), (int) b.getSize());
}

/** Collects what a MidiRemote / DawRemote sends. */
struct Sent
{
    std::vector<std::pair<int, juce::MidiMessage>> messages;

    void clear() { messages.clear(); }
    int size() const { return (int) messages.size(); }
    const juce::MidiMessage& at (int i) const { return messages[(size_t) i].second; }
    int portAt (int i) const { return messages[(size_t) i].first; }

    /** The first controller message on `channel` / `controller`, or -1. */
    int ccValue (int channel, int controller) const
    {
        for (auto& [port, m] : messages)
            if (m.isController() && m.getChannel() == channel && m.getControllerNumber() == controller)
                return m.getControllerValue();
        return -1;
    }

    bool has (const juce::MidiMessage& expected) const
    {
        for (auto& [port, m] : messages)
            if (m.getRawDataSize() == expected.getRawDataSize() && std::memcmp (m.getRawData(), expected.getRawData(), (size_t) m.getRawDataSize()) == 0)
                return true;
        return false;
    }
};

juce::String sysexString (const juce::MidiMessage& m, int skip)
{
    juce::String s;
    for (int i = skip; i < m.getSysExDataSize(); ++i)
        s << juce::String::charToString ((juce::juce_wchar) m.getSysExData()[i]);
    return s;
}
} // namespace

class MidiTests final : public juce::UnitTest
{
public:
    MidiTests() : juce::UnitTest ("MIDI remote", "LiveDAW") {}

    void runTest() override
    {
        testFaderLaw();
        testChannelControl();
        testFx();
        testCustomAndScenes();
        testSysexAndExternal();
        testMcu();
        testHui();
        testDawRouting();
        testSettings();
    }

private:
    void testFaderLaw()
    {
        beginTest ("WING fader law and the CH CONTROL map");
        for (auto [db, pos] : { std::pair { 0.0f, 0.75f }, { 10.0f, 1.0f }, { -10.0f, 0.5f }, { -30.0f, 0.25f }, { -60.0f, 0.0625f } })
        {
            expectWithinAbsoluteError (MidiRemote::faderPosition (db), pos, 1.0e-5f);
            expectWithinAbsoluteError (MidiRemote::faderDb (pos), db, 1.0e-3f);
        }
        expectEquals (MidiRemote::faderDb (0.0f), -144.0f);
        for (int v = 0; v < 128; ++v)
            expectEquals (juce::roundToInt (MidiRemote::faderPosition (MidiRemote::faderDb ((float) v / 127.0f)) * 127.0f), v);

        MixerEngine engine (0);
        PluginHost host (nullptr);
        Show show (engine, host);
        engine.setNumChannels (40);
        MidiRemote midi (engine, show, false);
        auto label = [&midi] (int c) { auto* s = midi.stripForCc (c); return s != nullptr ? s->getLabel() : juce::String ("-"); };
        expectEquals (label (12), juce::String ("CH 1"));
        expectEquals (label (31), juce::String ("CH 20"));
        expectEquals (label (44), juce::String ("CH 21"));
        expectEquals (label (63), juce::String ("CH 40"));
        expectEquals (label (70), juce::String ("AUX 1"));
        expectEquals (label (77), juce::String ("AUX 8"));
        expectEquals (label (78), juce::String ("BUS 1"));
        expectEquals (label (93), juce::String ("BUS 16"));
        expectEquals (label (94), juce::String ("MAIN 1"));
        expectEquals (label (95), juce::String ("MAIN 2"));
        expectEquals (label (102), juce::String ("MAIN 3"));
        expectEquals (label (103), juce::String ("MAIN 4"));
        expectEquals (label (104), juce::String ("MTX 1"));
        expectEquals (label (111), juce::String ("MTX 8"));
        expect (midi.stripForCc (32) == nullptr && midi.stripForCc (100) == nullptr && midi.stripForCc (9) == nullptr);
        for (int c = 0; c < 128; ++c)
            if (auto* s = midi.stripForCc (c))
                expectEquals (midi.ccForStrip (*s), c);
    }

    void testChannelControl()
    {
        beginTest ("CH CONTROL: incoming CCs set faders, mutes, pans, DCAs and mute groups");
        MixerEngine engine (0);
        PluginHost host (nullptr);
        Show show (engine, host);
        engine.setNumChannels (40);
        MidiRemote midi (engine, show, false);
        Sent sent;
        midi.onSend = [&sent] (int port, const juce::MidiMessage& m) { sent.messages.emplace_back (port, m); };
        auto s = midi.getSettings();
        s.routes[MidiRemote::ChControl] = MidiRemote::RouteUsb;
        midi.apply (s);

        auto& ch1 = *engine.getChannels()[0];
        auto& ch21 = *engine.getChannels()[20];
        midi.handleMessage (MidiRemote::Usb, cc (1, 12, 95));
        expectWithinAbsoluteError (ch1.params.faderDb.load(), MidiRemote::faderDb (95.0f / 127.0f), 1.0e-4f);
        midi.handleMessage (MidiRemote::Usb, cc (1, 44, 0));
        expectEquals (ch21.params.faderDb.load(), -144.0f);
        midi.handleMessage (MidiRemote::Usb, cc (2, 12, 127));
        expect (ch1.params.mute.load());
        midi.handleMessage (MidiRemote::Usb, cc (2, 12, 0));
        expect (! ch1.params.mute.load());
        midi.handleMessage (MidiRemote::Usb, cc (3, 12, 0));
        expectWithinAbsoluteError (ch1.params.pan.load(), -100.0f, 1.0e-4f);
        midi.handleMessage (MidiRemote::Usb, cc (3, 12, 127));
        expectWithinAbsoluteError (ch1.params.pan.load(), 100.0f, 1.0e-4f);
        midi.handleMessage (MidiRemote::Usb, cc (3, 12, 64));
        expectWithinAbsoluteError (ch1.params.pan.load(), 0.0f, 1.0e-4f);
        midi.handleMessage (MidiRemote::Usb, cc (1, 94, 127));
        expectWithinAbsoluteError (engine.getMain (0).params.faderDb.load(), 10.0f, 1.0e-3f);
        midi.handleMessage (MidiRemote::Usb, cc (4, 13, 95));
        expectWithinAbsoluteError (engine.getDca (1).faderDb.load(), MidiRemote::faderDb (95.0f / 127.0f), 1.0e-4f);
        midi.handleMessage (MidiRemote::Usb, cc (5, 27, 127));
        expect (engine.getDca (15).mute.load());
        midi.handleMessage (MidiRemote::Usb, cc (5, 28, 127));
        midi.handleMessage (MidiRemote::Usb, cc (5, 47, 127));
        expectEquals ((int) engine.muteGroups.load(), 0x81);
        midi.handleMessage (MidiRemote::Usb, cc (5, 28, 0));
        expectEquals ((int) engine.muteGroups.load(), 0x80);
        // Only on the routed port.
        midi.handleMessage (MidiRemote::Din, cc (2, 12, 127));
        expect (! ch1.params.mute.load());

        beginTest ("CH CONTROL: console changes go out, received values don't echo");
        sent.clear();
        midi.sendFeedback(); // the first pass learns the console
        expectEquals (sent.size(), 0);
        midi.stripForCc (16)->params.faderDb = -20.0f; // CH 5
        midi.stripForCc (16)->params.mute = true;
        midi.stripForCc (80)->params.pan = 100.0f;   // BUS 3
        engine.getDca (0).mute = true;
        engine.muteGroups = engine.muteGroups.load() | 0x4u; // group 3
        midi.sendFeedback();
        expectEquals (sent.ccValue (1, 16), juce::roundToInt (MidiRemote::faderPosition (-20.0f) * 127.0f));
        expectEquals (sent.ccValue (2, 16), 127);
        expectEquals (sent.ccValue (3, 80), 127);
        expectEquals (sent.ccValue (5, 12), 127);
        expectEquals (sent.ccValue (5, 30), 127);
        expectEquals (sent.size(), 5);
        expect (sent.portAt (0) == MidiRemote::Usb);
        sent.clear();
        midi.handleMessage (MidiRemote::Usb, cc (1, 13, 50));
        midi.sendFeedback();
        expectEquals (sent.size(), 0);

        beginTest ("CH CONTROL: CC 9 requests all values");
        midi.handleMessage (MidiRemote::Usb, cc (1, 9, 0));
        expectEquals (sent.size(), 40 + 8 + 16 + 4 + 8);
        expectEquals (sent.ccValue (1, 13), 50);
        sent.clear();
        midi.handleMessage (MidiRemote::Usb, cc (5, 9, 0));
        expectEquals (sent.size(), 16 + 8);
    }

    void testFx()
    {
        beginTest ("FX PARAMETER CONTROL: the CC map and both directions");
        for (int slot = 0; slot < kNumFx; ++slot)
            for (int param = -3; param < 33; ++param)
                if (const int c = MidiRemote::fxCc (slot, param); c >= 0)
                {
                    int s = -1, p = -9;
                    expect (MidiRemote::fxControl (9 + slot % 8, c, s, p));
                    expectEquals (s, slot);
                    expectEquals (p, param);
                }
        expectEquals (MidiRemote::fxCc (0, 31), 58);
        expectEquals (MidiRemote::fxCc (0, 32), -1);
        expectEquals (MidiRemote::fxCc (8, 32), 111);

        MixerEngine engine (0);
        PluginHost host (nullptr);
        Show show (engine, host);
        MidiRemote midi (engine, show, false);
        Sent sent;
        midi.onSend = [&sent] (int port, const juce::MidiMessage& m) { sent.messages.emplace_back (port, m); };
        auto s = midi.getSettings();
        s.routes[MidiRemote::FxControl] = MidiRemote::RouteDin;
        midi.apply (s);
        auto& fx = engine.getFx();
        midi.handleMessage (MidiRemote::Din, cc (9, 13, 127));
        expectWithinAbsoluteError (fx.params (0).mix.load(), 100.0f, 1.0e-3f);
        midi.handleMessage (MidiRemote::Din, cc (16, 71, 0));
        expectWithinAbsoluteError (fx.params (15).mix.load(), 0.0f, 1.0e-3f);
        midi.handleMessage (MidiRemote::Din, cc (9, 12, 0));
        expect (fx.params (0).bypass.load());
        midi.handleMessage (MidiRemote::Din, cc (9, 12, 127));
        expect (! fx.params (0).bypass.load());

        // A device by model index, then its first parameter at the top of its range.
        const int model = (int) models::table (models::Fx).size() - 1;
        if (FxSlots::isAllowed (model, 9) && model < 128)
        {
            midi.handleMessage (MidiRemote::Din, cc (10, 72, model));
            expectEquals (fx.getModel (9), model);
            const auto& spec = models::model (models::Fx, model);
            if (! spec.params.empty())
            {
                midi.handleMessage (MidiRemote::Din, cc (10, 73, 127));
                expectWithinAbsoluteError (fx.params (9).values[0].load(), spec.params[0].fromNormalised (1.0f), 1.0e-3f);
            }
        }

        sent.clear();
        midi.sendFeedback();
        fx.params (2).mix = 0.0f;
        midi.sendFeedback();
        expectEquals (sent.ccValue (11, 13), 0);
        sent.clear();
        midi.handleMessage (MidiRemote::Din, cc (9, 9, 4)); // FX 4 data
        expect (sent.size() >= 3);
        expect (sent.ccValue (12, 13) >= 0 && sent.ccValue (12, 12) == 127);
    }

    void testCustomAndScenes()
    {
        beginTest ("CUSTOM CONTROLS (RX ONLY): channel 6 encoders and keys");
        for (int layer = 0; layer < 16; ++layer)
            for (int control = 0; control < 4; ++control)
            {
                int l = -1, c = -1;
                expect (MidiRemote::customEncoder (MidiRemote::customEncoderCc (layer, control), l, c));
                expect (l == layer && c == control);
            }
        expectEquals (MidiRemote::customEncoderCc (5, 0), 44);
        expectEquals (MidiRemote::customEncoderCc (15, 3), 93);

        MixerEngine engine (0);
        PluginHost host (nullptr);
        Show show (engine, host);
        MidiRemote midi (engine, show, false);
        Sent sent;
        midi.onSend = [&sent] (int port, const juce::MidiMessage& m) { sent.messages.emplace_back (port, m); };
        auto s = midi.getSettings();
        s.routes[MidiRemote::CustomControls] = MidiRemote::RouteUsb;
        s.routes[MidiRemote::SceneChange] = MidiRemote::RouteUsb;
        s.routes[MidiRemote::ShowControl] = MidiRemote::RouteUsb;
        s.routes[MidiRemote::SceneTx] = MidiRemote::RouteDin;
        midi.apply (s);
        int encLayer = -1, encControl = -1, encValue = -1, keyLayer = -1, key = -1;
        bool keyDown = false;
        midi.onCustomEncoder = [&] (int l, int c, int v) { encLayer = l, encControl = c, encValue = v; };
        midi.onCustomKey = [&] (int l, int k, bool d) { keyLayer = l, key = k, keyDown = d; };
        midi.handleMessage (MidiRemote::Usb, cc (6, 17, 100));
        expect (encLayer == 1 && encControl == 1 && encValue == 100);
        midi.handleMessage (MidiRemote::Usb, juce::MidiMessage::noteOn (6, 13, (juce::uint8) 127));
        expect (keyLayer == 1 && key == 5 && keyDown);
        midi.handleMessage (MidiRemote::Usb, juce::MidiMessage::noteOff (6, 13));
        expect (! keyDown);

        beginTest ("SCENE CHANGE, SHOW CONTROL and SCENE MIDI TX");
        for (auto* sceneName : { "Intro", "Verse", "Chorus", "Outro" })
            show.addScene (sceneName);
        show.setSceneTag (2, 5);
        juce::String recalled;
        midi.onRecalled = [&recalled] (const juce::String& what, const state::ApplyResult&) { recalled = what; };
        sent.clear();
        midi.sendFeedback();
        midi.handleMessage (MidiRemote::Usb, juce::MidiMessage::programChange (7, 1));
        expectEquals (show.getCurrentSceneIndex(), 1);
        expect (recalled.contains ("scene 2"));
        midi.handleMessage (MidiRemote::Usb, juce::MidiMessage::programChange (8, 4)); // tag #5
        expectEquals (show.getCurrentSceneIndex(), 2);
        midi.handleMessage (MidiRemote::Usb, juce::MidiMessage::programChange (9, 1)); // PREV: the cursor only
        expectEquals (show.getCurrentSceneIndex(), 2);
        midi.handleMessage (MidiRemote::Usb, juce::MidiMessage::programChange (9, 0)); // GO
        expectEquals (show.getCurrentSceneIndex(), 2);
        midi.handleMessage (MidiRemote::Usb, juce::MidiMessage::programChange (9, 4)); // GO NEXT
        expectEquals (show.getCurrentSceneIndex(), 3);
        midi.handleMessage (MidiRemote::Usb, juce::MidiMessage::programChange (9, 3)); // GO PREV
        expectEquals (show.getCurrentSceneIndex(), 2);
        // Recalls made by MIDI are not sent back; recalls on the console are.
        midi.sendFeedback();
        expectEquals (sent.size(), 0);
        show.recallScene (0);
        midi.sendFeedback();
        expect (sent.has (juce::MidiMessage::programChange (7, 0)));
        expect (sent.has (cc (7, 0, 0)));
        expect (sent.portAt (0) == MidiRemote::Din);
        // Not routed: nothing happens.
        s.routes[MidiRemote::SceneChange] = MidiRemote::Off;
        midi.apply (s);
        midi.handleMessage (MidiRemote::Usb, juce::MidiMessage::programChange (7, 3));
        expectEquals (show.getCurrentSceneIndex(), 0);
    }

    void testSysexAndExternal()
    {
        beginTest ("SYSEX CONTROL and EXTERNAL MIDI CONTROL");
        MixerEngine engine (0);
        PluginHost host (nullptr);
        Show show (engine, host);
        MidiRemote midi (engine, show, false);
        Sent sent;
        midi.onSend = [&sent] (int port, const juce::MidiMessage& m) { sent.messages.emplace_back (port, m); };
        auto s = midi.getSettings();
        s.routes[MidiRemote::SysexControl] = MidiRemote::RouteUsb;
        midi.apply (s);
        midi.handleMessage (MidiRemote::Usb, sysex ({ 0x00, 0x20, 0x32, 0x57, 0x00 }));
        expectEquals (sent.size(), 1);
        expect (sent.at (0).isSysEx() && sent.at (0).getSysExData()[4] == 0x01);
        expect (sysexString (sent.at (0), 5).startsWith ("WING,"));

        // The default handlers work on the WING tree.
        engine.setNumChannels (4);
        midi.handleMessage (MidiRemote::Usb, sysexText ({ 0x00, 0x20, 0x32, 0x57, 0x02 }, "/ch.1.fdr=-3,mute=1,.2.pan=50"));
        expectWithinAbsoluteError (engine.getChannels()[0]->params.faderDb.load(), -3.0f, 1.0e-3f);
        expect (engine.getChannels()[0]->params.mute.load());
        expectWithinAbsoluteError (engine.getChannels()[1]->params.pan.load(), 50.0f, 1.0e-3f);
        sent.clear();
        midi.handleMessage (MidiRemote::Usb, sysexText ({ 0x00, 0x20, 0x32, 0x57, 0x03 }, "/ch.1.fdr"));
        expect (sent.size() == 1 && sent.at (0).getSysExData()[4] == 0x04 && sysexString (sent.at (0), 5) == "-3.0");
        sent.clear();
        midi.handleMessage (MidiRemote::Usb, sysexText ({ 0x00, 0x20, 0x32, 0x57, 0x02 }, "/ch.1.fd=-1"));
        expect (sent.size() == 1 && sent.at (0).getSysExData()[4] == 0x07 && sysexString (sent.at (0), 5) == "NODE NOT FOUND");
        sent.clear();
        midi.handleMessage (MidiRemote::Usb, sysexText ({ 0x00, 0x20, 0x32, 0x57, 0x05 }, "/ch.1.fdr"));
        expect (sent.size() == 1 && sent.at (0).getSysExData()[4] == 0x06 && sysexString (sent.at (0), 5).contains ("fader"));

        juce::String executed;
        midi.sysexExecute = [&executed] (const juce::String& text) { executed = text; return text.contains ("xx") ? juce::String ("NODE NOT FOUND") : juce::String(); };
        midi.sysexDump = [] (const juce::String& node) -> std::optional<juce::String> { return node == "/ch.1.fdr" ? std::optional<juce::String> ("-1.0") : std::nullopt; };
        sent.clear();
        midi.handleMessage (MidiRemote::Usb, sysexText ({ 0x00, 0x20, 0x32, 0x57, 0x02 }, "/ch.1.fdr=-1,mute=0"));
        expectEquals (executed, juce::String ("/ch.1.fdr=-1,mute=0"));
        expectEquals (sent.size(), 0);
        midi.handleMessage (MidiRemote::Usb, sysexText ({ 0x00, 0x20, 0x32, 0x57, 0x02 }, "/xx=1"));
        expect (sent.size() == 1 && sent.at (0).getSysExData()[4] == 0x07 && sysexString (sent.at (0), 5) == "NODE NOT FOUND");
        sent.clear();
        midi.handleMessage (MidiRemote::Usb, sysexText ({ 0x00, 0x20, 0x32, 0x57, 0x03 }, "/ch.1.fdr"));
        expect (sent.size() == 1 && sent.at (0).getSysExData()[4] == 0x04 && sysexString (sent.at (0), 5) == "-1.0");
        // Another manufacturer's sysex is ignored.
        sent.clear();
        midi.handleMessage (MidiRemote::Usb, sysex ({ 0x00, 0x20, 0x33, 0x57, 0x00 }));
        expectEquals (sent.size(), 0);

        expect (! midi.sendExternal (cc (1, 20, 64)));
        s.routes[MidiRemote::ExternalMidi] = MidiRemote::RouteDin;
        midi.apply (s);
        expect (midi.sendExternal (cc (1, 20, 64)));
        expect (sent.portAt (sent.size() - 1) == MidiRemote::Din);
        midi.handleMessage (MidiRemote::Din, cc (3, 7, 99));
        expectEquals (midi.getExternalCc (3, 7), 99);
        midi.handleMessage (MidiRemote::Din, juce::MidiMessage::noteOn (2, 60, (juce::uint8) 100));
        expect (midi.getExternalToggle (2, 60, true));
    }

    void testMcu()
    {
        beginTest ("MCU: the surface's messages (faders 14-bit, V-pots relative, buttons, banks)");
        DawRemote daw;
        DawRemote::Settings s;
        s.enabled = true;
        s.configuration = DawRemote::McuTwoExtenders;
        daw.setSettings (s);
        expectEquals (daw.getNumUnits(), 3);
        std::vector<std::pair<int, juce::MidiMessage>> out;
        daw.output = [&out] (int unit, const juce::MidiMessage& m) { out.emplace_back (unit, m); };
        auto last = [&out] { return out.back().second; };

        daw.moveFader (0, 1.0);
        expect (out.back().first == 0 && last().isPitchWheel() && last().getChannel() == 1 && last().getPitchWheelValue() == 16383);
        daw.moveFader (9, 0.5);
        expect (out.back().first == 1 && last().getChannel() == 2 && last().getPitchWheelValue() == 8192);
        daw.moveFader (DawRemote::kMaster, 0.0);
        expect (last().getChannel() == 9 && last().getPitchWheelValue() == 0);
        daw.touchFader (3, true);
        expect (last().isNoteOn() && last().getNoteNumber() == 0x6b && last().getVelocity() == 127);
        daw.touchFader (3, false);
        expect (last().getNoteNumber() == 0x6b && last().getVelocity() == 0);
        daw.turnVpot (2, 1);
        expect (last().getControllerNumber() == 0x12 && last().getControllerValue() == 0x01);
        daw.turnVpot (18, -3);
        expect (out.back().first == 2 && last().getControllerNumber() == 0x12 && last().getControllerValue() == 0x43);
        daw.pressVpot (5, true);
        expect (last().getNoteNumber() == 0x25 && last().getVelocity() == 127);
        daw.pressStripButton (10, DawRemote::StripButton::Mute, true);
        expect (out.back().first == 1 && last().getNoteNumber() == 0x12);
        daw.pressStripButton (7, DawRemote::StripButton::Select, true);
        expect (last().getNoteNumber() == 0x1f);
        daw.pressStripButton (0, DawRemote::StripButton::Rec, false);
        expect (last().getNoteNumber() == 0x00 && last().getVelocity() == 0);
        daw.pressCommand (DawRemote::findCommand ("T2"), true);
        expect (last().getNoteNumber() == 0x5e && last().getVelocity() == 127);
        daw.pressCommand (DawRemote::findCommand ("N7"), true); // BANK >
        expect (last().getNoteNumber() == 0x2f);
        daw.pressCommand (DawRemote::findCommand ("N8"), true); // CHANNEL <
        expect (last().getNoteNumber() == 0x30);
        daw.pressCommand (DawRemote::findCommand ("M3P"), true);
        expect (last().getNoteNumber() == 0x22);
        daw.turnJog (-1);
        expect (last().getControllerNumber() == 0x3c && last().getControllerValue() == 0x41);
        const auto count = out.size();
        daw.pressCommand (DawRemote::findCommand ("T12"), true); // SHUTTLE: HUI only
        expect (out.size() == count && ! daw.isAvailable (DawRemote::findCommand ("T12")));

        beginTest ("MCU: the DAW's messages (motor faders, LEDs, rings, meters, LCD, displays)");
        daw.handleMessage (0, juce::MidiMessage::pitchWheel (1, 1000));
        expectEquals (daw.getStrip (0).fader, 1000);
        daw.handleMessage (2, juce::MidiMessage::pitchWheel (8, 12000));
        expectEquals (daw.getStrip (23).fader, 12000);
        daw.handleMessage (0, juce::MidiMessage::pitchWheel (9, 777));
        expectEquals (daw.getMasterFader(), 777);
        daw.handleMessage (0, juce::MidiMessage::noteOn (1, 0x0a, (juce::uint8) 127));
        expectEquals (daw.getStrip (2).solo, (int) DawRemote::On);
        daw.handleMessage (1, juce::MidiMessage::noteOn (1, 0x1c, (juce::uint8) 1));
        expectEquals (daw.getStrip (12).select, (int) DawRemote::Blink);
        daw.handleMessage (0, juce::MidiMessage::noteOn (1, 0x5e, (juce::uint8) 127));
        expectEquals (daw.getCommandLed (DawRemote::findCommand ("T2")), (int) DawRemote::On);
        daw.handleMessage (0, juce::MidiMessage::noteOn (1, 0x5e, (juce::uint8) 0));
        expectEquals (daw.getCommandLed (DawRemote::findCommand ("T2")), (int) DawRemote::Off);
        daw.handleMessage (0, cc (1, 0x31, 0x16));
        expectEquals (daw.getStrip (1).ring, 0x16);
        expectEquals (DawRemote::ringLeds (0x16), 0x020); // boost/cut at the centre
        expectEquals (DawRemote::ringLeds (0x13), 0x03c); // boost/cut: LEDs 3-6 of 11
        expectEquals (DawRemote::ringLeds (0x01), 0x001);
        expectEquals (DawRemote::ringLeds (0x0b), 0x400);
        expectEquals (DawRemote::ringLeds (0x26), 0x03f);
        expectEquals (DawRemote::ringLeds (0x31), 0x020);
        expectEquals (DawRemote::ringLeds (0x32), 0x070);
        expectEquals (DawRemote::ringLeds (0x40), 0x800);
        daw.handleMessage (0, juce::MidiMessage::channelPressureChange (1, 0x3c));
        expectEquals (daw.getStrip (3).meter, 12);
        daw.handleMessage (0, juce::MidiMessage::channelPressureChange (1, 0x3e));
        expect (daw.getStrip (3).overload);
        daw.tick (juce::Time::getMillisecondCounter() + 1000);
        expect (daw.getStrip (3).meter < 12);
        daw.handleMessage (0, sysexText ({ 0x00, 0x00, 0x66, 0x14, 0x12, 0x00 }, "Kick   Snare  "));
        expectEquals (daw.getStrip (0).upper, juce::String ("Kick"));
        expectEquals (daw.getStrip (1).upper, juce::String ("Snare"));
        daw.handleMessage (1, sysexText ({ 0x00, 0x00, 0x66, 0x15, 0x12, 0x38 }, "  -3.5 "));
        expectEquals (daw.getStrip (8).lower, juce::String ("  -3.5"));
        daw.handleMessage (0, cc (1, 0x4b, 0x10)); // P
        daw.handleMessage (0, cc (1, 0x4a, 0x0e)); // N
        expectEquals (daw.getAssignment(), juce::String ("PN"));
        daw.handleMessage (0, cc (1, 0x40, 0x31)); // rightmost digit '1'
        daw.handleMessage (0, cc (1, 0x41, 0x72)); // '2.'
        expect (daw.getTimecode().endsWith ("2.1"));
        expect (daw.isConnected (0) && daw.isConnected (1) && daw.isConnected (2));

        beginTest ("MCU: handshake and version");
        out.clear();
        daw.handleMessage (0, sysex ({ 0x00, 0x00, 0x66, 0x14, 0x00 }));
        expect (out.size() == 1 && last().isSysEx() && last().getSysExDataSize() == 5 + 11 && last().getSysExData()[4] == 0x01);
        expectEquals (sysexString (last(), 5).substring (0, 7), DawRemote::serialNumber (0));
        daw.handleMessage (1, sysex ({ 0x00, 0x00, 0x66, 0x15, 0x02, 'L', 'D', 'A', 'W', '0', '0', '1', 1, 2, 3, 4 }));
        expect (out.back().first == 1 && last().getSysExData()[3] == 0x15 && last().getSysExData()[4] == 0x03);
        daw.handleMessage (0, sysex ({ 0x00, 0x00, 0x66, 0x14, 0x13, 0x00 }));
        expect (last().getSysExData()[4] == 0x14 && sysexString (last(), 5) == "V1.07");
        daw.handleMessage (0, sysex ({ 0x00, 0x00, 0x66, 0x14, 0x63 }));
        expectEquals (daw.getStrip (0).fader, 0);
        expect (daw.getStrip (0).upper.isEmpty());

        beginTest ("MCU: banks of four for the upper CC section, special keys");
        daw.setVpotBlock (9);
        expectEquals (daw.getVpotBlock(), 5);
        s.configuration = DawRemote::SingleMcu;
        daw.setSettings (s);
        expectEquals (daw.getVpotBlock(), 1);
        out.clear();
        daw.moveFader (8, 1.0); // beyond the single unit
        expect (out.empty());
        daw.pressCommand (DawRemote::findCommand ("SP3"), true); // RECRDY CTRL [SEL] held
        daw.pressChannelKey (2, DawRemote::StripButton::Select, true);
        expect (last().getNoteNumber() == 0x02);
        daw.pressCommand (DawRemote::findCommand ("SP3"), false);
        daw.pressChannelKey (2, DawRemote::StripButton::Select, true);
        expect (last().getNoteNumber() == 0x1a);
        daw.pressCommand (DawRemote::findCommand ("SP1"), true); // FADER TOUCH [MUTE] latches
        expectEquals (daw.getCommandLed (DawRemote::findCommand ("SP1")), (int) DawRemote::On);
        daw.pressChannelKey (4, DawRemote::StripButton::Mute, true);
        expect (last().getNoteNumber() == 0x6c && last().getVelocity() == 127);
        daw.pressChannelKey (4, DawRemote::StripButton::Mute, true);
        expect (last().getNoteNumber() == 0x6c && last().getVelocity() == 0);

        beginTest ("DAW presets: every key is a command of the emulation");
        for (int p = 0; p < DawRemote::presetNames().size(); ++p)
        {
            const auto keys = DawRemote::presetKeys (p);
            expectEquals ((int) keys.size(), 32);
            DawRemote check;
            DawRemote::Settings cs;
            cs.emulation = p == 5 ? DawRemote::Emulation::Hui : DawRemote::Emulation::Mcu;
            check.setSettings (cs);
            for (auto& k : keys)
                expect (k.command >= 0 && check.isAvailable (k.command), DawRemote::presetNames()[p] + " " + juce::String (k.layer) + "/" + juce::String (k.key));
        }
        expectEquals (DawRemote::commandLabel (DawRemote::findCommand ("T6"), 1), juce::String ("LEFT"));
        expectEquals (DawRemote::commandLabel (DawRemote::findCommand ("T6"), 3), juce::String ("MARKER"));
    }

    void testHui()
    {
        beginTest ("HUI: zones and ports, faders, V-pots, ping, LEDs and scribble strips");
        DawRemote daw;
        DawRemote::Settings s;
        s.enabled = true;
        s.emulation = DawRemote::Emulation::Hui;
        s.configuration = DawRemote::McuExtender;
        daw.setSettings (s);
        std::vector<std::pair<int, juce::MidiMessage>> out;
        daw.output = [&out] (int unit, const juce::MidiMessage& m) { out.emplace_back (unit, m); };
        auto at = [&out] (size_t back) { return out[out.size() - 1 - back].second; };

        daw.moveFader (1, 1.0);
        expect (at (1).getControllerNumber() == 0x01 && at (1).getControllerValue() == 0x7f);
        expect (at (0).getControllerNumber() == 0x21 && at (0).getControllerValue() == 0x7f);
        daw.touchFader (9, true);
        expect (out.back().first == 1 && at (1).getControllerNumber() == 0x0f && at (1).getControllerValue() == 0x01);
        expect (at (0).getControllerNumber() == 0x2f && at (0).getControllerValue() == 0x40);
        daw.pressStripButton (3, DawRemote::StripButton::Solo, true);
        expect (at (1).getControllerValue() == 0x03 && at (0).getControllerValue() == 0x43);
        daw.pressStripButton (3, DawRemote::StripButton::Solo, false);
        expect (at (0).getControllerValue() == 0x03);
        daw.turnVpot (0, 2);
        expect (at (0).getControllerNumber() == 0x40 && at (0).getControllerValue() == 0x42);
        daw.turnVpot (0, -2);
        expect (at (0).getControllerValue() == 0x02);
        daw.pressCommand (DawRemote::findCommand ("T1"), true); // STOP: zone 0e port 3
        expect (at (1).getControllerValue() == 0x0e && at (0).getControllerValue() == 0x43);
        daw.turnJog (1);
        expect (at (0).getControllerNumber() == 0x0d && at (0).getControllerValue() == 0x41);

        out.clear();
        daw.handleMessage (0, juce::MidiMessage::noteOn (1, 0, (juce::uint8) 0));
        expect (out.size() == 1 && at (0).isNoteOn() && at (0).getNoteNumber() == 0 && at (0).getVelocity() == 0x7f);
        daw.handleMessage (0, cc (1, 0x0c, 0x02));
        daw.handleMessage (0, cc (1, 0x2c, 0x42));
        expectEquals (daw.getStrip (2).mute, (int) DawRemote::On);
        daw.handleMessage (0, cc (1, 0x0c, 0x0e));
        daw.handleMessage (0, cc (1, 0x2c, 0x44));
        expectEquals (daw.getCommandLed (DawRemote::findCommand ("T2")), (int) DawRemote::On);
        daw.handleMessage (1, cc (1, 0x03, 0x40));
        daw.handleMessage (1, cc (1, 0x23, 0x00));
        expectEquals (daw.getStrip (11).fader, 0x2000);
        daw.handleMessage (0, cc (1, 0x14, 0x16));
        expectEquals (daw.getStrip (4).ring, 0x16);
        daw.handleMessage (0, juce::MidiMessage::aftertouchChange (1, 5, 0x1a));
        expectEquals (daw.getStrip (5).meter, 10);
        daw.handleMessage (1, sysexText ({ 0x00, 0x00, 0x66, 0x05, 0x00, 0x10, 0x02 }, "Bass"));
        expectEquals (daw.getStrip (10).upper, juce::String ("Bass"));
        daw.handleMessage (0, sysex ({ 0x00, 0x00, 0x66, 0x05, 0x00, 0x11, 0x01, 0x12 }));
        expect (daw.getTimecode().trim().endsWith ("2.1"));
        expect (daw.isConnected (0));
        expect (daw.isAvailable (DawRemote::findCommand ("T12")) && ! daw.isAvailable (DawRemote::findCommand ("A1")));
    }

    void testDawRouting()
    {
        beginTest ("DAW remote through the ports: MIDI DAW 1-3 on USB, one unit on DIN");
        MixerEngine engine (0);
        PluginHost host (nullptr);
        Show show (engine, host);
        MidiRemote midi (engine, show, false);
        Sent sent;
        midi.onSend = [&sent] (int port, const juce::MidiMessage& m) { sent.messages.emplace_back (port, m); };
        auto s = midi.getSettings();
        s.daw.enabled = true;
        s.daw.configuration = DawRemote::McuTwoExtenders;
        s.routes[MidiRemote::ChControl] = MidiRemote::RouteDin;
        midi.apply (s);
        auto& daw = midi.getDaw();
        daw.moveFader (16, 1.0);
        expect (sent.size() == 1 && sent.portAt (0) == MidiRemote::Daw3);
        midi.handleMessage (MidiRemote::Daw2, juce::MidiMessage::pitchWheel (1, 500));
        expectEquals (daw.getStrip (8).fader, 500);
        // The DAW port's messages don't reach MIDI REMOTE CONTROL.
        engine.setNumChannels (4);
        engine.getChannels()[0]->params.faderDb = -10.0f;
        midi.handleMessage (MidiRemote::Daw1, cc (1, 12, 0));
        expectEquals (engine.getChannels()[0]->params.faderDb.load(), -10.0f);
        daw.setOn (true);
        expect (daw.isOn());
        s.daw.enabled = false;
        midi.apply (s);
        expect (! daw.isOn());

        s.daw.enabled = true;
        s.daw.usb = false;
        midi.apply (s);
        sent.clear();
        expectEquals (daw.getNumUnits(), 1);
        daw.pressCommand (DawRemote::findCommand ("T1"), true);
        expect (sent.size() == 1 && sent.portAt (0) == MidiRemote::Din);
        // DIN now carries the DAW: CH CONTROL on DIN gets nothing.
        midi.handleMessage (MidiRemote::Din, cc (1, 12, 0));
        expectEquals (engine.getChannels()[0]->params.faderDb.load(), -10.0f);
    }

    void testSettings()
    {
        beginTest ("MIDI and DAW settings persist in the app settings");
        auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("livedaw-midi-tests-" + juce::String (juce::Time::currentTimeMillis()));
        dir.createDirectory();
        {
            juce::PropertiesFile::Options o;
            o.applicationName = "miditest";
            juce::PropertiesFile props (dir.getChildFile ("t.settings"), o);
            auto s = MidiRemote::Settings::load (props);
            expect (s.routes[MidiRemote::ChControl] == MidiRemote::Off && s.inputs[MidiRemote::Usb] == MidiRemote::virtualDevice && s.inputs[MidiRemote::Din].isEmpty());
            expect (! s.daw.enabled && s.daw.usb && s.daw.configuration == DawRemote::McuTwoExtenders && s.daw.disableWheel);
            s.routes[MidiRemote::ShowControl] = MidiRemote::RouteDin;
            s.inputs[MidiRemote::Din] = "My Keyboard";
            s.outputs[MidiRemote::Daw2] = {};
            s.daw.enabled = true;
            s.daw.emulation = DawRemote::Emulation::Hui;
            s.daw.preset = 5;
            s.save (props);
            props.saveIfNeeded();
            const auto loaded = MidiRemote::Settings::load (props);
            expect (loaded == s);
            expect (loaded.daw.emulation == DawRemote::Emulation::Hui && loaded.outputs[MidiRemote::Daw2].isEmpty());
        }
        dir.deleteRecursively();
    }
};

static MidiTests midiTests;
