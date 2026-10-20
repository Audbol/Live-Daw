#pragma once

#include "engine/ConsoleState.h"
#include "engine/DawRemote.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_data_structures/juce_data_structures.h>

#include <array>
#include <functional>
#include <memory>
#include <optional>

namespace livedaw
{

class MixerEngine;
class Show;
class Strip;

/** MIDI on the console (WING SETUP > REMOTE and DAW, WING MIDI documentation
    and "WING Remote Protocols"):

    - Ports. The WING's DIN socket and its USB MIDI ports (MIDI Control, MIDI
      DAW 1-3) become MIDI devices of this computer: each port has an input
      and an output, a device chosen on SETUP or a virtual port the app
      creates ("Sidecar MIDI Control", "Sidecar MIDI DAW 1"...) where the
      platform allows it. Devices that disappear are reopened when they come
      back.
    - MIDI REMOTE CONTROL. Each function is OFF, DIN or USB, as on the WING:
      CH CONTROL (faders on MIDI channel 1, mutes on 2, pans on 3, DCAs on 4,
      DCA mutes and mute groups on 5), FX PARAMETER CONTROL (channels 9-16),
      CUSTOM CONTROLS (RX ONLY, channel 6), SYSEX CONTROL (the WING's sysex
      commands), EXTERNAL MIDI CONTROL (the custom controls' MIDI functions
      send here), SCENE CHANGE (channel 7: bank + program change), SHOW
      CONTROL (channel 8: scene tags; channel 9: GO / PREV / NEXT) and SCENE
      MIDI TX (scene recalls go out as program changes on channel 7). Console
      changes made anywhere are sent back out (feedback) for CH CONTROL and FX
      PARAMETER CONTROL.
    - DAW remote control (DawRemote) on MIDI DAW 1-3, or DIN.

    Threading: MIDI input callbacks only queue the messages; everything else,
    including applying them to the console, runs on the message thread.
    Console parameters are the engine's shared atomics, as the UI and the WING
    remote write them. Message thread only (apart from the device callbacks). */
class MidiRemote final : private juce::AsyncUpdater, private juce::Timer
{
public:
    /** `useDevices` false: no MIDI devices are opened (tests feed
        handleMessage() and read onSend). */
    MidiRemote (MixerEngine&, Show&, bool useDevices = true);
    ~MidiRemote() override;

    /** The WING's MIDI ports. USB is the WING's "MIDI Control" port. */
    enum Port
    {
        Din,
        Usb,
        Daw1,
        Daw2,
        Daw3,
        kNumPorts
    };
    static juce::String portName (int port);

    /** The MIDI REMOTE CONTROL rows (the /$ctl/midi nodes). */
    enum Function
    {
        ChControl,      // CH CONTROL (FDR/MUTE/PAN)
        FxControl,      // FX PARAMETER CONTROL
        CustomControls, // CUSTOM CONTROLS (RX ONLY)
        SysexControl,   // SYSEX CONTROL
        ExternalMidi,   // EXTERNAL MIDI CONTROL
        SceneChange,    // SCENE CHANGE
        ShowControl,    // SHOW CONTROL
        SceneTx,        // SCENE MIDI TX
        kNumFunctions
    };
    static juce::String functionName (int function);
    /** What the ⓘ of a row explains: the MIDI channels, CCs and notes it uses. */
    static juce::String functionInfo (int function);

    /** OFF | DIN | USB. */
    enum Route
    {
        Off,
        RouteDin,
        RouteUsb
    };

    /** The value of a port's device setting that means the app's virtual port. */
    static constexpr const char* virtualDevice = "<virtual>";

    /** SETUP > REMOTE and DAW, kept in the app settings (`midi.*`, `daw.*`). */
    struct Settings
    {
        std::array<int, kNumFunctions> routes {}; // Route per function
        /** Device names per port (empty: none, virtualDevice: the app's virtual port). */
        std::array<juce::String, kNumPorts> inputs, outputs;
        DawRemote::Settings daw;

        Settings();
        static Settings load (juce::PropertiesFile&);
        void save (juce::PropertiesFile&) const;
        bool operator== (const Settings&) const;
        bool operator!= (const Settings& o) const { return ! operator== (o); }
    };

    /** Opens and closes devices to match `settings`. */
    void apply (const Settings&);
    const Settings& getSettings() const noexcept { return settings; }

    //==============================================================================
    // Devices

    /** The devices of this computer (names), without the app's own virtual ports. */
    static juce::StringArray getInputDevices();
    static juce::StringArray getOutputDevices();
    /** Whether this platform lets the app create virtual MIDI ports. */
    static bool canCreateVirtualPorts();
    /** The name of the app's virtual port for `port` ("Sidecar MIDI DAW 1"). */
    static juce::String virtualPortName (int port);

    struct PortStatus
    {
        bool inputOpen = false, outputOpen = false;
        bool inputMissing = false, outputMissing = false; // configured but not there
        juce::int64 received = 0, sent = 0;
    };
    PortStatus getPortStatus (int port) const;

    //==============================================================================
    // Messages (message thread)

    /** Handles a message that arrived on `port` (the device callbacks queue
        theirs here; tests call it directly). */
    void handleMessage (int port, const juce::MidiMessage&);
    /** Sends the console changes since the last call (CH CONTROL and FX
        PARAMETER CONTROL feedback, SCENE MIDI TX). Called by a timer. */
    void sendFeedback();
    /** Forgets what was sent, so the next sendFeedback() sends every value. */
    void resendAll();
    /** Every outgoing message, before it goes to the port's device (tests, monitors). */
    std::function<void (int port, const juce::MidiMessage&)> onSend;

    /** EXTERNAL MIDI CONTROL: the custom controls' MIDI functions. Returns
        false when the function is OFF. */
    bool sendExternal (const juce::MidiMessage&);

    /** SEND MIDI (LIBRARY): the MIDI data of a scene as the console's EDIT
        MIDI TEXT STRING takes it - hexadecimal bytes with optional separators
        ("C002", "B0,01,7F", several messages in a row), "U:" or "D:" before
        data sending it (and what follows) to the USB or DIN port instead of
        EXTERNAL MIDI CONTROL's. Each message goes with its port (-1: the
        default). Returns false (and `error`) for anything else. */
    struct PortMessage
    {
        int port = -1; // Din, Usb or -1
        juce::MidiMessage message;
    };
    static bool parseMidiText (const juce::String& text, std::vector<PortMessage>& out, juce::String& error);
    /** Sends SEND MIDI data (when EXTERNAL MIDI CONTROL is DIN or USB);
        false if nothing was sent. */
    bool sendMidiText (const juce::String& text);
    /** The value of an external CC (MIDI CC encoders), as last sent or received. */
    int getExternalCc (int channel, int cc) const noexcept;
    void setExternalCc (int channel, int cc, int value);
    /** The state of an external toggle (MIDI CC / NOTE toggles), as last sent or received. */
    bool getExternalToggle (int channel, int number, bool isNote) const noexcept;
    void setExternalToggle (int channel, int number, bool isNote, bool on) noexcept;

    /** CUSTOM CONTROLS (RX ONLY): encoder `control` of layer `layer` (0-15)
        set to `value` (0-127), or one of its keys (0-3 upper, 4-7 lower)
        pressed or released. Set by the UI, which owns the assignments. */
    std::function<void (int layer, int control, int value)> onCustomEncoder;
    std::function<void (int layer, int key, bool down)> onCustomKey;
    /** A scene recall made by MIDI ("scene 3"), for the log. */
    std::function<void (const juce::String& what, const state::ApplyResult&)> onRecalled;

    DawRemote& getDaw() noexcept { return daw; }

    //==============================================================================
    // The WING MIDI remote mapping (static so the tests and the ⓘ texts can use it)

    /** The strip of a CH CONTROL CC (12..111), or nullptr. */
    Strip* stripForCc (int cc) const;
    /** The CH CONTROL CC of a strip (-1 if it has none). */
    int ccForStrip (const Strip&) const;
    /** FX PARAMETER CONTROL: the FX slot and parameter of channel `ch` (9-16)
        CC `cc`: param -3 insert ON, -2 FX model, -1 FX mix, 0.. the parameters. */
    static bool fxControl (int channel, int cc, int& slot, int& param) noexcept;
    static int fxCc (int slot, int param) noexcept;
    /** CUSTOM CONTROLS: the layer and encoder of a channel 6 CC. */
    static bool customEncoder (int cc, int& layer, int& control) noexcept;
    static int customEncoderCc (int layer, int control) noexcept;

    /** The WING fader law: 0..1 position for a level in dB (-oo = -144), and back. */
    static float faderPosition (float db) noexcept;
    static float faderDb (float position) noexcept;

    /** The SYSEX CONTROL identity ("WING,<ip>,<name>,ngc-full,..."). */
    juce::String getIdentity() const;
    /** The console name the identity reports (SETUP > GENERAL). */
    juce::String consoleName { "SIDECAR" };
    /** SYSEX CONTROL execute / dump / describe through the WING parameter
        tree: set by the app (they need the WING remote's tree). Return an
        error text ("NODE NOT FOUND") or the data. */
    std::function<juce::String (const juce::String& command)> sysexExecute;
    std::function<std::optional<juce::String> (const juce::String& node)> sysexDump, sysexDescribe;

private:
    struct Device;

    /** Called by the device callbacks (MIDI threads): queues the message. */
    void enqueue (int port, const juce::MidiMessage&);
    void handleAsyncUpdate() override;
    void timerCallback() override;

    void openDevices();
    void closeDevices();
    void send (int port, const juce::MidiMessage&);
    int routeOf (int port) const noexcept;
    bool routed (int function, int port) const noexcept;
    /** The port a function sends on (-1 when it's OFF). */
    int portOf (int function) const noexcept;
    int dawPort (int unit) const noexcept;
    int dawUnit (int port) const noexcept;

    void handleChannelControl (int port, const juce::MidiMessage&);
    void handleFx (int port, const juce::MidiMessage&);
    void handleCustom (const juce::MidiMessage&);
    void handleScenes (int port, const juce::MidiMessage&);
    void handleSysex (int port, const juce::MidiMessage&);
    void handleExternal (const juce::MidiMessage&);
    void recall (int scene);
    void sendAllChannelControl (int midiChannel);
    void sendAllFx (int slot);

    /** CH CONTROL values (fader 0-127, mute 0/127, pan 0-127) of a CC slot. */
    int currentValue (int midiChannel, int cc) const;
    void applyValue (int midiChannel, int cc, int value);
    int currentFxValue (int slot, int param) const;
    void refreshCcStrips() const;

    MixerEngine& engine;
    Show& show;
    const bool useDevices;
    Settings settings;
    DawRemote daw;

    std::array<std::unique_ptr<Device>, kNumPorts> devices;
    juce::MidiDeviceListConnection deviceListConnection;
    bool listening = false;
    std::array<PortStatus, kNumPorts> status {};

    // Device callbacks -> the message thread.
    juce::CriticalSection queueLock;
    std::vector<std::pair<int, juce::MidiMessage>> queue, draining;

    // What was sent / received last (-1: unknown), to send changes only.
    std::array<std::array<int, 128>, 5> lastChannelValues {};
    std::array<std::array<int, 128>, 16> lastFxValues {};
    bool primeChannels = true, primeFx = true;
    int lastSceneIndex = -2;
    int ticks = 0;

    // The strips of the CH CONTROL CCs, rebuilt when the console's structure changes.
    mutable std::array<Strip*, 128> ccStrips {};
    mutable int ccStripsVersion = -1;

    // SCENE CHANGE / SHOW CONTROL: the bank of each channel per port, the GO cursor.
    std::array<std::array<int, 2>, kNumPorts> banks {};
    int sceneCursor = -1;

    std::array<std::array<juce::uint8, 128>, 16> externalCc {};
    std::array<std::array<bool, 256>, 16> externalToggles {};

    JUCE_DECLARE_NON_COPYABLE (MidiRemote)
};

} // namespace livedaw
