#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_data_structures/juce_data_structures.h>

#include <array>
#include <functional>
#include <vector>

namespace livedaw
{

/** DAW remote control (WING SETUP > DAW, the surface's DAW key, WING DAW
    Documentation): the console becomes a Mackie Control Universal with up to
    two extenders, or up to three Mackie HUI units, for a DAW. Each emulated
    unit has eight channel strips (fader, V-pot, REC / SOLO / MUTE / SELECT and
    the DAW's scribble text) on its own MIDI port (USB: MIDI DAW 1-3; DIN: one
    unit). The master unit also has the master fader, the transport and the
    other MCU buttons, the jog wheel, the timecode and the two-digit assignment
    display.

    This class only speaks the protocols: the surface calls the "surface ->
    DAW" methods, `output` takes the messages for each unit, and the DAW's
    messages go to handleMessage(); the views poll the state (getRevision()
    changes whenever the DAW changed something). Message thread only. */
class DawRemote
{
public:
    enum class Emulation
    {
        Mcu,
        Hui
    };

    /** CONFIGURATION: which parts of the surface the DAW gets (the WING's
        choices; the custom controls always). */
    enum Configuration
    {
        CustomControlsOnly,
        SingleMcu,
        McuExtender,
        McuTwoExtenders,
        kNumConfigurations
    };
    static juce::String configurationName (int configuration);

    /** SETUP > DAW (kept in the app settings, `daw.*`). */
    struct Settings
    {
        bool enabled = false;        // DAW ENABLE: the DAW key works, the DAW ports are open
        bool usb = true;             // CONNECTION: USB (MIDI DAW 1-3) or DIN (one unit)
        Emulation emulation = Emulation::Mcu;
        bool upperCc = false;        // USE UPPER CC FOR DAW: the upper CC section is four V-pots
        bool disableWheel = true;    // DISABLE WHEEL DURING PLAY
        int configuration = McuTwoExtenders;
        int preset = 0;              // LAST LOADED PRESET (presetNames(); 0 = "-")

        static Settings load (juce::PropertiesFile&);
        void save (juce::PropertiesFile&) const;
        bool operator== (const Settings&) const;
        bool operator!= (const Settings& o) const { return ! operator== (o); }
    };

    static constexpr int kMaxUnits = 3, kStrips = 8, kMaxChannels = kMaxUnits * kStrips;
    /** The master fader as a channel number. */
    static constexpr int kMaster = kMaxChannels;

    enum class StripButton
    {
        Rec,
        Solo,
        Mute,
        Select,
        Auto,   // HUI only
        VSel,   // HUI only
        Insert  // HUI only
    };

    /** An LED: off, blinking or on. */
    enum Led
    {
        Off = 0,
        Blink = 1,
        On = 2
    };

    DawRemote();

    void setSettings (const Settings&);
    const Settings& getSettings() const noexcept { return settings; }
    bool isHui() const noexcept { return settings.emulation == Emulation::Hui; }

    /** DAW mode (the DAW key, /$ctl/daw/$on): the surface controls the DAW.
        Only while DAW ENABLE is on. */
    bool isOn() const noexcept { return on; }
    void setOn (bool);
    std::function<void()> onModeChanged;

    /** Emulated units (0-3) and their channels. DIN carries one unit. */
    int getNumUnits() const noexcept;
    int getNumChannels() const noexcept { return getNumUnits() * kStrips; }

    /** The messages for unit `unit` (0 = the master unit). */
    std::function<void (int unit, const juce::MidiMessage&)> output;

    //==============================================================================
    // The surface -> the DAW. Channels count across the units: 0-7 the master
    // unit, 8-15 the first extender, 16-23 the second; kMaster the master fader.

    /** A fader moved (0..1). Touch it first, as a motor fader does. */
    void moveFader (int channel, double position);
    void touchFader (int channel, bool touched);
    /** A V-pot turned by `ticks` (> 0 clockwise). */
    void turnVpot (int channel, int ticks);
    void pressVpot (int channel, bool down);
    void pressStripButton (int channel, StripButton, bool down);
    /** A DAW BUTTONS command (index into commands()) pressed or released. */
    void pressCommand (int command, bool down);
    /** The jog wheel (the WING's wheel in DAW mode). */
    void turnJog (int ticks);
    /** A DAW V-POTS function (vpotFunctionName()) turned / pushed. */
    void turnVpotFunction (int function, int ticks);
    void pressVpotFunction (int function, bool down);

    //==============================================================================
    // The DAW -> the surface.

    /** A message from the DAW on unit `unit`'s port. */
    void handleMessage (int unit, const juce::MidiMessage&);

    /** What the DAW shows for one channel strip. */
    struct Strip
    {
        int fader = 0;                  // 0..16383 (the DAW's fader position)
        int ring = 0;                   // V-pot ring, MCU encoding (bits 4-5 mode, 0-3 value, 6 centre LED)
        int rec = Off, solo = Off, mute = Off, select = Off, vsel = Off, autoLed = Off, insert = Off;
        int meter = 0;                  // 0..13 (MCU 0x0..0xD; HUI 0..12)
        bool overload = false;
        bool touched = false;           // the surface's fader is touched
        juce::String upper, lower;      // the DAW's scribble text (MCU: 7 + 7 characters; HUI: a 4-character name)
    };

    const Strip& getStrip (int channel) const noexcept;
    int getMasterFader() const noexcept { return masterFader; }
    /** The LED of a DAW BUTTONS command. */
    int getCommandLed (int command) const noexcept;
    /** The timecode / beats display (MCU: 10 digits, HUI: 8), left to right. */
    juce::String getTimecode() const;
    /** The MCU's two-digit assignment display ("PN", "TR"...). */
    juce::String getAssignment() const;
    /** The master unit's main display: MCU 2 x 56 characters, HUI 2 x 40. */
    juce::String getDisplayLine (int line) const;
    /** True once the DAW talked to unit `unit` (HUI: a ping within 3 s). */
    bool isConnected (int unit) const noexcept;
    /** Bumped by every change the DAW made, so views know to repaint. */
    int getRevision() const noexcept { return revision; }
    /** Meter decay and the HUI ping timeout (about 10 Hz). */
    void tick (juce::uint32 nowMs);
    /** Everything back to the power-on state (LEDs off, faders down, no text). */
    void reset();

    /** The 11 LEDs of a V-pot ring (bit 0 = leftmost; bit 11 = the centre LED
        under the knob) for a ring value. */
    static int ringLeds (int ring) noexcept;

    //==============================================================================
    // Command tables (WING Remote Protocols, "MCU [DAW BUTTONS] commands list").

    struct Command
    {
        const char* code;   // "T1"
        const char* name;   // "STOP" (the CC EDIT list)
        const char* label;  // "STOP" (the CC LCD, at most 7 characters)
        const char* group;  // "TRANSPORT"
        int mcuNote;        // -1: not on the MCU
        int huiZone, huiPort; // -1: not on the HUI
    };
    static const std::vector<Command>& commands();
    static int findCommand (const juce::String& code) noexcept;
    /** The command groups in list order ("TRANSPORT", "NAVIGATION", ...). */
    static juce::StringArray commandGroups();
    /** Whether the command exists in the current emulation (else drawn greyed). */
    bool isAvailable (int command) const noexcept;
    /** A command's label as the DAW of a preset calls it (translation table). */
    static juce::String commandLabel (int command, int preset);

    /** DAW V-POTS: M1-M8 (the master unit's V-pots), E1-E16 (the extenders'),
        JOG. Their pushes (M1P...) are the V-POT PUSH commands. */
    static constexpr int kNumVpotFunctions = 25;
    static juce::String vpotFunctionName (int function);
    /** The channel a V-pot function drives (-1 for JOG). */
    static int vpotFunctionChannel (int function) noexcept;

    //==============================================================================
    // Presets (PRESET / LOAD): the four DAW layers of the lower CC section.

    static juce::StringArray presetNames();
    struct PresetKey
    {
        int layer;    // DAW 1-4 as 0-3
        int key;      // 0-3 the upper row, 4-7 the lower row
        int command;  // index into commands()
    };
    static std::vector<PresetKey> presetKeys (int preset);
    /** The LCD name of a column of a preset's layer ("TRANSP"). */
    static juce::String presetColumnName (int preset, int layer, int column);

    /** The upper CC section's block of four channels (UP / DOWN, SELECT). */
    int getVpotBlock() const noexcept { return vpotBlock; }
    void setVpotBlock (int block);

    /** The special keys (SP1-SP6, latched while lit): FADER TOUCH [MUTE],
        V-POT CTRL [SEL/SOLO], RECRDY CTRL [SEL], AUTO / V-SEL / INSERT [SEL]. */
    enum Special
    {
        FaderTouchMute,
        VpotControl,
        RecReadyControl,
        AutoSelect,
        VSelSelect,
        InsertSelect,
        kNumSpecials
    };
    bool isSpecialOn (int special) const noexcept { return juce::isPositiveAndBelow (special, (int) kNumSpecials) && specials[(size_t) special]; }
    /** A DAW strip's SELECT / SOLO / MUTE key, through the special modes
        (e.g. SELECT sends REC while RECRDY CTRL [SEL] is held, SOLO pushes
        the V-pot with V-POT CTRL; SELECT held + the wheel turns it). */
    void pressChannelKey (int channel, StripButton key, bool down);

    /** The MCU serial number the master unit reports (7 characters). */
    static juce::String serialNumber (int unit);

private:
    void send (int unit, const juce::MidiMessage&);
    void noteBang (int unit, int note, bool down);
    void huiSwitch (int unit, int zone, int port, bool down);
    void handleMcu (int unit, const juce::MidiMessage&);
    void handleHui (int unit, const juce::MidiMessage&);
    void handleMcuSysex (int unit, const juce::uint8* data, int size);
    void handleHuiSysex (int unit, const juce::uint8* data, int size);
    void setText (int unit, int offset, const juce::uint8* chars, int count);
    void changed() noexcept { ++revision; }
    int unitOf (int channel) const noexcept { return channel / kStrips; }

    Settings settings;
    bool on = false;
    int revision = 0;
    int vpotBlock = 0;
    std::array<bool, kNumSpecials> specials {};
    int heldSelect = -1; // V-POT CTRL: the channel whose SELECT is held
    std::array<bool, kMaxChannels + 1> latchedTouch {}; // FADER TOUCH [MUTE]

    struct Unit
    {
        std::array<Strip, kStrips> strips {};
        std::array<juce::uint8, 128> noteLeds {};      // MCU: the LED of each note (Led)
        std::array<std::array<juce::uint8, 8>, 0x1e> huiLeds {}; // HUI: zone x port
        std::array<char, 112> lcd {};                 // MCU 2 x 56 / HUI main display 2 x 40 (+ padding)
        int huiZone = 0;                              // the zone the DAW selected for LEDs
        std::array<int, kStrips + 1> huiFaderHigh {}; // a HUI fader's high byte until its low byte
        juce::uint32 lastMessageMs = 0;
        bool talked = false;
        std::array<juce::uint32, kStrips> meterTime {};
    };
    std::array<Unit, kMaxUnits> units {};
    Strip noStrip;
    int masterFader = 0;
    std::array<char, 12> timecode {};  // MCU CC 0x40-0x49 (right to left), HUI sysex 0x11
    std::array<char, 2> assignment {}; // MCU CC 0x4A / 0x4B
    std::array<bool, 12> timecodeDots {};
    juce::uint32 nowMs = 0;
};

} // namespace livedaw
