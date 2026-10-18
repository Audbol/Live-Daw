#pragma once

#include "engine/MidiRemote.h"
#include "ui/Knobs.h"
#include "ui/UiContext.h"

/** The UI side of MIDI remote control and DAW remote control: connecting the
    service to the console's views, the custom controls' MIDI and DAW
    functions, and the DAW mode of the surface. */
namespace livedaw::ui::midiui
{

/** Hooks the service to the UI (custom controls received over MIDI, recalls
    in the log, DAW mode redraws) and applies the saved settings. */
void connect (UiContext&);

/** Saves `settings` in the app settings and applies them. */
void applySettings (UiContext&, const MidiRemote::Settings&);

/** The DAW remote (nullptr without the MIDI service). */
DawRemote* daw (UiContext&);
/** DAW mode is on (the DAW key). */
bool dawModeOn (UiContext&);

//==============================================================================
// Custom control functions (category codes in CustomControlsPage.h).

/** MIDI CC/NOTE/PGM keys: `target` = type * 16 + MIDI channel (0-15), `option`
    = the CC, note or program number. */
enum MidiKeyType
{
    CcToggle,
    CcPush,
    NoteToggle,
    NotePush,
    ProgramChange,
    kNumMidiKeyTypes
};
juce::String midiKeyTypeName (int type);

/** DAW V-POTS (an endless knob turning a V-pot or the jog wheel). */
KnobBinding bindDawVpot (UiContext&, int function);
/** MIDI CC: an absolute knob sending CC `cc` on MIDI channel `channel` (0-15). */
KnobBinding bindMidiCc (UiContext&, int channel, int cc);

void pressDawCommand (UiContext&, int command, bool down);
void pressMidiKey (UiContext&, int target, int option, bool down);
/** The LED of a DAW command (SurfaceKey::Led: 0 off, 1 lit, 2 blinking). */
int dawCommandLed (UiContext&, int command);
bool midiKeyLit (UiContext&, int target, int option);

juce::String describeDawCommand (UiContext&, int command);
juce::String describeMidiKey (int target, int option);
juce::String describeMidiCc (int channel, int cc);

//==============================================================================
// The custom controls section in DAW mode (WING DAW Documentation: the lower
// part's layer keys are DAW 1-4; with USE UPPER CC FOR DAW and a DAW layer the
// upper part is four channels of the emulated controller: encoder = V-pot,
// upper key = V-pot push, lower key = REC, UP / DOWN = blocks of four; the
// wheel is the jog wheel, the arrows the cursor keys).

/** DAW mode, USE UPPER CC FOR DAW and a DAW layer on the lower part. */
bool upperIsDaw (UiContext&, bool dawLayerSelected);
/** The DAW channel of an upper encoder column (0-3) in the current block. */
int upperChannel (UiContext&, int column);
KnobBinding bindUpperVpot (UiContext&, int column);
/** Upper keys 0-3: V-pot push, 4-7: REC of the column's channel. */
void pressUpperKey (UiContext&, int key, bool down);
int upperKeyLed (UiContext&, int key);
/** UP / DOWN: the next / previous block of four channels. */
void stepUpperBlock (UiContext&, int delta);
/** The upper LCD: the four channels' scribble text and V-pot rings. */
void paintUpperLcd (juce::Graphics&, juce::Rectangle<int> area, UiContext&);
/** The two-digit display: the DAW's assignment display ("PN"), or empty. */
juce::String assignmentDisplay (UiContext&);
/** The wheel as the jog wheel (not while the DAW plays with DISABLE WHEEL DURING PLAY). */
void turnWheel (UiContext&, int steps);
/** The navigation arrows as the DAW's cursor keys. */
void pressArrow (UiContext&, int dx, int dy);
/** The DAW key: DAW mode on / off (with DAW ENABLE); false if DAW control is off. */
bool pressDawKey (UiContext&);

} // namespace livedaw::ui::midiui
