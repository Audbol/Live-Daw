#include "engine/MidiRemote.h"

#include "engine/MixerEngine.h"
#include "engine/Models.h"
#include "engine/RustStrings.h"
#include "engine/WingRemote.h"
#include "engine/Show.h"
#include "engine/Strip.h"

namespace livedaw
{

namespace
{
    // The WING's MIDI channels (1-based, as in its documentation).
    constexpr int kFaderChannel = 1, kMuteChannel = 2, kPanChannel = 3, kDcaChannel = 4, kDcaMuteChannel = 5, kCustomChannel = 6,
                  kSceneChannel = 7, kTagChannel = 8, kShowChannel = 9, kFirstFxChannel = 9;
    constexpr int kRequestAllCc = 9;
    constexpr float kFaderMinDb = -144.0f, kFaderMaxDb = 10.0f;

    const char* const routeKeys[MidiRemote::kNumFunctions] = { "enchctl", "enfxctl", "encustctl", "ensysex", "enmidicc", "enscenes", "enshowctl", "enscenetx" };
    const char* const portKeys[MidiRemote::kNumPorts] = { "din", "usb", "daw1", "daw2", "daw3" };
    constexpr const char* virtualPrefix = "Sidecar MIDI";

    int panToMidi (float pan)
    {
        const float p = juce::jlimit (-100.0f, 100.0f, pan);
        return juce::jlimit (0, 127, juce::roundToInt (64.0f + p * (p <= 0.0f ? 64.0f : 63.0f) / 100.0f));
    }

    float panFromMidi (int v)
    {
        return v <= 64 ? (float) (v - 64) * 100.0f / 64.0f : (float) (v - 64) * 100.0f / 63.0f;
    }

    int toMidi (float normalised)
    {
        return juce::jlimit (0, 127, juce::roundToInt (juce::jlimit (0.0f, 1.0f, normalised) * 127.0f));
    }

    juce::MidiMessage cc (int channel, int controller, int value)
    {
        return juce::MidiMessage::controllerEvent (channel, controller, juce::jlimit (0, 127, value));
    }
} // namespace

//==============================================================================
/** A port's open devices. The input's callback only queues the messages. */
struct MidiRemote::Device final : private juce::MidiInputCallback
{
    Device (MidiRemote& o, int p) : owner (o), port (p) {}
    ~Device() override { closeInput(); }

    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& m) override { owner.enqueue (port, m); }

    bool openInput (const juce::String& name)
    {
        closeInput();
        if (name == virtualDevice)
            input = juce::MidiInput::createNewDevice (virtualPortName (port), this);
        else
            for (auto& d : juce::MidiInput::getAvailableDevices())
                if (d.name == name)
                {
                    input = juce::MidiInput::openDevice (d.identifier, this);
                    break;
                }
        if (input == nullptr)
            return false;
        input->start();
        inputName = name;
        return true;
    }

    bool openOutput (const juce::String& name)
    {
        output.reset();
        if (name == virtualDevice)
            output = juce::MidiOutput::createNewDevice (virtualPortName (port));
        else
            for (auto& d : juce::MidiOutput::getAvailableDevices())
                if (d.name == name)
                {
                    output = juce::MidiOutput::openDevice (d.identifier);
                    break;
                }
        if (output != nullptr)
            outputName = name;
        return output != nullptr;
    }

    void closeInput()
    {
        if (input != nullptr)
            input->stop(); // no callback runs after this
        input.reset();
        inputName = {};
    }

    void closeOutput()
    {
        output.reset();
        outputName = {};
    }

    MidiRemote& owner;
    const int port;
    std::unique_ptr<juce::MidiInput> input;
    std::unique_ptr<juce::MidiOutput> output;
    juce::String inputName, outputName;
};

//==============================================================================
juce::String MidiRemote::portName (int port)
{
    static const char* names[] = { "DIN", "USB (MIDI CONTROL)", "USB (MIDI DAW 1)", "USB (MIDI DAW 2)", "USB (MIDI DAW 3)" };
    return names[juce::jlimit (0, (int) kNumPorts - 1, port)];
}

juce::String MidiRemote::functionName (int function)
{
    static const char* names[] = { "CH CONTROL (FDR/MUTE/PAN)", "FX PARAMETER CONTROL", "CUSTOM CONTROLS (RX ONLY)", "SYSEX CONTROL",
                                   "EXTERNAL MIDI CONTROL", "SCENE CHANGE", "SHOW CONTROL", "SCENE MIDI TX" };
    return names[juce::jlimit (0, (int) kNumFunctions - 1, function)];
}

juce::String MidiRemote::functionInfo (int function)
{
    switch (function)
    {
        case ChControl:
            return "FADER on MIDI CH 1, MUTE on CH 2, PAN on CH 3:\n"
                   "CC 12-31 CH 1-20, CC 44-63 CH 21-40, CC 70-77 AUX 1-8, CC 78-93 BUS 1-16,\n"
                   "CC 94/95 MAIN 1/2, CC 102/103 MAIN 3/4, CC 104-111 MTX 1-8.\n"
                   "DCA FADER on CH 4, DCA MUTE and MUTE GROUPS on CH 5:\n"
                   "CC 12-27 DCA 1-16, CC 28-31 MUTE GROUP 1-4, CC 44-47 MUTE GROUP 5-8.\n"
                   "CC 9 requests all values. MUTE: 0 off, 127 on. PAN: 0 left, 64 centre, 127 right.\n"
                   "Changes on the console are sent back.";
        case FxControl:
            return "FX 1-8 on MIDI CH 9-16: CC 12 INSERT ON, CC 13 FX MIX, CC 14 FX MODEL,\n"
                   "CC 15-31 PARAMETERS 1-17, CC 44-58 PARAMETERS 18-32.\n"
                   "FX 9-16 on MIDI CH 9-16: CC 70 INSERT ON, CC 71 FX MIX, CC 72 FX MODEL,\n"
                   "CC 73-95 PARAMETERS 1-23, CC 102-111 PARAMETERS 24-33.\n"
                   "CH 9 CC 9 with value 1-16 requests the values of FX 1-16 (0: all).";
        case CustomControls:
            return "MIDI CH 6, layers 1-16 (receive only):\n"
                   "CC 12-31 the encoders of layers 1-5, CC 44-63 layers 6-10, CC 70-93 layers 11-16.\n"
                   "NOTE 0-127: the keys, 8 per layer (upper row, then lower row).";
        case SysexControl:
            return "F0 00 20 32 57 <cmd> <data> F7:\n"
                   "00 identity, 02 execute (\"/ch.1.fdr=-1,mute=0\"), 03 dump a node, 05 describe a node.\n"
                   "Replies: 01 identity, 04 / 06 OK with data, 07 error text.";
        case ExternalMidi:
            return "The custom controls' MIDI functions (MIDI CC encoders, MIDI CC / NOTE / PGM keys)\n"
                   "send on this port; values received on it update their state.";
        case SceneChange:
            return "MIDI CH 7: PROGRAM CHANGE 1-128 recalls scene 1-128;\n"
                   "with more than 128 scenes, CC 0 (bank MSB) selects scenes 129-256, 257-384...";
        case ShowControl:
            return "MIDI CH 8: PROGRAM CHANGE 1-128 recalls the scene tagged #1-#128 (CC 0: bank MSB).\n"
                   "MIDI CH 9: PROGRAM CHANGE 1 GO, 2 PREV, 3 NEXT, 4 GO PREV, 5 GO NEXT.";
        case SceneTx:
            return "Every scene recall is sent as MIDI CH 7 CC 0 (bank) + PROGRAM CHANGE,\n"
                   "so another console (SCENE CHANGE) follows.";
        default:
            return {};
    }
}

juce::String MidiRemote::virtualPortName (int port)
{
    static const char* names[] = { "DIN", "Control", "DAW 1", "DAW 2", "DAW 3" };
    return juce::String (virtualPrefix) + " " + names[juce::jlimit (0, (int) kNumPorts - 1, port)];
}

//==============================================================================
MidiRemote::Settings::Settings()
{
    routes.fill (Off);
    // The WING's USB ports become the app's virtual ports.
    for (int p = Usb; p < kNumPorts; ++p)
        inputs[(size_t) p] = outputs[(size_t) p] = virtualDevice;
}

MidiRemote::Settings MidiRemote::Settings::load (juce::PropertiesFile& p)
{
    Settings s;
    for (int f = 0; f < kNumFunctions; ++f)
        s.routes[(size_t) f] = juce::jlimit (0, 2, p.getIntValue (juce::String ("midi.") + routeKeys[f], Off));
    for (int port = 0; port < kNumPorts; ++port)
    {
        const juce::String key = juce::String ("midi.") + portKeys[port];
        s.inputs[(size_t) port] = p.getValue (key + ".in", s.inputs[(size_t) port]);
        s.outputs[(size_t) port] = p.getValue (key + ".out", s.outputs[(size_t) port]);
    }
    s.daw = DawRemote::Settings::load (p);
    return s;
}

void MidiRemote::Settings::save (juce::PropertiesFile& p) const
{
    for (int f = 0; f < kNumFunctions; ++f)
        p.setValue (juce::String ("midi.") + routeKeys[f], routes[(size_t) f]);
    for (int port = 0; port < kNumPorts; ++port)
    {
        const juce::String key = juce::String ("midi.") + portKeys[port];
        p.setValue (key + ".in", inputs[(size_t) port]);
        p.setValue (key + ".out", outputs[(size_t) port]);
    }
    daw.save (p);
}

bool MidiRemote::Settings::operator== (const Settings& o) const
{
    return routes == o.routes && inputs == o.inputs && outputs == o.outputs && daw == o.daw;
}

//==============================================================================
MidiRemote::MidiRemote (MixerEngine& e, Show& s, bool devicesOn) : engine (e), show (s), useDevices (devicesOn)
{
    for (int p = 0; p < kNumPorts; ++p)
        devices[(size_t) p] = std::make_unique<Device> (*this, p);
    for (auto& a : lastChannelValues)
        a.fill (-1);
    for (auto& a : lastFxValues)
        a.fill (-1);
    for (auto& b : banks)
        b.fill (0);
    daw.output = [this] (int unit, const juce::MidiMessage& m) { send (dawPort (unit), m); };
    show.onSceneMidi = [this] (const juce::StringArray& texts)
    {
        for (auto& t : texts)
            sendMidiText (t);
    };

    // SYSEX CONTROL works on the WING parameter tree (livedaw-wing).
    auto wingSysex = [this] (int command, const juce::String& text, juce::String& result)
    {
        ld::LdString* out = nullptr;
        const char* utf8 = text.toRawUTF8();
        const bool ok = ld::ld_wing_sysex (engine.getWing().getHandle(), command, utf8, std::strlen (utf8), &out);
        result = takeString (out);
        return ok;
    };
    sysexExecute = [wingSysex] (const juce::String& text)
    {
        juce::String result;
        return wingSysex (2, text, result) ? juce::String() : result;
    };
    sysexDump = [wingSysex] (const juce::String& text) -> std::optional<juce::String>
    {
        juce::String result;
        return wingSysex (3, text, result) ? std::optional<juce::String> (result) : std::nullopt;
    };
    sysexDescribe = [wingSysex] (const juce::String& text) -> std::optional<juce::String>
    {
        juce::String result;
        return wingSysex (5, text, result) ? std::optional<juce::String> (result) : std::nullopt;
    };
    startTimerHz (25);
}

MidiRemote::~MidiRemote()
{
    show.onSceneMidi = nullptr;
    stopTimer();
    deviceListConnection.reset();
    closeDevices();
    cancelPendingUpdate();
}

void MidiRemote::apply (const Settings& s)
{
    const bool routesChanged = s.routes != settings.routes;
    settings = s;
    daw.setSettings (s.daw);
    if (routesChanged)
    {
        primeChannels = primeFx = true;
        lastSceneIndex = -2;
    }
    openDevices();
}

//==============================================================================
juce::StringArray MidiRemote::getInputDevices()
{
    juce::StringArray names;
    for (auto& d : juce::MidiInput::getAvailableDevices())
        if (! d.name.startsWith (virtualPrefix))
            names.addIfNotAlreadyThere (d.name);
    return names;
}

juce::StringArray MidiRemote::getOutputDevices()
{
    juce::StringArray names;
    for (auto& d : juce::MidiOutput::getAvailableDevices())
        if (! d.name.startsWith (virtualPrefix))
            names.addIfNotAlreadyThere (d.name);
    return names;
}

bool MidiRemote::canCreateVirtualPorts()
{
   #if JUCE_LINUX || JUCE_MAC || JUCE_BSD
    return true;
   #else
    return false;
   #endif
}

MidiRemote::PortStatus MidiRemote::getPortStatus (int port) const
{
    return juce::isPositiveAndBelow (port, (int) kNumPorts) ? status[(size_t) port] : PortStatus {};
}

void MidiRemote::openDevices()
{
    if (! useDevices)
        return;
    if (! listening)
    {
        // Hot-plug: reopen the configured devices when the device list changes.
        deviceListConnection = juce::MidiDeviceListConnection::make ([this] { openDevices(); });
        listening = true;
    }

    for (int p = 0; p < kNumPorts; ++p)
    {
        bool needed = false;
        if (p == Din || p == Usb)
            for (auto r : settings.routes)
                needed = needed || r == (p == Din ? RouteDin : RouteUsb);
        if (settings.daw.enabled && ((p == Din && ! settings.daw.usb) || (p >= Daw1 && settings.daw.usb && p - Daw1 < daw.getNumUnits())))
            needed = true;

        auto& d = *devices[(size_t) p];
        auto& st = status[(size_t) p];
        const auto& wantIn = settings.inputs[(size_t) p];
        const auto& wantOut = settings.outputs[(size_t) p];
        if (! needed || wantIn.isEmpty() || d.inputName != wantIn)
            d.closeInput();
        if (! needed || wantOut.isEmpty() || d.outputName != wantOut)
            d.closeOutput();
        if (needed && wantIn.isNotEmpty() && d.input == nullptr)
            d.openInput (wantIn);
        if (needed && wantOut.isNotEmpty() && d.output == nullptr)
            d.openOutput (wantOut);
        st.inputOpen = d.input != nullptr;
        st.outputOpen = d.output != nullptr;
        st.inputMissing = needed && wantIn.isNotEmpty() && ! st.inputOpen;
        st.outputMissing = needed && wantOut.isNotEmpty() && ! st.outputOpen;
    }
}

void MidiRemote::closeDevices()
{
    for (auto& d : devices)
        if (d != nullptr)
        {
            d->closeInput();
            d->closeOutput();
        }
}

void MidiRemote::enqueue (int port, const juce::MidiMessage& m)
{
    {
        const juce::ScopedLock sl (queueLock);
        queue.emplace_back (port, m);
    }
    triggerAsyncUpdate();
}

void MidiRemote::handleAsyncUpdate()
{
    {
        const juce::ScopedLock sl (queueLock);
        draining.swap (queue);
    }
    for (auto& [port, message] : draining)
        handleMessage (port, message);
    draining.clear();
}

void MidiRemote::timerCallback()
{
    sendFeedback();
    daw.tick (juce::Time::getMillisecondCounter());
    // Devices that went away: try again every two seconds (hot-plug).
    if (++ticks % 50 == 0)
        for (auto& st : status)
            if (st.inputMissing || st.outputMissing)
            {
                openDevices();
                break;
            }
}

void MidiRemote::send (int port, const juce::MidiMessage& m)
{
    if (! juce::isPositiveAndBelow (port, (int) kNumPorts))
        return;
    ++status[(size_t) port].sent;
    if (onSend)
        onSend (port, m);
    if (auto* out = devices[(size_t) port]->output.get())
        out->sendMessageNow (m);
}

int MidiRemote::routeOf (int port) const noexcept
{
    return port == Din ? RouteDin : port == Usb ? RouteUsb : Off;
}

bool MidiRemote::routed (int function, int port) const noexcept
{
    const int r = routeOf (port);
    return r != Off && settings.routes[(size_t) function] == r;
}

int MidiRemote::portOf (int function) const noexcept
{
    switch (settings.routes[(size_t) juce::jlimit (0, (int) kNumFunctions - 1, function)])
    {
        case RouteDin: return Din;
        case RouteUsb: return Usb;
        default:       return -1;
    }
}

int MidiRemote::dawPort (int unit) const noexcept
{
    return settings.daw.usb ? Daw1 + unit : Din;
}

int MidiRemote::dawUnit (int port) const noexcept
{
    if (! settings.daw.enabled)
        return -1;
    if (! settings.daw.usb)
        return port == Din ? 0 : -1;
    const int unit = port - Daw1;
    return juce::isPositiveAndBelow (unit, daw.getNumUnits()) ? unit : -1;
}

//==============================================================================
void MidiRemote::handleMessage (int port, const juce::MidiMessage& m)
{
    if (! juce::isPositiveAndBelow (port, (int) kNumPorts))
        return;
    ++status[(size_t) port].received;

    if (const int unit = dawUnit (port); unit >= 0)
    {
        daw.handleMessage (unit, m);
        return;
    }
    if (m.isSysEx())
    {
        if (routed (SysexControl, port))
            handleSysex (port, m);
        return;
    }
    if (routed (ExternalMidi, port))
        handleExternal (m);

    const int ch = m.getChannel();
    if (ch >= kFaderChannel && ch <= kDcaMuteChannel && routed (ChControl, port) && m.isController())
        handleChannelControl (port, m);
    else if (ch == kCustomChannel && routed (CustomControls, port))
        handleCustom (m);
    else if (ch == kSceneChannel || ch == kTagChannel || (ch == kShowChannel && m.isProgramChange()))
        handleScenes (port, m);
    if (ch >= kFirstFxChannel && routed (FxControl, port) && m.isController())
        handleFx (port, m);
}

//==============================================================================
float MidiRemote::faderPosition (float db) noexcept
{
    // Four linear segments meeting at -60, -30 and -10 dB; 0 dB at 0.75,
    // +10 dB at the top and -90 dB at the bottom (the WING / X32 fader law).
    const float d = juce::jlimit (kFaderMinDb, kFaderMaxDb, db);
    float pos;
    if (d >= -10.0f)
        pos = (d + 30.0f) / 40.0f;
    else if (d >= -30.0f)
        pos = (d + 50.0f) / 80.0f;
    else if (d >= -60.0f)
        pos = (d + 70.0f) / 160.0f;
    else
        pos = (d + 90.0f) / 480.0f;
    return juce::jlimit (0.0f, 1.0f, pos);
}

float MidiRemote::faderDb (float position) noexcept
{
    const float p = juce::jlimit (0.0f, 1.0f, position);
    if (p >= 0.5f)
        return p * 40.0f - 30.0f;
    if (p >= 0.25f)
        return p * 80.0f - 50.0f;
    if (p >= 0.0625f)
        return p * 160.0f - 70.0f;
    return p > 0.0f ? p * 480.0f - 90.0f : kFaderMinDb;
}

void MidiRemote::refreshCcStrips() const
{
    if (ccStripsVersion == engine.getStructureVersion())
        return;
    ccStripsVersion = engine.getStructureVersion();
    ccStrips.fill (nullptr);
    auto find = [this] (StripKind kind, int number) -> Strip*
    {
        for (auto& s : engine.getStrips (kind))
            if (s->getNumber() == number)
                return s.get();
        return nullptr;
    };
    for (int n = 1; n <= 20; ++n)
    {
        ccStrips[(size_t) (11 + n)] = find (StripKind::Channel, n);
        ccStrips[(size_t) (43 + n)] = find (StripKind::Channel, 20 + n);
    }
    for (int n = 1; n <= 8; ++n)
    {
        ccStrips[(size_t) (69 + n)] = find (StripKind::Aux, n);
        ccStrips[(size_t) (103 + n)] = find (StripKind::Matrix, n);
    }
    for (int n = 1; n <= 16; ++n)
        ccStrips[(size_t) (77 + n)] = find (StripKind::Bus, n);
    ccStrips[94] = find (StripKind::Main, 1);
    ccStrips[95] = find (StripKind::Main, 2);
    ccStrips[102] = find (StripKind::Main, 3);
    ccStrips[103] = find (StripKind::Main, 4);
}

Strip* MidiRemote::stripForCc (int controller) const
{
    if (! juce::isPositiveAndBelow (controller, 128))
        return nullptr;
    refreshCcStrips();
    return ccStrips[(size_t) controller];
}

int MidiRemote::ccForStrip (const Strip& s) const
{
    refreshCcStrips();
    for (size_t i = 0; i < ccStrips.size(); ++i)
        if (ccStrips[i] == &s)
            return (int) i;
    return -1;
}

int MidiRemote::currentValue (int midiChannel, int controller) const
{
    switch (midiChannel)
    {
        case kFaderChannel:
        case kMuteChannel:
        case kPanChannel:
        {
            auto* s = stripForCc (controller);
            if (s == nullptr)
                return -1;
            if (midiChannel == kFaderChannel)
                return toMidi (faderPosition (s->params.faderDb.load()));
            if (midiChannel == kMuteChannel)
                return s->params.mute.load() ? 127 : 0;
            return panToMidi (s->params.pan.load());
        }
        case kDcaChannel:
            if (controller >= 12 && controller < 12 + kNumDcas)
                return toMidi (faderPosition (engine.getDca (controller - 12).faderDb.load()));
            return -1;
        case kDcaMuteChannel:
            if (controller >= 12 && controller < 12 + kNumDcas)
                return engine.getDca (controller - 12).mute.load() ? 127 : 0;
            if ((controller >= 28 && controller <= 31) || (controller >= 44 && controller <= 47))
            {
                const int group = controller <= 31 ? controller - 28 : controller - 40;
                return ((engine.muteGroups.load() >> group) & 1u) != 0 ? 127 : 0;
            }
            return -1;
        default:
            return -1;
    }
}

void MidiRemote::applyValue (int midiChannel, int controller, int value)
{
    const bool on = value >= 64;
    switch (midiChannel)
    {
        case kFaderChannel:
        case kMuteChannel:
        case kPanChannel:
        {
            auto* s = stripForCc (controller);
            if (s == nullptr)
                return;
            if (midiChannel == kFaderChannel)
                s->params.faderDb = faderDb ((float) value / 127.0f);
            else if (midiChannel == kMuteChannel)
                s->params.mute = on;
            else
                s->params.pan = panFromMidi (value);
            break;
        }
        case kDcaChannel:
            if (controller >= 12 && controller < 12 + kNumDcas)
                engine.getDca (controller - 12).faderDb = faderDb ((float) value / 127.0f);
            break;
        case kDcaMuteChannel:
            if (controller >= 12 && controller < 12 + kNumDcas)
                engine.getDca (controller - 12).mute = on;
            else if ((controller >= 28 && controller <= 31) || (controller >= 44 && controller <= 47))
            {
                const auto bit = 1u << (uint32_t) (controller <= 31 ? controller - 28 : controller - 40);
                const auto groups = engine.muteGroups.load();
                engine.muteGroups = on ? groups | bit : groups & ~bit;
            }
            break;
        default:
            break;
    }
    // What the console holds now is what the sender knows: no echo.
    lastChannelValues[(size_t) (midiChannel - 1)][(size_t) controller] = currentValue (midiChannel, controller);
}

void MidiRemote::handleChannelControl (int, const juce::MidiMessage& m)
{
    const int ch = m.getChannel(), controller = m.getControllerNumber();
    if (controller == kRequestAllCc)
    {
        sendAllChannelControl (ch);
        return;
    }
    applyValue (ch, controller, m.getControllerValue());
}

void MidiRemote::sendAllChannelControl (int midiChannel)
{
    const int port = portOf (ChControl);
    if (port < 0)
        return;
    for (int c = 0; c < 128; ++c)
        if (const int v = currentValue (midiChannel, c); v >= 0)
        {
            send (port, cc (midiChannel, c, v));
            lastChannelValues[(size_t) (midiChannel - 1)][(size_t) c] = v;
        }
}

//==============================================================================
bool MidiRemote::fxControl (int channel, int controller, int& slot, int& param) noexcept
{
    if (channel < kFirstFxChannel || channel > 16)
        return false;
    const int base = channel - kFirstFxChannel;
    if (controller >= 12 && controller <= 58)
    {
        slot = base;
        if (controller == 12)
            param = -3;
        else if (controller == 13)
            param = -1;
        else if (controller == 14)
            param = -2;
        else if (controller <= 31)
            param = controller - 15;      // 1-17
        else if (controller >= 44)
            param = controller - 44 + 17; // 18-32
        else
            return false;
        return true;
    }
    if (controller >= 70 && controller <= 111)
    {
        slot = base + 8;
        if (controller == 70)
            param = -3;
        else if (controller == 71)
            param = -1;
        else if (controller == 72)
            param = -2;
        else if (controller <= 95)
            param = controller - 73;       // 1-23
        else if (controller >= 102)
            param = controller - 102 + 23; // 24-33
        else
            return false;
        return true;
    }
    return false;
}

int MidiRemote::fxCc (int slot, int param) noexcept
{
    if (! juce::isPositiveAndBelow (slot, kNumFx))
        return -1;
    const bool low = slot < 8;
    switch (param)
    {
        case -3: return low ? 12 : 70;
        case -1: return low ? 13 : 71;
        case -2: return low ? 14 : 72;
        default: break;
    }
    if (param < 0)
        return -1;
    if (low)
        return param < 17 ? 15 + param : param < 32 ? 44 + param - 17 : -1;
    return param < 23 ? 73 + param : param < 33 ? 102 + param - 23 : -1;
}

int MidiRemote::currentFxValue (int slot, int param) const
{
    auto& fx = engine.getFx();
    auto& p = fx.params (slot);
    switch (param)
    {
        case -3: return p.bypass.load() ? 0 : 127;
        case -1: return toMidi (p.mix.load() / 100.0f);
        case -2: return juce::jlimit (0, 127, p.model.load());
        default: break;
    }
    const auto& spec = models::model (models::Fx, p.model.load());
    if (! juce::isPositiveAndBelow (param, (int) spec.params.size()) || param >= ld::kMaxFxParams)
        return -1;
    return toMidi (spec.params[(size_t) param].toNormalised (p.values[param].load()));
}

void MidiRemote::handleFx (int, const juce::MidiMessage& m)
{
    const int controller = m.getControllerNumber(), value = m.getControllerValue();
    if (m.getChannel() == kFirstFxChannel && controller == kRequestAllCc)
    {
        // FX data request: 1-16 one FX, 0 all of them.
        if (value >= 1 && value <= kNumFx)
            sendAllFx (value - 1);
        else
            for (int s = 0; s < kNumFx; ++s)
                sendAllFx (s);
        return;
    }
    int slot = 0, param = 0;
    if (! fxControl (m.getChannel(), controller, slot, param))
        return;
    auto& fx = engine.getFx();
    auto& p = fx.params (slot);
    switch (param)
    {
        case -3: p.bypass = value < 64; break;
        case -1: p.mix = (float) value * 100.0f / 127.0f; break;
        case -2:
            if (value != ld::kFxExt && value < (int) models::table (models::Fx).size() && FxSlots::isAllowed (value, slot))
                fx.setModel (slot, value);
            break;
        default:
        {
            const auto& spec = models::model (models::Fx, p.model.load());
            if (juce::isPositiveAndBelow (param, (int) spec.params.size()) && param < ld::kMaxFxParams)
                p.values[param] = spec.params[(size_t) param].fromNormalised ((float) value / 127.0f);
            break;
        }
    }
    lastFxValues[(size_t) slot][(size_t) controller] = currentFxValue (slot, param);
}

void MidiRemote::sendAllFx (int slot)
{
    const int port = portOf (FxControl);
    if (port < 0 || ! juce::isPositiveAndBelow (slot, kNumFx))
        return;
    const int channel = kFirstFxChannel + slot % 8;
    for (int param = -3; param < 33; ++param)
        if (const int c = fxCc (slot, param); c >= 0)
            if (const int v = currentFxValue (slot, param); v >= 0)
            {
                send (port, cc (channel, c, v));
                lastFxValues[(size_t) slot][(size_t) c] = v;
            }
}

//==============================================================================
bool MidiRemote::customEncoder (int controller, int& layer, int& control) noexcept
{
    int first = -1;
    if (controller >= 12 && controller <= 31)
        first = 12, layer = 0;
    else if (controller >= 44 && controller <= 63)
        first = 44, layer = 5;
    else if (controller >= 70 && controller <= 93)
        first = 70, layer = 10;
    if (first < 0)
        return false;
    layer += (controller - first) / 4;
    control = (controller - first) % 4;
    return true;
}

int MidiRemote::customEncoderCc (int layer, int control) noexcept
{
    if (! juce::isPositiveAndBelow (layer, 16) || ! juce::isPositiveAndBelow (control, 4))
        return -1;
    const int base = layer < 5 ? 12 + layer * 4 : layer < 10 ? 44 + (layer - 5) * 4 : 70 + (layer - 10) * 4;
    return base + control;
}

void MidiRemote::handleCustom (const juce::MidiMessage& m)
{
    if (m.isController())
    {
        int layer = 0, control = 0;
        if (customEncoder (m.getControllerNumber(), layer, control) && onCustomEncoder)
            onCustomEncoder (layer, control, m.getControllerValue());
    }
    else if (m.isNoteOnOrOff() && onCustomKey)
    {
        const int note = m.getNoteNumber();
        onCustomKey (note / 8, note % 8, m.isNoteOn());
    }
}

//==============================================================================
void MidiRemote::recall (int scene)
{
    if (! juce::isPositiveAndBelow (scene, show.getNumScenes()))
        return;
    const auto result = show.recallScene (scene);
    lastSceneIndex = show.getCurrentSceneIndex(); // not sent back out (SCENE MIDI TX)
    sceneCursor = -1;
    if (onRecalled)
        onRecalled ("scene " + juce::String (scene + 1) + " (MIDI)", result);
}

void MidiRemote::handleScenes (int port, const juce::MidiMessage& m)
{
    const int ch = m.getChannel();
    const auto p = (size_t) port;
    if (ch == kSceneChannel && routed (SceneChange, port))
    {
        if (m.isController() && m.getControllerNumber() == 0)
            banks[p][0] = m.getControllerValue();
        else if (m.isProgramChange())
        {
            // Bank MSB only counts when the show has more than 128 scenes.
            const int bank = show.getNumScenes() > 128 ? banks[p][0] : 0;
            recall (bank * 128 + m.getProgramChangeNumber());
        }
    }
    else if (ch == kTagChannel && routed (ShowControl, port))
    {
        if (m.isController() && m.getControllerNumber() == 0)
            banks[p][1] = m.getControllerValue();
        else if (m.isProgramChange())
        {
            const int tag = banks[p][1] * 128 + m.getProgramChangeNumber() + 1;
            const int scene = show.findSceneByTag (tag);
            if (scene >= 0)
                recall (scene);
        }
    }
    else if (ch == kShowChannel && routed (ShowControl, port) && m.isProgramChange())
    {
        // GO recalls the scene PREV / NEXT picked (else the next one).
        const int current = show.getCurrentSceneIndex();
        const int cursor = sceneCursor >= 0 ? sceneCursor : current + 1;
        const int last = show.getNumScenes() - 1;
        switch (m.getProgramChangeNumber())
        {
            case 0: recall (juce::jlimit (0, juce::jmax (0, last), cursor)); break;
            case 1: sceneCursor = juce::jlimit (0, juce::jmax (0, last), cursor - 1); break;
            case 2: sceneCursor = juce::jlimit (0, juce::jmax (0, last), cursor + 1); break;
            case 3:
            case 4:
            {
                const auto result = m.getProgramChangeNumber() == 3 ? show.recallPrevious() : show.recallNext();
                lastSceneIndex = show.getCurrentSceneIndex();
                sceneCursor = -1;
                if (onRecalled)
                    onRecalled (m.getProgramChangeNumber() == 3 ? "previous scene (MIDI)" : "next scene (MIDI)", result);
                break;
            }
            default: break;
        }
    }
}

//==============================================================================
juce::String MidiRemote::getIdentity() const
{
    return "WING," + juce::IPAddress::getLocalAddress().toString() + "," + consoleName + ",ngc-full,SIDECAR,"
           + juce::String (JUCE_APPLICATION_VERSION_STRING) + ":sidecar";
}

void MidiRemote::handleSysex (int port, const juce::MidiMessage& m)
{
    const auto* d = m.getSysExData();
    const int size = m.getSysExDataSize();
    // F0 00 20 32 57 <cmd> <data> F7 (Behringer, WING).
    if (size < 5 || d[0] != 0x00 || d[1] != 0x20 || d[2] != 0x32 || d[3] != 0x57)
        return;
    const juce::String text (juce::CharPointer_UTF8 (reinterpret_cast<const char*> (d + 5)), (size_t) (size - 5));
    auto reply = [this, port] (int command, const juce::String& data)
    {
        juce::MemoryBlock block;
        const juce::uint8 header[] = { 0x00, 0x20, 0x32, 0x57, (juce::uint8) command };
        block.append (header, sizeof (header));
        // Sysex data is 7-bit.
        for (auto c : data.toStdString())
            block.append (&c, 1);
        for (size_t i = sizeof (header); i < block.getSize(); ++i)
            block[i] = (char) (block[i] & 0x7f);
        send (port, juce::MidiMessage::createSysExMessage (block.getData(), (int) block.getSize()));
    };
    switch (d[4])
    {
        case 0x00: reply (0x01, getIdentity()); break;
        case 0x02:
            if (const auto error = sysexExecute ? sysexExecute (text) : juce::String ("NODE NOT FOUND"); error.isNotEmpty())
                reply (0x07, error);
            break;
        case 0x03:
        case 0x05:
        {
            const auto& fn = d[4] == 0x03 ? sysexDump : sysexDescribe;
            if (const auto data = fn ? fn (text) : std::optional<juce::String>(); data.has_value())
                reply (d[4] + 1, *data);
            else
                reply (0x07, "NODE NOT FOUND");
            break;
        }
        default: break;
    }
}

//==============================================================================
bool MidiRemote::sendExternal (const juce::MidiMessage& m)
{
    const int port = portOf (ExternalMidi);
    if (port < 0)
        return false;
    send (port, m);
    return true;
}

bool MidiRemote::parseMidiText (const juce::String& text, std::vector<PortMessage>& out, juce::String& error)
{
    out.clear();
    std::vector<juce::uint8> bytes;
    int port = -1;
    // The bytes since the last port prefix, split into messages at their status bytes.
    auto flush = [&]
    {
        for (size_t i = 0; i < bytes.size();)
        {
            const int status = bytes[i];
            if (status < 0x80)
            {
                error = "DATA BYTE WITHOUT A STATUS BYTE";
                return false;
            }
            size_t length = 0;
            if (status == 0xf0)
            {
                const auto end = std::find (bytes.begin() + (std::ptrdiff_t) i, bytes.end(), (juce::uint8) 0xf7);
                if (end == bytes.end())
                {
                    error = "SYSEX WITHOUT F7";
                    return false;
                }
                length = (size_t) (end - bytes.begin()) + 1 - i;
            }
            else
                length = (size_t) juce::MidiMessage::getMessageLengthFromFirstByte ((juce::uint8) status);
            if (i + length > bytes.size())
            {
                error = "INCOMPLETE MESSAGE";
                return false;
            }
            for (size_t k = i + 1; k < i + length - (status == 0xf0 ? 1 : 0); ++k)
                if (bytes[k] >= 0x80)
                {
                    error = "INCOMPLETE MESSAGE";
                    return false;
                }
            out.push_back ({ port, juce::MidiMessage (bytes.data() + i, (int) length) });
            i += length;
        }
        bytes.clear();
        return true;
    };

    juce::StringArray tokens;
    tokens.addTokens (text.toUpperCase(), ", \t\r\n", {});
    tokens.removeEmptyStrings();
    for (auto token : tokens)
    {
        if (token.startsWith ("U:") || token.startsWith ("D:"))
        {
            if (! flush())
                return false;
            port = token[0] == 'U' ? Usb : Din;
            token = token.substring (2);
        }
        if (token.length() % 2 != 0 || ! token.containsOnly ("0123456789ABCDEF"))
        {
            error = "NOT HEXADECIMAL BYTES: " + token;
            return false;
        }
        for (int i = 0; i < token.length(); i += 2)
            bytes.push_back ((juce::uint8) token.substring (i, i + 2).getHexValue32());
    }
    return flush();
}

bool MidiRemote::sendMidiText (const juce::String& text)
{
    const int port = portOf (ExternalMidi);
    std::vector<PortMessage> messages;
    juce::String error;
    if (port < 0 || ! parseMidiText (text, messages, error))
        return false;
    for (auto& m : messages)
        send (m.port >= 0 ? m.port : port, m.message);
    return ! messages.empty();
}

int MidiRemote::getExternalCc (int channel, int controller) const noexcept
{
    if (! juce::isPositiveAndBelow (channel - 1, 16) || ! juce::isPositiveAndBelow (controller, 128))
        return 0;
    return externalCc[(size_t) (channel - 1)][(size_t) controller];
}

void MidiRemote::setExternalCc (int channel, int controller, int value)
{
    if (juce::isPositiveAndBelow (channel - 1, 16) && juce::isPositiveAndBelow (controller, 128))
        externalCc[(size_t) (channel - 1)][(size_t) controller] = (juce::uint8) juce::jlimit (0, 127, value);
}

bool MidiRemote::getExternalToggle (int channel, int number, bool isNote) const noexcept
{
    if (! juce::isPositiveAndBelow (channel - 1, 16) || ! juce::isPositiveAndBelow (number, 128))
        return false;
    return externalToggles[(size_t) (channel - 1)][(size_t) (number + (isNote ? 128 : 0))];
}

void MidiRemote::setExternalToggle (int channel, int number, bool isNote, bool on) noexcept
{
    if (juce::isPositiveAndBelow (channel - 1, 16) && juce::isPositiveAndBelow (number, 128))
        externalToggles[(size_t) (channel - 1)][(size_t) (number + (isNote ? 128 : 0))] = on;
}

void MidiRemote::handleExternal (const juce::MidiMessage& m)
{
    // A device's own state comes back: the MIDI keys and encoders follow it.
    if (m.isController())
    {
        setExternalCc (m.getChannel(), m.getControllerNumber(), m.getControllerValue());
        setExternalToggle (m.getChannel(), m.getControllerNumber(), false, m.getControllerValue() >= 64);
    }
    else if (m.isNoteOnOrOff())
        setExternalToggle (m.getChannel(), m.getNoteNumber(), true, m.isNoteOn());
}

//==============================================================================
void MidiRemote::resendAll()
{
    for (auto& a : lastChannelValues)
        a.fill (-1);
    for (auto& a : lastFxValues)
        a.fill (-1);
    primeChannels = primeFx = false;
}

void MidiRemote::sendFeedback()
{
    // CH CONTROL: faders, mutes, pans, DCAs and mute groups.
    if (const int port = portOf (ChControl); port >= 0)
    {
        for (int ch = kFaderChannel; ch <= kDcaMuteChannel; ++ch)
        {
            auto& last = lastChannelValues[(size_t) (ch - 1)];
            for (int c = 12; c < 112; ++c)
            {
                const int v = currentValue (ch, c);
                if (v < 0 || v == last[(size_t) c])
                    continue;
                if (! primeChannels)
                    send (port, cc (ch, c, v));
                last[(size_t) c] = v;
            }
        }
        primeChannels = false;
    }

    // FX PARAMETER CONTROL.
    if (const int port = portOf (FxControl); port >= 0)
    {
        for (int slot = 0; slot < kNumFx; ++slot)
        {
            auto& last = lastFxValues[(size_t) slot];
            for (int param = -3; param < 33; ++param)
            {
                const int c = fxCc (slot, param);
                if (c < 0)
                    continue;
                const int v = currentFxValue (slot, param);
                if (v < 0 || v == last[(size_t) c])
                    continue;
                if (! primeFx)
                    send (port, cc (kFirstFxChannel + slot % 8, c, v));
                last[(size_t) c] = v;
            }
        }
        primeFx = false;
    }

    // SCENE MIDI TX: a recall made on the console goes out as a program change.
    const int scene = show.getCurrentSceneIndex();
    if (scene != lastSceneIndex)
    {
        const int port = portOf (SceneTx);
        if (port >= 0 && lastSceneIndex != -2 && scene >= 0)
        {
            send (port, cc (kSceneChannel, 0, scene / 128));
            send (port, juce::MidiMessage::programChange (kSceneChannel, scene % 128));
        }
        lastSceneIndex = scene;
    }
}

} // namespace livedaw
