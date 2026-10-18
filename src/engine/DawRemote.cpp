#include "engine/DawRemote.h"

namespace livedaw
{

namespace
{
    // Mackie's sysex manufacturer id and device ids.
    constexpr juce::uint8 kMcuId = 0x14, kMcuExtenderId = 0x15, kLogicControlId = 0x10, kLogicExtenderId = 0x11, kHuiId = 0x05;

    // MCU notes of the channel strips (+ strip) and of the faders' touch.
    constexpr int kRecNote = 0x00, kSoloNote = 0x08, kMuteNote = 0x10, kSelectNote = 0x18, kVpotPushNote = 0x20, kTouchNote = 0x68;
    // MCU controllers.
    constexpr int kVpotCc = 0x10, kJogCc = 0x3c, kRingCc = 0x30, kTimecodeCc = 0x40, kAssignmentCc = 0x4a;
    // HUI channel strip ports (zone = strip).
    constexpr int kHuiTouch = 0, kHuiSelect = 1, kHuiMute = 2, kHuiSolo = 3, kHuiAuto = 4, kHuiVSel = 5, kHuiInsert = 6, kHuiRec = 7;

    juce::MidiMessage noteMessage (int note, bool down)
    {
        return juce::MidiMessage::noteOn (1, note, (juce::uint8) (down ? 0x7f : 0x00));
    }

    juce::MidiMessage ccMessage (int controller, int value)
    {
        return juce::MidiMessage::controllerEvent (1, controller, juce::jlimit (0, 127, value));
    }

    /** MCU relative value: the clicks, bit 6 set when turning counter-clockwise. */
    int mcuRelative (int ticks)
    {
        const int n = juce::jlimit (1, 63, std::abs (ticks));
        return ticks < 0 ? 0x40 | n : n;
    }

    /** HUI delta: 0x40 + clicks clockwise, the clicks counter-clockwise. */
    int huiRelative (int ticks)
    {
        const int n = juce::jlimit (1, 63, std::abs (ticks));
        return ticks < 0 ? n : 0x40 + n;
    }

    int ledOf (int velocity)
    {
        if (velocity == 0x7f)
            return DawRemote::On;
        return (velocity & 1) != 0 ? DawRemote::Blink : DawRemote::Off;
    }

    // clang-format off
    const std::vector<DawRemote::Command>& commandTable()
    {
        // The WING's DAW BUTTONS list (code, name) with the Mackie Control
        // note and the HUI zone / port of the same function; the HUI layout
        // follows the public reverse-engineered HUI protocol.
        static const std::vector<DawRemote::Command> table {
            { "T1", "STOP", "STOP", "TRANSPORT", 0x5d, 0x0e, 3 },
            { "T2", "PLAY", "PLAY", "TRANSPORT", 0x5e, 0x0e, 4 },
            { "T3", "RECORD", "REC", "TRANSPORT", 0x5f, 0x0e, 5 },
            { "T4", "REWIND", "REW", "TRANSPORT", 0x5b, 0x0e, 1 },
            { "T5", "FAST FWD", "FFWD", "TRANSPORT", 0x5c, 0x0e, 2 },
            { "T6", "MARKER", "MARKER", "TRANSPORT", 0x54, -1, -1 },
            { "T7", "NUDGE", "NUDGE", "TRANSPORT", 0x55, -1, -1 },
            { "T8", "CYCLE", "CYCLE", "TRANSPORT", 0x56, 0x0f, 3 },
            { "T9", "DROP", "DROP", "TRANSPORT", 0x57, -1, -1 },
            { "T10", "REPLACE", "REPLACE", "TRANSPORT", 0x58, -1, -1 },
            { "T11", "SCRUB", "SCRUB", "TRANSPORT", 0x65, 0x0d, 5 },
            { "T12", "SHUTTLE", "SHUTTLE", "TRANSPORT", -1, 0x0d, 6 },
            { "T13", "RETURN TO ZERO", "RTZ", "TRANSPORT", -1, 0x0f, 0 },
            { "T14", "GO TO END", "END", "TRANSPORT", -1, 0x0f, 1 },
            { "T15", "IN", "IN", "TRANSPORT", -1, 0x10, 2 },
            { "T16", "OUT", "OUT", "TRANSPORT", -1, 0x10, 3 },
            { "T17", "PRE", "PRE", "TRANSPORT", -1, 0x10, 1 },
            { "T18", "POST", "POST", "TRANSPORT", -1, 0x10, 4 },
            { "T19", "ONLINE", "ONLINE", "TRANSPORT", -1, 0x0f, 2 },
            { "T20", "QUICK PUNCH", "QPUNCH", "TRANSPORT", -1, 0x0f, 4 },
            { "N1", "UP (NAV)", "UP", "NAVIGATION", 0x60, 0x0d, 4 },
            { "N2", "DOWN (NAV)", "DOWN", "NAVIGATION", 0x61, 0x0d, 0 },
            { "N3", "LEFT (NAV)", "LEFT", "NAVIGATION", 0x62, 0x0d, 1 },
            { "N4", "RIGHT (NAV)", "RIGHT", "NAVIGATION", 0x63, 0x0d, 3 },
            { "N5", "ZOOM", "ZOOM", "NAVIGATION", 0x64, 0x0d, 2 },
            { "N6", "BK <", "BANK<", "NAVIGATION", 0x2e, 0x0a, 1 },
            { "N7", "BK >", "BANK>", "NAVIGATION", 0x2f, 0x0a, 3 },
            { "N8", "CH <", "CH<", "NAVIGATION", 0x30, 0x0a, 0 },
            { "N9", "CH >", "CH>", "NAVIGATION", 0x31, 0x0a, 2 },
            { "A1", "TRACK (ASSIGN)", "TRACK", "ASSIGN", 0x28, -1, -1 },
            { "A2", "SEND (ASSIGN)", "SEND", "ASSIGN", 0x29, -1, -1 },
            { "A3", "PAN (ASSIGN)", "PAN", "ASSIGN", 0x2a, 0x0b, 2 },
            { "A4", "PLUG-IN (ASSIGN)", "PLUG-IN", "ASSIGN", 0x2b, -1, -1 },
            { "A5", "EQ (ASSIGN)", "EQ", "ASSIGN", 0x2c, -1, -1 },
            { "A6", "INST (ASSIGN)", "INST", "ASSIGN", 0x2d, -1, -1 },
            { "A7", "SEND A (ASSIGN)", "SEND A", "ASSIGN", -1, 0x0b, 7 },
            { "A8", "SEND B (ASSIGN)", "SEND B", "ASSIGN", -1, 0x0b, 6 },
            { "A9", "SEND C (ASSIGN)", "SEND C", "ASSIGN", -1, 0x0b, 5 },
            { "A10", "SEND D (ASSIGN)", "SEND D", "ASSIGN", -1, 0x0b, 4 },
            { "A11", "SEND E (ASSIGN)", "SEND E", "ASSIGN", -1, 0x0b, 3 },
            { "A12", "INPUT (ASSIGN)", "INPUT", "ASSIGN", -1, 0x0b, 1 },
            { "A13", "OUTPUT (ASSIGN)", "OUTPUT", "ASSIGN", -1, 0x0b, 0 },
            { "A14", "ASSIGN (ASSIGN)", "ASSIGN", "ASSIGN", -1, 0x0c, 0 },
            { "A15", "SHIFT (ASSIGN)", "SHIFT", "ASSIGN", -1, 0x0c, 3 },
            { "A16", "MUTE (ASSIGN)", "MUTE", "ASSIGN", -1, 0x0c, 4 },
            { "F1", "F1", "F1", "FUNCTION", 0x36, 0x1b, 0 },
            { "F2", "F2", "F2", "FUNCTION", 0x37, 0x1b, 1 },
            { "F3", "F3", "F3", "FUNCTION", 0x38, 0x1b, 2 },
            { "F4", "F4", "F4", "FUNCTION", 0x39, 0x1b, 3 },
            { "F5", "F5", "F5", "FUNCTION", 0x3a, 0x1b, 4 },
            { "F6", "F6", "F6", "FUNCTION", 0x3b, 0x1b, 5 },
            { "F7", "F7", "F7", "FUNCTION", 0x3c, 0x1b, 6 },
            { "F8", "F8", "F8", "FUNCTION", 0x3d, 0x1b, 7 },
            { "V1", "GLOBAL (VIEW)", "GLOBAL", "VIEW", 0x33, -1, -1 },
            { "V2", "MIDI (VIEW)", "MIDI", "VIEW", 0x3e, -1, -1 },
            { "V3", "INPUTS (VIEW)", "INPUTS", "VIEW", 0x3f, -1, -1 },
            { "V4", "AUDIO TRACKS (VIEW)", "AUDIO", "VIEW", 0x40, -1, -1 },
            { "V5", "INSTRUMENT (VIEW)", "INSTR", "VIEW", 0x41, -1, -1 },
            { "V6", "AUX (VIEW)", "AUX", "VIEW", 0x42, -1, -1 },
            { "V7", "BUSES (VIEW)", "BUSES", "VIEW", 0x43, -1, -1 },
            { "V8", "OUTPUTS (VIEW)", "OUTPUTS", "VIEW", 0x44, -1, -1 },
            { "V9", "USER (VIEW)", "USER", "VIEW", 0x45, -1, -1 },
            { "V10", "MIX (VIEW)", "MIX", "VIEW", -1, 0x09, 0 },
            { "V11", "EDIT (VIEW)", "EDIT", "VIEW", -1, 0x09, 1 },
            { "V12", "TRANSPORT (VIEW)", "TRANSP", "VIEW", -1, 0x09, 2 },
            { "V13", "MEM/LOC (VIEW)", "MEM-LOC", "VIEW", -1, 0x09, 3 },
            { "V14", "STATUS (VIEW)", "STATUS", "VIEW", -1, 0x09, 4 },
            { "V15", "ALT (VIEW)", "ALT", "VIEW", -1, 0x09, 5 },
            { "AU1", "READ/OFF (AUTOM)", "READ", "AUTOMATION", 0x4a, 0x18, 2 },
            { "AU2", "WRITE (AUTOM)", "WRITE", "AUTOMATION", 0x4b, 0x18, 4 },
            { "AU3", "TRIM (AUTOM)", "TRIM", "AUTOMATION", 0x4c, 0x18, 0 },
            { "AU4", "TOUCH (AUTOM)", "TOUCH", "AUTOMATION", 0x4d, 0x18, 5 },
            { "AU5", "LATCH (AUTOM)", "LATCH", "AUTOMATION", 0x4e, 0x18, 1 },
            { "AU6", "OFF (AUTOM)", "OFF", "AUTOMATION", -1, 0x18, 3 },
            { "AU7", "FADER (AUTOM)", "FADER", "AUTOMATION", -1, 0x17, 2 },
            { "AU8", "PAN (AUTOM)", "PAN", "AUTOMATION", -1, 0x17, 1 },
            { "AU9", "MUTE (AUTOM)", "MUTE", "AUTOMATION", -1, 0x17, 5 },
            { "AU10", "SEND (AUTOM)", "SEND", "AUTOMATION", -1, 0x17, 4 },
            { "AU11", "SEND MUTE (AUTOM)", "SNDMUTE", "AUTOMATION", -1, 0x17, 3 },
            { "AU12", "PLUG-IN (AUTOM)", "PLUG-IN", "AUTOMATION", -1, 0x17, 0 },
            { "SY1", "SHIFT", "SHIFT", "MODIFIERS", 0x46, 0x08, 1 },
            { "SY2", "OPTION", "OPTION", "MODIFIERS", 0x47, 0x08, 5 },
            { "SY3", "CTRL", "CTRL", "MODIFIERS", 0x48, 0x08, 0 },
            { "SY4", "ALT", "ALT", "MODIFIERS", 0x49, 0x08, 4 },
            { "SY5", "SAVE", "SAVE", "MODIFIERS", 0x50, 0x08, 7 },
            { "SY6", "UNDO", "UNDO", "MODIFIERS", 0x51, 0x08, 3 },
            { "SY7", "CANCEL", "CANCEL", "MODIFIERS", 0x52, -1, -1 },
            { "SY8", "ENTER", "ENTER", "MODIFIERS", 0x53, 0x14, 0 },
            { "SY9", "EDIT MODE", "EDITMOD", "MODIFIERS", -1, 0x08, 2 },
            { "SY10", "EDIT TOOL", "EDITTOOL", "MODIFIERS", -1, 0x08, 6 },
            { "OT1", "FLIP", "FLIP", "OTHER", 0x32, -1, -1 },
            { "OT2", "GROUP", "GROUP", "OTHER", 0x4f, 0x19, 5 },
            { "OT3", "NAME/VALUE", "NAM/VAL", "OTHER", 0x34, -1, -1 },
            { "OT4", "TIME/BEATS", "SMPTE", "OTHER", 0x35, -1, -1 },
            { "OT5", "CLICK", "CLICK", "OTHER", 0x59, -1, -1 },
            { "OT6", "SOLO", "SOLO", "OTHER", 0x5a, -1, -1 },
            { "OT7", "FOOTSW A", "FOOTSWA", "OTHER", 0x66, -1, -1 },
            { "OT8", "FOOTSW B", "FOOTSWB", "OTHER", 0x67, -1, -1 },
            { "OT9", "DEFAULT", "DEFAULT", "OTHER", -1, 0x0c, 1 },
            { "OT10", "SUSPEND", "SUSPEND", "OTHER", -1, 0x0c, 2 },
            { "OT11", "BYPASS", "BYPASS", "OTHER", -1, 0x0c, 5 },
            { "OT12", "RECRDY ALL", "RECRDY", "OTHER", -1, 0x0c, 6 },
            { "E1", "CUT (EDIT)", "CUT", "EDIT", -1, 0x1a, 1 },
            { "E2", "COPY (EDIT)", "COPY", "EDIT", -1, 0x1a, 4 },
            { "E3", "PASTE (EDIT)", "PASTE", "EDIT", -1, 0x1a, 0 },
            { "E4", "SEPARATE (EDIT)", "SEPARAT", "EDIT", -1, 0x1a, 5 },
            { "E5", "CAPTURE (EDIT)", "CAPTURE", "EDIT", -1, 0x1a, 2 },
            { "E6", "DELETE (EDIT)", "DELETE", "EDIT", -1, 0x1a, 3 },
            { "E7", "ASSIGN (EDIT)", "ASSIGN", "EDIT", -1, 0x1c, 1 },
            { "E8", "COMPARE (EDIT)", "COMPARE", "EDIT", -1, 0x1c, 7 },
            { "E9", "BYPASS (EDIT)", "BYPASS", "EDIT", -1, 0x1c, 6 },
            { "E10", "INS/PARAM (EDIT)", "INS/PAR", "EDIT", -1, 0x1c, 0 },
            // The special keys work on the DAW strips' keys (kSpecial).
            { "SP1", "FADER TOUCH [MUTE]", "TOUCH", "SPECIAL", -2, -2, -1 },
            { "SP2", "V-POT CTRL [SEL/SOLO]", "VPOTCTL", "SPECIAL", -2, -2, -1 },
            { "SP3", "RECRDY CTRL [SEL]", "RECRDY", "SPECIAL", -2, -2, -1 },
            { "SP4", "AUTO [SEL]", "AUTO", "SPECIAL", -1, -2, -1 },
            { "SP5", "V-SEL [SEL]", "V-SEL", "SPECIAL", -1, -2, -1 },
            { "SP6", "INSERT [SEL]", "INSERT", "SPECIAL", -1, -2, -1 },
            // DAW V-POTS pushes (kVpotPush): M1P-M8P the master unit, E1P-E16P the extenders.
            { "M1P", "V-POT M1 PUSH", "PUSH M1", "V-POT PUSH", -3, -3, 0 },
            { "M2P", "V-POT M2 PUSH", "PUSH M2", "V-POT PUSH", -3, -3, 1 },
            { "M3P", "V-POT M3 PUSH", "PUSH M3", "V-POT PUSH", -3, -3, 2 },
            { "M4P", "V-POT M4 PUSH", "PUSH M4", "V-POT PUSH", -3, -3, 3 },
            { "M5P", "V-POT M5 PUSH", "PUSH M5", "V-POT PUSH", -3, -3, 4 },
            { "M6P", "V-POT M6 PUSH", "PUSH M6", "V-POT PUSH", -3, -3, 5 },
            { "M7P", "V-POT M7 PUSH", "PUSH M7", "V-POT PUSH", -3, -3, 6 },
            { "M8P", "V-POT M8 PUSH", "PUSH M8", "V-POT PUSH", -3, -3, 7 },
            { "E1P", "V-POT EXT1 PUSH", "PUSH E1", "V-POT PUSH", -3, -3, 8 },
            { "E2P", "V-POT EXT2 PUSH", "PUSH E2", "V-POT PUSH", -3, -3, 9 },
            { "E3P", "V-POT EXT3 PUSH", "PUSH E3", "V-POT PUSH", -3, -3, 10 },
            { "E4P", "V-POT EXT4 PUSH", "PUSH E4", "V-POT PUSH", -3, -3, 11 },
            { "E5P", "V-POT EXT5 PUSH", "PUSH E5", "V-POT PUSH", -3, -3, 12 },
            { "E6P", "V-POT EXT6 PUSH", "PUSH E6", "V-POT PUSH", -3, -3, 13 },
            { "E7P", "V-POT EXT7 PUSH", "PUSH E7", "V-POT PUSH", -3, -3, 14 },
            { "E8P", "V-POT EXT8 PUSH", "PUSH E8", "V-POT PUSH", -3, -3, 15 },
            { "E9P", "V-POT EXT9 PUSH", "PUSH E9", "V-POT PUSH", -3, -3, 16 },
            { "E10P", "V-POT EXT10 PUSH", "PUSH E10", "V-POT PUSH", -3, -3, 17 },
            { "E11P", "V-POT EXT11 PUSH", "PUSH E11", "V-POT PUSH", -3, -3, 18 },
            { "E12P", "V-POT EXT12 PUSH", "PUSH E12", "V-POT PUSH", -3, -3, 19 },
            { "E13P", "V-POT EXT13 PUSH", "PUSH E13", "V-POT PUSH", -3, -3, 20 },
            { "E14P", "V-POT EXT14 PUSH", "PUSH E14", "V-POT PUSH", -3, -3, 21 },
            { "E15P", "V-POT EXT15 PUSH", "PUSH E15", "V-POT PUSH", -3, -3, 22 },
            { "E16P", "V-POT EXT16 PUSH", "PUSH E16", "V-POT PUSH", -3, -3, 23 },
        };
        return table;
    }

    /** The translation table (WING DAW Translation-Table 20-06-19): what an MCU
        command does in a DAW, for the LCD. Columns: CUBASE/NUENDO, LIVE,
        LOGIC, STUDIO ONE, REAPER; empty = the MCU name. */
    struct Translation
    {
        const char* code;
        const char* labels[5];
    };

    const Translation translations[] = {
        { "T6", { "LEFT", "LEFT", "", "", "" } },
        { "T7", { "RIGHT", "RIGHT", "", "", "" } },
        { "T8", { "", "LOOP", "", "", "LOOP" } },
        { "T9", { "PUNCH", "PUNCH IN", "", "", "RECMODE" } },
        { "T10", { "PRV MRK", "PUNCHOUT", "", "", "" } },
        { "OT5", { "ADD MRK", "HOME", "", "", "" } },
        { "OT6", { "NXT MRK", "END", "", "", "SOLODEF" } },
        { "SY1", { "UNDO", "UNDO", "", "", "" } },
        { "SY2", { "REDO", "REDO", "", "", "" } },
        { "SY3", { "SAVE", "SAVE", "", "", "" } },
        { "SY4", { "REVERT", "REVERT", "", "", "" } },
        { "AU1", { "", "SESS/ARR", "", "", "" } },
        { "AU2", { "", "CLIP/FX", "", "", "" } },
        { "AU3", { "SENDS", "UNDO", "", "", "" } },
        { "AU4", { "PROJECT", "BROWSER", "", "", "" } },
        { "AU5", { "MIXER", "DETAIL", "", "", "" } },
        { "OT2", { "MOTORS", "REDO", "", "", "" } },
        { "SY5", { "INSTR", "BTA", "", "", "" } },
        { "SY6", { "MASTER", "DRAW", "", "", "" } },
        { "SY7", { "SOLODEF", "MARKER", "", "", "" } },
        { "SY8", { "SHIFT", "FOLLOW", "", "", "" } },
        { "V2", { "FB 1", "", "", "", "" } },
        { "V3", { "FB 2", "", "", "", "" } },
        { "V4", { "FB 3", "", "", "", "" } },
        { "V5", { "FB 4", "", "", "", "" } },
        { "V6", { "FB 5", "", "", "", "" } },
        { "V7", { "FB 6", "", "", "", "" } },
        { "V8", { "FB 7", "", "", "", "" } },
        { "V9", { "FB 8", "", "", "", "" } },
        { "F1", { "", "", "", "INPUTS", "LOOPMRK" } },
        { "F2", { "", "", "", "TRACK", "CURSMRK" } },
        { "F3", { "", "", "", "CONSOLE", "SELREG" } },
        { "F4", { "", "", "", "OPEN CH", "STOREG" } },
        { "F5", { "", "", "", "ADD INS", "STOMRK" } },
        { "F6", { "", "", "", "ADD SND", "LOOPREG" } },
        { "F7", { "", "", "", "CH EDIT", "LOOPREG" } },
        { "F8", { "", "", "", "FLOAT", "" } },
        { "A1", { "", "I/O", "", "", "" } },
        { "A2", { "", "SENDS", "", "", "" } },
        { "A4", { "PLUG-IN", "PLUG-IN", "", "LINK", "" } },
        { "A5", { "", "PAGE <", "", "BYP INS", "" } },
        { "A6", { "DYN/FX", "PAGE >", "", "", "" } },
        { "V1", { "EDIT", "RETURN", "", "", "" } },
        { "OT3", { "", "METERS", "", "", "" } },
        { "OT4", { "", "BEATS", "", "", "" } },
    };

    /** Preset column of the translation table (-1: the MCU / HUI names). */
    int translationColumn (int preset)
    {
        switch (preset)
        {
            case 1: return 0; // CUBASE
            case 2: return 1; // LIVE
            case 3: return 2; // LOGIC X
            case 4: return 0; // NUENDO
            case 6: return 4; // REAPER
            case 7: return 3; // STUDIO ONE
            default: return -1;
        }
    }
    // clang-format on
} // namespace

//==============================================================================
juce::String DawRemote::configurationName (int configuration)
{
    static const char* names[] = { "CUSTOM CONTROLS ONLY", "SINGLE MCU", "MCU + EXTENDER", "MCU + 2x EXTENDER" };
    return names[juce::jlimit (0, (int) kNumConfigurations - 1, configuration)];
}

DawRemote::Settings DawRemote::Settings::load (juce::PropertiesFile& p)
{
    Settings s;
    s.enabled = p.getBoolValue ("daw.on", false);
    s.usb = p.getIntValue ("daw.conn", 1) != 0;
    s.emulation = p.getIntValue ("daw.emul", 0) != 0 ? Emulation::Hui : Emulation::Mcu;
    s.upperCc = p.getBoolValue ("daw.ccup", false);
    s.disableWheel = p.getBoolValue ("daw.disjog", true);
    s.configuration = juce::jlimit (0, (int) kNumConfigurations - 1, p.getIntValue ("daw.config", McuTwoExtenders));
    s.preset = juce::jlimit (0, presetNames().size() - 1, p.getIntValue ("daw.preset", 0));
    return s;
}

void DawRemote::Settings::save (juce::PropertiesFile& p) const
{
    p.setValue ("daw.on", enabled);
    p.setValue ("daw.conn", usb ? 1 : 0);
    p.setValue ("daw.emul", emulation == Emulation::Hui ? 1 : 0);
    p.setValue ("daw.ccup", upperCc);
    p.setValue ("daw.disjog", disableWheel);
    p.setValue ("daw.config", configuration);
    p.setValue ("daw.preset", preset);
}

bool DawRemote::Settings::operator== (const Settings& o) const
{
    return enabled == o.enabled && usb == o.usb && emulation == o.emulation && upperCc == o.upperCc && disableWheel == o.disableWheel
           && configuration == o.configuration && preset == o.preset;
}

//==============================================================================
DawRemote::DawRemote()
{
    reset();
}

void DawRemote::setSettings (const Settings& s)
{
    const bool protocolChanged = s.emulation != settings.emulation || s.usb != settings.usb || s.enabled != settings.enabled;
    settings = s;
    if (protocolChanged)
        reset();
    if (! settings.enabled)
        setOn (false);
    setVpotBlock (vpotBlock);
    changed();
}

void DawRemote::setOn (bool shouldBeOn)
{
    shouldBeOn = shouldBeOn && settings.enabled;
    if (shouldBeOn == on)
        return;
    on = shouldBeOn;
    // Leaving DAW mode lets go of what the surface was holding.
    for (int c = 0; c <= kMaster; ++c)
        if (latchedTouch[(size_t) c])
            touchFader (c, false);
    latchedTouch = {};
    heldSelect = -1;
    changed();
    if (onModeChanged)
        onModeChanged();
}

int DawRemote::getNumUnits() const noexcept
{
    if (! settings.usb)
        return 1;
    switch (settings.configuration)
    {
        case McuExtender:     return 2;
        case McuTwoExtenders: return 3;
        default:              return 1;
    }
}

void DawRemote::setVpotBlock (int block)
{
    const int blocks = juce::jmax (1, getNumChannels() / 4);
    const int b = juce::jlimit (0, blocks - 1, block);
    if (b != vpotBlock)
    {
        vpotBlock = b;
        changed();
    }
}

juce::String DawRemote::serialNumber (int unit)
{
    return "LDAW00" + juce::String (juce::jlimit (0, 9, unit));
}

//==============================================================================
void DawRemote::send (int unit, const juce::MidiMessage& m)
{
    if (output && juce::isPositiveAndBelow (unit, getNumUnits()))
        output (unit, m);
}

void DawRemote::noteBang (int unit, int note, bool down)
{
    send (unit, noteMessage (note, down));
}

void DawRemote::huiSwitch (int unit, int zone, int port, bool down)
{
    send (unit, ccMessage (0x0f, zone));
    send (unit, ccMessage (0x2f, (down ? 0x40 : 0x00) | (port & 7)));
}

void DawRemote::moveFader (int channel, double position)
{
    const int value = juce::jlimit (0, 16383, juce::roundToInt (juce::jlimit (0.0, 1.0, position) * 16383.0));
    if (channel == kMaster)
    {
        masterFader = value;
        if (! isHui())
            send (0, juce::MidiMessage::pitchWheel (9, value));
        changed();
        return;
    }
    if (! juce::isPositiveAndBelow (channel, getNumChannels()))
        return;
    const int unit = unitOf (channel), strip = channel % kStrips;
    units[(size_t) unit].strips[(size_t) strip].fader = value;
    if (isHui())
    {
        send (unit, ccMessage (strip, value >> 7));
        send (unit, ccMessage (0x20 + strip, value & 0x7f));
    }
    else
        send (unit, juce::MidiMessage::pitchWheel (strip + 1, value));
    changed();
}

void DawRemote::touchFader (int channel, bool touched)
{
    if (channel == kMaster)
    {
        if (! isHui())
            noteBang (0, kTouchNote + 8, touched);
        return;
    }
    if (! juce::isPositiveAndBelow (channel, getNumChannels()))
        return;
    const int unit = unitOf (channel), strip = channel % kStrips;
    auto& s = units[(size_t) unit].strips[(size_t) strip];
    if (s.touched == touched)
        return;
    s.touched = touched;
    if (isHui())
        huiSwitch (unit, strip, kHuiTouch, touched);
    else
        noteBang (unit, kTouchNote + strip, touched);
    changed();
}

void DawRemote::turnVpot (int channel, int ticks)
{
    if (ticks == 0 || ! juce::isPositiveAndBelow (channel, getNumChannels()))
        return;
    const int unit = unitOf (channel), strip = channel % kStrips;
    if (isHui())
        send (unit, ccMessage (0x40 + strip, huiRelative (ticks)));
    else
        send (unit, ccMessage (kVpotCc + strip, mcuRelative (ticks)));
}

void DawRemote::pressVpot (int channel, bool down)
{
    if (! juce::isPositiveAndBelow (channel, getNumChannels()))
        return;
    const int unit = unitOf (channel), strip = channel % kStrips;
    // The HUI has no V-pot push: its V-SEL key does the job.
    if (isHui())
        huiSwitch (unit, strip, kHuiVSel, down);
    else
        noteBang (unit, kVpotPushNote + strip, down);
}

void DawRemote::pressStripButton (int channel, StripButton b, bool down)
{
    if (! juce::isPositiveAndBelow (channel, getNumChannels()))
        return;
    const int unit = unitOf (channel), strip = channel % kStrips;
    if (isHui())
    {
        int port = kHuiSelect;
        switch (b)
        {
            case StripButton::Rec:    port = kHuiRec; break;
            case StripButton::Solo:   port = kHuiSolo; break;
            case StripButton::Mute:   port = kHuiMute; break;
            case StripButton::Select: port = kHuiSelect; break;
            case StripButton::Auto:   port = kHuiAuto; break;
            case StripButton::VSel:   port = kHuiVSel; break;
            case StripButton::Insert: port = kHuiInsert; break;
        }
        huiSwitch (unit, strip, port, down);
        return;
    }
    int base = -1;
    switch (b)
    {
        case StripButton::Rec:    base = kRecNote; break;
        case StripButton::Solo:   base = kSoloNote; break;
        case StripButton::Mute:   base = kMuteNote; break;
        case StripButton::Select: base = kSelectNote; break;
        case StripButton::Auto:
        case StripButton::VSel:
        case StripButton::Insert: break; // HUI only
    }
    if (base >= 0)
        noteBang (unit, base + strip, down);
}

void DawRemote::pressChannelKey (int channel, StripButton key, bool down)
{
    if (! juce::isPositiveAndBelow (channel, getNumChannels()))
        return;
    if (key == StripButton::Mute && specials[FaderTouchMute])
    {
        // FADER TOUCH [MUTE]: MUTE latches the fader's touch (X32 style).
        if (down)
        {
            latchedTouch[(size_t) channel] = ! latchedTouch[(size_t) channel];
            touchFader (channel, latchedTouch[(size_t) channel]);
        }
        return;
    }
    if (key == StripButton::Select)
    {
        if (specials[RecReadyControl])
            return pressStripButton (channel, StripButton::Rec, down);
        if (isHui() && specials[AutoSelect])
            return pressStripButton (channel, StripButton::Auto, down);
        if (isHui() && specials[VSelSelect])
            return pressStripButton (channel, StripButton::VSel, down);
        if (isHui() && specials[InsertSelect])
            return pressStripButton (channel, StripButton::Insert, down);
        if (specials[VpotControl])
        {
            // V-POT CTRL: SELECT held + the wheel turns the V-pot.
            heldSelect = down ? channel : (heldSelect == channel ? -1 : heldSelect);
            return;
        }
    }
    if (key == StripButton::Solo && specials[VpotControl])
        return pressVpot (channel, down);
    pressStripButton (channel, key, down);
}

void DawRemote::pressCommand (int command, bool down)
{
    const auto& table = commands();
    if (! juce::isPositiveAndBelow (command, (int) table.size()))
        return;
    const auto& c = table[(size_t) command];
    if (c.mcuNote == -3)
        return pressVpot (c.huiPort, down);
    if (c.mcuNote == -2 || c.huiZone == -2)
    {
        // The special keys: FADER TOUCH and V-POT CTRL latch, the [SEL] ones
        // work while held.
        const int special = command - findCommand ("SP1");
        if (! juce::isPositiveAndBelow (special, (int) kNumSpecials))
            return;
        if (special == FaderTouchMute || special == VpotControl)
        {
            if (down)
                specials[(size_t) special] = ! specials[(size_t) special];
        }
        else
            specials[(size_t) special] = down;
        changed();
        return;
    }
    if (isHui())
    {
        if (c.huiZone >= 0)
            huiSwitch (0, c.huiZone, c.huiPort, down);
    }
    else if (c.mcuNote >= 0)
        noteBang (0, c.mcuNote, down);
}

void DawRemote::turnJog (int ticks)
{
    if (ticks == 0)
        return;
    if (heldSelect >= 0)
        return turnVpot (heldSelect, ticks);
    if (isHui())
        send (0, ccMessage (0x0d, huiRelative (ticks)));
    else
        send (0, ccMessage (kJogCc, mcuRelative (ticks)));
}

juce::String DawRemote::vpotFunctionName (int function)
{
    if (function < 8)
        return "V-POT M" + juce::String (juce::jmax (0, function) + 1);
    if (function < 24)
        return "V-POT EXT" + juce::String (function - 7);
    return "JOG WHEEL";
}

int DawRemote::vpotFunctionChannel (int function) noexcept
{
    return juce::isPositiveAndBelow (function, kMaxChannels) ? function : -1;
}

void DawRemote::turnVpotFunction (int function, int ticks)
{
    const int channel = vpotFunctionChannel (function);
    if (channel >= 0)
        turnVpot (channel, ticks);
    else if (function == kNumVpotFunctions - 1)
        turnJog (ticks);
}

void DawRemote::pressVpotFunction (int function, bool down)
{
    if (const int channel = vpotFunctionChannel (function); channel >= 0)
        pressVpot (channel, down);
}

//==============================================================================
const DawRemote::Strip& DawRemote::getStrip (int channel) const noexcept
{
    if (! juce::isPositiveAndBelow (channel, kMaxChannels))
        return noStrip;
    return units[(size_t) unitOf (channel)].strips[(size_t) (channel % kStrips)];
}

int DawRemote::getCommandLed (int command) const noexcept
{
    const auto& table = commands();
    if (! juce::isPositiveAndBelow (command, (int) table.size()))
        return Off;
    const auto& c = table[(size_t) command];
    if (c.mcuNote == -2 || c.huiZone == -2)
        return isSpecialOn (command - findCommand ("SP1")) ? On : Off;
    if (c.mcuNote == -3)
        return Off;
    if (isHui())
        return c.huiZone >= 0 ? (int) units[0].huiLeds[(size_t) c.huiZone][(size_t) c.huiPort] : (int) Off;
    return c.mcuNote >= 0 ? (int) units[0].noteLeds[(size_t) c.mcuNote] : (int) Off;
}

juce::String DawRemote::getTimecode() const
{
    juce::String s;
    const int digits = isHui() ? 8 : 10;
    for (int i = 10 - digits; i < 10; ++i)
    {
        s << juce::String::charToString (timecode[(size_t) i] != 0 ? (juce::juce_wchar) timecode[(size_t) i] : ' ');
        if (timecodeDots[(size_t) i])
            s << '.';
    }
    return s;
}

juce::String DawRemote::getAssignment() const
{
    juce::String s;
    for (auto c : assignment)
        s << juce::String::charToString (c != 0 ? (juce::juce_wchar) c : ' ');
    return s;
}

juce::String DawRemote::getDisplayLine (int line) const
{
    const int width = isHui() ? 40 : 56;
    const int offset = juce::jlimit (0, 1, line) * 56;
    juce::String s;
    for (int i = 0; i < width; ++i)
    {
        const char c = units[0].lcd[(size_t) (offset + i)];
        s << juce::String::charToString (c >= 0x20 && c < 0x7f ? (juce::juce_wchar) c : ' ');
    }
    return s;
}

bool DawRemote::isConnected (int unit) const noexcept
{
    if (! juce::isPositiveAndBelow (unit, kMaxUnits))
        return false;
    const auto& u = units[(size_t) unit];
    if (isHui())
        return u.talked && nowMs - u.lastMessageMs < 3000;
    return u.talked;
}

void DawRemote::tick (juce::uint32 now)
{
    nowMs = now;
    // Meters fall by one segment every 300 ms after the DAW's last value (as on the MCU).
    for (auto& u : units)
        for (size_t s = 0; s < (size_t) kStrips; ++s)
            if (u.strips[s].meter > 0 && now - u.meterTime[s] >= 300)
            {
                --u.strips[s].meter;
                u.meterTime[s] = now;
                changed();
            }
}

void DawRemote::reset()
{
    for (auto& u : units)
    {
        for (auto& s : u.strips)
        {
            const bool touched = s.touched;
            s = Strip();
            s.touched = touched;
        }
        u.noteLeds = {};
        for (auto& z : u.huiLeds)
            z = {};
        u.lcd.fill (' ');
        u.huiZone = 0;
        u.huiFaderHigh = {};
        u.talked = false;
    }
    masterFader = 0;
    timecode.fill (' ');
    timecodeDots = {};
    assignment.fill (' ');
    changed();
}

int DawRemote::ringLeds (int ring) noexcept
{
    const int mode = (ring >> 4) & 3, value = ring & 0x0f;
    int leds = 0;
    if (value >= 1 && value <= 11)
    {
        const int p = value - 1; // 0..10, the centre is 5
        switch (mode)
        {
            case 0: leds = 1 << p; break;
            case 1:
                for (int i = juce::jmin (p, 5); i <= juce::jmax (p, 5); ++i)
                    leds |= 1 << i;
                break;
            case 2:
                for (int i = 0; i <= p; ++i)
                    leds |= 1 << i;
                break;
            default:
                for (int i = 5 - p; i <= 5 + p; ++i)
                    if (i >= 0 && i <= 10)
                        leds |= 1 << i;
                break;
        }
    }
    if ((ring & 0x40) != 0)
        leds |= 1 << 11;
    return leds;
}

//==============================================================================
void DawRemote::handleMessage (int unit, const juce::MidiMessage& m)
{
    if (! juce::isPositiveAndBelow (unit, kMaxUnits))
        return;
    auto& u = units[(size_t) unit];
    u.lastMessageMs = nowMs;
    if (isHui())
        handleHui (unit, m);
    else
        handleMcu (unit, m);
    if (! u.talked)
    {
        u.talked = true;
        changed();
    }
}

void DawRemote::setText (int unit, int offset, const juce::uint8* chars, int count)
{
    auto& u = units[(size_t) unit];
    for (int i = 0; i < count; ++i)
        if (juce::isPositiveAndBelow (offset + i, (int) u.lcd.size()))
            u.lcd[(size_t) (offset + i)] = (char) (chars[i] & 0x7f);
    if (! isHui())
        for (size_t s = 0; s < (size_t) kStrips; ++s)
        {
            juce::String upper, lower;
            for (size_t i = 0; i < 7; ++i)
            {
                const char a = u.lcd[s * 7 + i], b = u.lcd[56 + s * 7 + i];
                upper << juce::String::charToString (a >= 0x20 && a < 0x7f ? (juce::juce_wchar) a : ' ');
                lower << juce::String::charToString (b >= 0x20 && b < 0x7f ? (juce::juce_wchar) b : ' ');
            }
            u.strips[s].upper = upper.trimEnd();
            u.strips[s].lower = lower.trimEnd();
        }
    changed();
}

void DawRemote::handleMcu (int unit, const juce::MidiMessage& m)
{
    auto& u = units[(size_t) unit];
    if (m.isSysEx())
        return handleMcuSysex (unit, m.getSysExData(), m.getSysExDataSize());

    if (m.isPitchWheel())
    {
        const int ch = m.getChannel() - 1;
        if (ch == 8 && unit == 0)
            masterFader = m.getPitchWheelValue();
        else if (juce::isPositiveAndBelow (ch, kStrips))
            u.strips[(size_t) ch].fader = m.getPitchWheelValue();
        changed();
        return;
    }
    if (m.isNoteOnOrOff())
    {
        const int note = m.getNoteNumber();
        const int led = m.isNoteOn (true) ? ledOf (m.getVelocity()) : Off;
        u.noteLeds[(size_t) note] = (juce::uint8) led;
        if (note < 0x20)
        {
            auto& s = u.strips[(size_t) (note & 7)];
            switch (note >> 3)
            {
                case 0:  s.rec = led; break;
                case 1:  s.solo = led; break;
                case 2:  s.mute = led; break;
                default: s.select = led; break;
            }
        }
        changed();
        return;
    }
    if (m.isController())
    {
        const int cc = m.getControllerNumber(), v = m.getControllerValue();
        if (cc >= kRingCc && cc < kRingCc + kStrips)
            u.strips[(size_t) (cc - kRingCc)].ring = v;
        else if (unit == 0 && cc >= kTimecodeCc && cc < kTimecodeCc + 10)
        {
            // Right to left; bit 6 is the digit's dot.
            const int c = v & 0x3f;
            const size_t index = (size_t) (9 - (cc - kTimecodeCc));
            timecode[index] = (char) (c < 0x20 ? c + 0x40 : c);
            timecodeDots[index] = (v & 0x40) != 0;
        }
        else if (unit == 0 && (cc == kAssignmentCc || cc == kAssignmentCc + 1))
        {
            const int c = v & 0x3f;
            assignment[cc == kAssignmentCc ? 1 : 0] = (char) (c < 0x20 ? c + 0x40 : c);
        }
        else
            return;
        changed();
        return;
    }
    if (m.isChannelPressure())
    {
        const int v = m.getChannelPressureValue();
        const size_t s = (size_t) ((v >> 4) & 7);
        auto& strip = u.strips[s];
        const int level = v & 0x0f;
        if (level == 0x0e)
            strip.overload = true;
        else if (level == 0x0f)
            strip.overload = false;
        else
        {
            strip.meter = level;
            u.meterTime[s] = nowMs;
        }
        changed();
    }
}

void DawRemote::handleMcuSysex (int unit, const juce::uint8* d, int size)
{
    if (size < 5 || d[0] != 0x00 || d[1] != 0x00 || d[2] != 0x66)
        return;
    const auto id = d[3];
    if (id != kMcuId && id != kMcuExtenderId && id != kLogicControlId && id != kLogicExtenderId)
        return;
    const auto replyId = unit == 0 ? kMcuId : kMcuExtenderId;
    auto reply = [this, unit, replyId] (juce::uint8 command, const juce::MemoryBlock& payload)
    {
        juce::MemoryBlock data;
        const juce::uint8 header[] = { 0x00, 0x00, 0x66, replyId, command };
        data.append (header, sizeof (header));
        data.append (payload.getData(), payload.getSize());
        send (unit, juce::MidiMessage::createSysExMessage (data.getData(), (int) data.getSize()));
    };
    const auto serial = serialNumber (unit);
    const juce::MemoryBlock serialBytes (serial.toRawUTF8(), 7);

    switch (d[4])
    {
        case 0x00: // device query: the host connection query (serial + challenge)
        {
            auto payload = serialBytes;
            const juce::uint8 challenge[] = { 0x4c, 0x44, 0x41, 0x57 };
            payload.append (challenge, sizeof (challenge));
            reply (0x01, payload);
            break;
        }
        case 0x02: // host connection reply: confirm (the response isn't checked)
            reply (0x03, serialBytes);
            break;
        case 0x13: // firmware version request
        {
            const char version[] = "V1.07";
            reply (0x14, juce::MemoryBlock (version, 5));
            break;
        }
        case 0x10: // Logic Control: timecode digits, right to left
            for (int i = 5; i < size && i < 15; ++i)
            {
                const int c = d[i] & 0x3f;
                const size_t index = (size_t) (9 - (i - 5));
                timecode[index] = (char) (c < 0x20 ? c + 0x40 : c);
                timecodeDots[index] = (d[i] & 0x40) != 0;
            }
            changed();
            break;
        case 0x11: // Logic Control: assignment display
            for (int i = 5; i < size && i < 7; ++i)
            {
                const int c = d[i] & 0x3f;
                assignment[(size_t) (i - 5)] = (char) (c < 0x20 ? c + 0x40 : c);
            }
            changed();
            break;
        case 0x12: // LCD
            if (size >= 6)
                setText (unit, d[5], d + 6, size - 6);
            break;
        case 0x61: // faders to minimum
            for (auto& s : units[(size_t) unit].strips)
                s.fader = 0;
            if (unit == 0)
                masterFader = 0;
            changed();
            break;
        case 0x62: // all LEDs off
        {
            auto& u = units[(size_t) unit];
            u.noteLeds = {};
            for (auto& s : u.strips)
                s.rec = s.solo = s.mute = s.select = Off;
            changed();
            break;
        }
        case 0x63: // reset
            reset();
            break;
        default: // backlight saver, touchless faders, meter modes... nothing to show
            break;
    }
}

void DawRemote::handleHui (int unit, const juce::MidiMessage& m)
{
    auto& u = units[(size_t) unit];
    if (m.isSysEx())
        return handleHuiSysex (unit, m.getSysExData(), m.getSysExDataSize());

    if (m.isNoteOnOrOff() && m.getNoteNumber() == 0 && m.getVelocity() == 0)
    {
        // Ping: answer it to stay online.
        send (unit, juce::MidiMessage::noteOn (1, 0, (juce::uint8) 0x7f));
        return;
    }
    if (m.isController())
    {
        const int cc = m.getControllerNumber(), v = m.getControllerValue();
        if (cc < kStrips)
            u.huiFaderHigh[(size_t) cc] = v;
        else if (cc >= 0x20 && cc < 0x20 + kStrips)
        {
            u.strips[(size_t) (cc - 0x20)].fader = (u.huiFaderHigh[(size_t) (cc - 0x20)] << 7) | v;
            changed();
        }
        else if (cc == 0x0c)
            u.huiZone = v;
        else if (cc == 0x2c)
        {
            const int port = v & 7;
            const int led = (v & 0x40) != 0 ? On : Off;
            if (juce::isPositiveAndBelow (u.huiZone, (int) u.huiLeds.size()))
            {
                u.huiLeds[(size_t) u.huiZone][(size_t) port] = (juce::uint8) led;
                if (u.huiZone < kStrips)
                {
                    auto& s = u.strips[(size_t) u.huiZone];
                    switch (port)
                    {
                        case kHuiSelect: s.select = led; break;
                        case kHuiMute:   s.mute = led; break;
                        case kHuiSolo:   s.solo = led; break;
                        case kHuiAuto:   s.autoLed = led; break;
                        case kHuiVSel:   s.vsel = led; break;
                        case kHuiInsert: s.insert = led; break;
                        case kHuiRec:    s.rec = led; break;
                        default: break;
                    }
                }
                changed();
            }
        }
        else if (cc >= 0x10 && cc < 0x10 + kStrips)
        {
            u.strips[(size_t) (cc - 0x10)].ring = v;
            changed();
        }
        return;
    }
    if (m.isAftertouch())
    {
        // Meters: a0 0y sv (side s, value v 0..c); the strip shows the louder side.
        const int y = m.getNoteNumber(), sv = m.getAfterTouchValue();
        if (juce::isPositiveAndBelow (y, kStrips))
        {
            auto& s = u.strips[(size_t) y];
            const int level = juce::jmin (12, sv & 0x0f);
            s.meter = (sv & 0x10) != 0 ? juce::jmax (s.meter, level) : level;
            s.overload = level >= 12;
            u.meterTime[(size_t) y] = nowMs;
            changed();
        }
    }
}

void DawRemote::handleHuiSysex (int unit, const juce::uint8* d, int size)
{
    if (size < 6 || d[0] != 0x00 || d[1] != 0x00 || d[2] != 0x66 || d[3] != kHuiId || d[4] != 0x00)
        return;
    auto& u = units[(size_t) unit];
    switch (d[5])
    {
        case 0x10: // a 4-character channel display: yy c c c c
            if (size >= 11 && d[6] < kStrips)
            {
                juce::String name;
                for (int i = 7; i < 11; ++i)
                    name << juce::String::charToString (d[i] >= 0x20 && d[i] < 0x7f ? (juce::juce_wchar) d[i] : ' ');
                u.strips[d[6]].upper = name.trimEnd();
                changed();
            }
            break;
        case 0x11: // timecode: y0 (rightmost) .. y7
        {
            static const char hex[] = "0123456789AbCdEF";
            for (int i = 6; i < size && i < 14; ++i)
            {
                const size_t index = (size_t) (9 - (i - 6));
                timecode[index] = hex[d[i] & 0x0f];
                timecodeDots[index] = (d[i] & 0x10) != 0;
            }
            changed();
            break;
        }
        case 0x12: // the 2 x 40 main display: zone + 10 characters, repeated
            for (int i = 6; i + 10 < size; i += 11)
            {
                const int zone = d[i] & 7;
                setText (unit, (zone / 4) * 56 + (zone % 4) * 10, d + i + 1, 10);
            }
            break;
        default:
            break;
    }
}

//==============================================================================
const std::vector<DawRemote::Command>& DawRemote::commands()
{
    return commandTable();
}

int DawRemote::findCommand (const juce::String& code) noexcept
{
    const auto& table = commands();
    for (size_t i = 0; i < table.size(); ++i)
        if (code == table[i].code)
            return (int) i;
    return -1;
}

juce::StringArray DawRemote::commandGroups()
{
    juce::StringArray groups;
    for (auto& c : commands())
        groups.addIfNotAlreadyThere (c.group);
    return groups;
}

bool DawRemote::isAvailable (int command) const noexcept
{
    const auto& table = commands();
    if (! juce::isPositiveAndBelow (command, (int) table.size()))
        return false;
    const auto& c = table[(size_t) command];
    return isHui() ? c.huiZone != -1 : c.mcuNote != -1;
}

juce::String DawRemote::commandLabel (int command, int preset)
{
    const auto& table = commands();
    if (! juce::isPositiveAndBelow (command, (int) table.size()))
        return {};
    const auto& c = table[(size_t) command];
    if (const int column = translationColumn (preset); column >= 0)
        for (auto& t : translations)
            if (juce::String (t.code) == c.code && *t.labels[column] != 0)
                return t.labels[column];
    return c.label;
}

juce::StringArray DawRemote::presetNames()
{
    return { "-", "CUBASE", "LIVE", "LOGIC X", "NUENDO", "PRO TOOLS", "REAPER", "STUDIO ONE" };
}

std::vector<DawRemote::PresetKey> DawRemote::presetKeys (int preset)
{
    // DAW 1 transport, DAW 2 banks and navigation, DAW 3 automation, DAW 4
    // assignments: the MCU layout, or the HUI one for Pro Tools (the WING's
    // recommendation for it).
    static const char* const mcu[4][8] = { { "T4", "T5", "T1", "T2", "T8", "T6", "T7", "T3" },
                                           { "N6", "N7", "N8", "N9", "N5", "T11", "OT1", "OT3" },
                                           { "AU1", "AU2", "AU4", "AU5", "AU3", "OT2", "SY5", "SY6" },
                                           { "A1", "A2", "A3", "A4", "A5", "A6", "SY1", "V1" } };
    static const char* const hui[4][8] = { { "T4", "T5", "T1", "T2", "T8", "T13", "T14", "T3" },
                                           { "N6", "N7", "N8", "N9", "N5", "T11", "T12", "T19" },
                                           { "AU1", "AU2", "AU4", "AU5", "AU3", "AU6", "SY5", "SY6" },
                                           { "A7", "A8", "A3", "A12", "V10", "V11", "V12", "V13" } };
    std::vector<PresetKey> keys;
    const auto& table = preset == 5 ? hui : mcu;
    for (int layer = 0; layer < 4; ++layer)
        for (int key = 0; key < 8; ++key)
            keys.push_back ({ layer, key, findCommand (table[layer][key]) });
    return keys;
}

juce::String DawRemote::presetColumnName (int preset, int layer, int column)
{
    static const char* const mcu[4][4] = { { "WIND", "LOCATE", "STOP", "PLAY" },
                                           { "BANK", "BANK", "CHANNEL", "CHANNEL" },
                                           { "AUTO", "AUTO", "AUTO", "AUTO" },
                                           { "ASSIGN", "ASSIGN", "ASSIGN", "ASSIGN" } };
    static const char* const hui[4][4] = { { "WIND", "LOCATE", "LOCATE", "PLAY" },
                                           { "BANK", "BANK", "CHANNEL", "CHANNEL" },
                                           { "AUTO", "AUTO", "AUTO", "AUTO" },
                                           { "SENDS", "SENDS", "VIEW", "VIEW" } };
    const auto& table = preset == 5 ? hui : mcu;
    return table[juce::jlimit (0, 3, layer)][juce::jlimit (0, 3, column)];
}

} // namespace livedaw
