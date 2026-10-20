# Changelog

All notable changes to Sidecar are listed here. Versions follow
[semantic versioning](https://semver.org); 0.x releases are betas.

## [0.1.1] - beta

The second beta: plugin delay compensation, an optional plugin sandbox (experimental),
the WING's oscillator, scene and snippet files, the CLIP player, SEND MIDI, MIDI CC
faders, gate and dynamics KEY TAP, matrix send PAN LINK, much more of the WING remote
protocol (88.2 % of its parameters) and many fixes found by using the console end to
end. Known limitations: see the
[README](https://github.com/Audbol/Live-Daw/blob/main/README.md#known-limitations).

### Added

- The WING's **oscillator**: the OSCILLATOR source group on ROUTING > SOURCES has OSC 1
  and OSC 2, which generate a sine (20 Hz to 20 kHz), pink or white noise at -40 to
  -6 dBFS. Patch them to channels like any input; SOURCES shows SINE / PINK / WHITE,
  FREQ and LEVEL for them, MUTE switches them off and STEREO pairs them. Saved with
  the show and driven by WING remote apps (`/io/in/OSC`).
- LIBRARY > SHOW > **SEND MIDI**: MIDI data (hexadecimal bytes, `U:` / `D:` for the
  USB / DIN port) that a scene sends each time it is recalled, on the EDIT MIDI TEXT
  STRING window; it goes out of EXTERNAL MIDI CONTROL's port and is saved with the
  show.
- **MIDI CC faders** in the layers (VIEW > CONFIG, the MIDI CC field): a fader sends
  its CC out of EXTERNAL MIDI CONTROL and follows the CC coming in.
- **FADER SPILL** custom control keys: spill a DCA, a bus's sources, an FX slot,
  automix group X / Y or the selected channel's sends onto the left section.
- An **RTA on MONITORS EQ** (the RTA key): the monitor path's spectrum, PRE or POST
  its EQ, drawn as EQ/RTA SETTINGS say.
- LIBRARY: scene and snippet files. EXPORT on the SNAP and SNIP tabs saves the selected
  scene (with its snippets) or snippet as a `.lsnap` / `.lsnip` file in the INT or USB
  folder; IMPORT (or a double-click on the file) adds it to the show.
- LIBRARY: the CLIP tab plays an audio clip (WAV, FLAC, AIFF, MP3) chosen at INT / USB
  on the USB player, with PLAY, PAUSE, STOP, meter, time and format.
- WING remote: more of the WING 3.1 parameter tree drives the console: 88.2 % of its parameters
  (60,491 of 68,556, up from 82.8 % in 0.1.0). New: LISTEN from remote apps (BAND SOLO with
  its band, KEY SOLO of the GATE and DYNAMICS slots, XOVER SOLO on channels, aux,
  buses, mains and matrices), the inserts' state, which strip uses each FX slot, a
  channel's source GAIN and +48V, the active RTA source and settings, the monitor
  paths' actual level, scene control (`/$ctl/lib`: GO, GO NEXT, GO PREV, GO TAG,
  PREV / NEXT, the current scene and show), and the show's fader layers and custom
  controls both ways (`/$ctl/layer`, `/$ctl/user`: layer keys, spill, scroll
  position, user layer names and fader assignments, edited layer keys, each custom
  control's functions, name and colour, the CC layer).
- **Matrix send PAN LINK** (HOME > MATRIX SENDS, the pan ring's pop-up; FEED
  CONFIGURATION's SEND PANNING): a channel's or aux's matrix send pans with the
  channel (LINK REVERSED mirrors it), or with its own PAN when unlinked. Saved with
  shows and scenes.
- A DCA's **icon** and scribble strip **LIGHT** (VIEW > OVERVIEW, the DCA's name),
  shown on the fader strips and saved with the show.
- WING remote also drives the matrix sends' PAN LINK, `$muteovr` on every strip,
  DCA `icon` / `led`, the USB recorder and player (`/rec`, `/play`: transport, PLAYFILE, position,
  REPEAT, REC PATH, format, state and file) and the recall safes (`/$ctl/safes`:
  strips, DCAs, mute groups, FX).
- Plugin delay compensation: the latency each hosted plugin reports (and its
  changes) is compensated so that every source summed into a bus, main or matrix
  arrives in time, across bus stages; a plugin's FX MIX delays its dry signal to
  match. On by default; SETUP > PLUGINS turns it off and shows the total
  compensation and each loaded plugin's latency. Compensation stops at 8192
  samples; a plugin reporting more is logged and the limit is shown.
- Experimental plugin sandbox: SETUP > PLUGINS > RUN PLUGINS IN SANDBOX (off by
  default) runs each plugin in its own helper process (`SidecarPluginHost`, shipped
  next to the app). Audio goes through shared memory with a bounded wait, so a plugin
  that crashes or hangs only bypasses its FX slot (dry signal, event log entry); the
  helper is restarted with the plugin's last known state. State save / restore and
  parameter changes work through the sandbox; the plugin's own editor opens in a
  window of the helper process. The sandbox's one block of latency is reported like a
  plugin's own, so delay compensation covers it.
- GATE / DYNAMICS **KEY TAP**: a key from another channel (KEY SOURCE) is taken at
  that channel's tap point: IN (after the input trim), FILT, 3, 4, 5, PFL, AFL or
  POST, so a key taken AFL or POST follows the source's fader and mute. A key from a
  channel later in the console arrives in the same block (channels run in order of
  their keys); keys that form a cycle, or a channel keyed by itself, are one block
  late. Saved with shows and scenes and set from WING remote apps (`gatesc/tap`,
  `dynsc/tap` on channels). Until now a key from another channel was always that
  channel's raw input, before its trim.

### Changed

- The default console name on the network (WING discovery, MIDI identity) is now
  SIDECAR instead of LIVE-DAW, and the WING remote reports the model "sidecar". A
  name set on SETUP > GENERAL is kept.
- Matrix sends of input and aux channels pan instead of balancing the channel's
  panned output: linked (the default) they follow the channel's pan, unlinked they
  use their own. Shows from before load a centred send linked (it sounds as before)
  and a panned one with its pan. The engine's ABI version is 11.

- Scene fades also glide DCA faders, strip widths and the bus and matrix send pans
  (before: faders, pans and send levels only). Switches and settings still apply when
  the fade starts.
- Scene fades move levels (faders, sends, DCAs) along the fader's travel, as a moving
  fader does, instead of in a straight line in dB; a fade up from -oo is now heard from
  the start rather than only in its second half.
- Channel strip section with an analog EQ model: SHIFT pages through all of the
  model's parameters four at a time (1-4, 5-8, 9-12, 13-16), so models with 12-16
  parameters are reachable from the encoders; the captions show the page and the
  parameter on each encoder.
- Only the custom controls' GPIO and SD functions (no hardware) are still drawn
  disabled; the start-up notice and the known limitations say so.

### Fixed

- A scene fade overwrote a fader (or send, pan, DCA) moved while it ran until the
  fade ended. Moving one now takes it out of the fade, from the surface, the screen,
  MIDI, a DAW remote or a WING remote.
- The send pan pop-up (HOME > BUS / MATRIX SENDS, the pan ring) had only an on/off
  link key, so LINK REVERSED could only be set on FEED CONFIGURATION. Its PAN LINK is
  now OFF / LINK / LINK REVERSED.
- HI CUT reset (double-click) went to 10 kHz while a new or INITed channel has 18 kHz;
  it now resets to 18 kHz on the FILTER screen, the channel strip section and the
  4-Channel section. Double-clicking the LC, HC (or all-pass) line on the FILTER graph
  resets it too.
- ROUTING repainted its whole page ten times a second for the source meters; on a slow
  machine (or a debug build) with a large display the screen then stopped reacting to
  clicks. Only the meters that move are repainted now, the rest twice a second.
- The window reopens fitted to its screen: a size saved on a bigger screen (an external
  monitor since unplugged) left part of the surface off screen.
- Recorder: RECORD ALL CHANNELS (or any track) failed with "Input ... doesn't exist"
  for a channel patched to a USB PLAYER source; those sources always exist.
- HOME > NAME took no typing from the computer keyboard until the on-screen keyboard
  was clicked, and a space typed at the end of a name vanished; the default name
  ("Ch 2") now starts as an empty field, so typing replaces it.
- Scribble strips: a long name keeps its size, runs on to the right and breaks onto a
  second row at a space, as on the WING, instead of being squeezed onto one row.
- VIEW > CONFIG: deleting the user layer on USER 1 moved USER 2's layer onto USER 1 and
  left nothing to delete on USER 2. The USER keys now keep their layers, and new user
  layers are named after their key instead of copying the shown layer's name.
- The recorder screen's keys, labels and readouts fit their boxes at every window
  size, from 1152 x 680 to 4K and ultrawide.
- ROUTING > SOURCES: the GAIN knob set only the selected source of a stereo or
  mid/side pair; on the pair's second source it changed nothing you could hear (the
  pair plays at the first source's gain). It now sets both, in 0.5 dB steps, like
  the fader and the wheel.
- Spills (DCA SPILL, a bus's sources) didn't follow the console: a strip assigned to
  the spilled DCA, or a send switched on to the spilled bus, only showed up after
  another layer change. The spill now updates at once.
- HOME > NAME: the name field always drew its caret at the end and dropped a space
  typed at the end, so after the arrow keys moved the caret, typing went somewhere
  else than the caret showed. The field now shows the text as typed with the caret
  where typing goes.
- 4-Channel section: in GAIN / 48 V mode the keys under the encoders did nothing;
  they now switch the +48V of the strip's source (stored, as on HOME > INPUT and
  ROUTING > SOURCES) and light while it is on.
- ROUTING > SOURCES: switching a source to STEREO or MID/SIDE left the channels
  already patched to it mono (they only heard the left source), and switching back
  to MONO left them stereo. Channels and aux on the pair's first source now follow
  its mode, as on the console and as WING remote apps already did.
- HOME > OVERVIEW: the INPUT panel's source MUTE key was always disabled (from
  before sources were modelled). It now mutes the channel's source (both sources of
  a pair) as ROUTING > SOURCES does, and lights while the source is muted.
- VIEW > MUTE GROUPS: the SOURCES box always showed "-", even with sources tagged to
  the group on ROUTING > SOURCES > TAGS, and UNASSIGN left those tags. The box now
  shows the tagged source (or how many) and UNASSIGN clears them too.
- LIBRARY > SHOW > SAVE AS: the show takes the new name (it kept the old one, so the
  title bar and the LIBRARY showed the name of the show it was copied from).
- LIBRARY: renaming the open show's file (RENAME at INT / USB) left the show saving to
  the old file name, so the next SAVE brought the old file back.
- The parameter bar under the screen printed a long name and value over each other
  (recorder screen: POSITION, LOOP OUT).
- Two-line key captions were cut or wrapped onto a third line when the key was narrow
  (LIBRARY GO PREV / GO NEXT, recorder RECORD ALL CHANNELS); they now shrink to fit.
- Text cut in fixed-size boxes on large windows: EFFECTS slot numbers 10 and 14 showed
  "...", the recorder's marker times and the SNIP counter's PARAMETERS were cut.
- USB recorder / player: renaming a clip (or its folder) that is in the open playlist
  left the playlist pointing at the old file, so the clip showed as missing
  (1970-01-01) and couldn't play.
- SETUP > GENERAL > CONSOLE NAME only took effect after a restart: the running WING
  remote kept announcing the old name, and the MIDI identity (SysEx) kept it in both
  places it can be edited.
- FX: the Vintage Room's early reflections and the Concert reverb's reflection
  clusters now sit at their ER / REFL times as soon as the device is loaded (they
  stayed at the start until the time was changed).
- FILTER: HI CUT goes down to 50 Hz as on the WING (and as the engine allows) on the
  FILTER screen, its graph, the channel strip section and the 4-Channel section; it
  stopped at 200 Hz.
- INPUT: with a negative BALANCE the post-trim meter showed the right side boosted
  instead of turned down.
- EQ: with an analog EQ model, every 6th parameter (SOUL's LM GAIN and HF GAIN...)
  never came onto the knobs: the knob pages held five parameters but were chosen
  in sixes.
- CHANNEL COPY / INIT: CONN now copies the ALT input and the MAIN / ALT switch too,
  SEND copies TAP WIDTH and TAP EQ, and a MACH EQ4 isn't copied onto an aux channel
  (which doesn't offer it).
- Channel strip section: the INPUT encoder's DELAY now has the INPUT screen's range
  (up to 500 ms in every unit, at the current sample rate); it stopped at 150 m,
  500 ft or 24000 samples, so the two controls disagreed.
- INPUT: the TRIM & BALANCE pad's balance handle follows the ST key while the screen
  is open (it stayed disabled, or enabled, until the screen was opened again).
- Multitrack editor: an automatic crossfade stayed on both clips after they were
  moved apart (or one was deleted), so each faded out or in over nothing. Moves,
  nudges, trims, deletes and cuts now take back the fades of crossfades whose
  clips no longer overlap; nudges and trims also crossfade new overlaps, as moves
  did.
- Recorder: after the clock rate changed (SETUP > AUDIO), recording went on into the
  open project at its old rate, so the take was placed and played at the wrong
  length and speed (an octave off after a 48 / 96 kHz change). A project without
  audio now moves to the console's rate (markers and loop keep their times), and
  one with audio refuses to record and says why.
- Recorder screen: pressing VIRTUAL SOUNDCHECK again went back to MAIN but left
  SETUP > AUDIO > GLOBAL INPUT SELECT OVERRIDE on, so channels set to ALT on their
  own were stuck on MAIN. Leaving virtual soundcheck now puts INPUT SELECT back as
  it was.
- SETUP > GENERAL counted one DSP thread less (the workers only) than the event log
  and SETUP > LOG, which count the audio thread too.
- LIBRARY > SNAP / SNIP > EXPORT: the scene or snippet in the file kept its old name
  when it was exported under another one, so the list showed the file as "Scene 1x"
  but IMPORT added "Scene 1". The exported scene or snippet now takes the name it
  was saved under.
- LIBRARY (INT / USB) and the USB RECORDER / PLAYER browser didn't show files copied
  into, deleted from or renamed in their folder outside the app until the screen
  was opened again; they now follow such changes within a second.
- HOME INS screens: the FX PROCESSOR list shows NONE and all 16 slots without
  scrolling (FX 9-16 beside FX 1-8), and FX SPILL is unavailable while the insert
  has no FX processor.
- USB player: "1 SONG" instead of "1 SONGS"; the timeline shows the clip's length
  as soon as it is known (from the file's header before it has played), as the
  player panel does; the meters' CLIP label no longer gets cut in small windows.
- LIBRARY: the SNAP scene NOTES are in the screen's body type (they were tiny);
  SNIP's REC key reads white on red while recording; MONITORS shows SRC LEVEL,
  SRC MIX, TALK LVL and DIR LVL in dB.
- Small windows keep every section of the surface: the minimum window is now
  1344 x 680 (was 1152 x 680, which hid the 4-Channel section and the custom
  controls' upper part), the custom controls stay between the fader sections down
  to 40 px strips, and the top row keeps the WING's arrangement. Dropdown fields
  (VIEW > CONFIG and others) shrink their text instead of cutting it.
- Small windows: panel titles, ROUTING's fields and group selectors, the MONITORS
  tabs, fields and EQ key shrink their text instead of cutting it ("PHO...",
  "ERRO..."); METERS' two-digit strip numbers no longer overlap; SETUP > GENERAL's
  USB HOST SPEED row is no longer cut off; long event log entries wrap onto a
  second line (the tooltip shows the whole entry). MONITORS' unlit MUTE key reads
  on its dark red.
- Small windows, HOME and other screens: two-line keys no longer break a word
  ("PARTIA L UPDATE"); key labels (DELAY, FILTER), format chips, the dynamics
  graphs' level scale ("-2" for -20), the sends' TAP / POST and PAN LINK and the
  VIEW > SENDS values fit their space; the EQ screen's FILTER key keeps its room.
- Screen only on a large display: the SNAP scene's FADE and NOTES rows no longer
  overlap, slider values (CEILING, FADE, MOUSE SPEED) use the screen's type, and
  SETUP > AUDIO's INPUT SELECT captions grow with the screen (in a small window its
  cells are no longer squeezed out by them).
- Automix: a channel set to AUTO X / AUTO Y with its INS 2 ON key off was listed on
  the automix screen, spilled by AUTOMIX FADER SPILL and lit on the surface's AUTO
  LEDs, but not gain-shared. As on the console, INS 2's ON key switches automix in
  and out: only AUTO X / Y with INS 2 ON is a member, everywhere. The selected
  channel stays on its own automix screen while its INS 2 is off, drawn as not a
  member.
- Automix screen: the channel columns now follow other channels joining or leaving
  the groups (from their own INS 2, a scene or a remote app) and channels being added
  or removed, instead of only refreshing when the screen was opened.

## [0.1.0] - beta

The first public release. Sidecar was developed as "Live DAW" before it was renamed;
settings, shows and folders from those builds are picked up automatically.

### Console

- WING topology: 40 input channels, 8 aux, 16 stereo buses, 4 stereo mains, 8
  stereo matrices, 16 DCAs, 8 mute groups and 16 FX slots; more channels and buses
  can be added. Buses 9-16 feed buses 1-8; matrices take a direct input.
- Channel processing in the WING's order: INPUT (trim, balance, polarity, delay),
  FILTER (low / high cut, tilt, max, all-pass), a GATE and a DYNAMICS slot with the
  WING's 32 dynamics models, the 6-band WING EQ (8 bands on buses) and the analog
  EQ models, PRE and POST inserts, automix (groups X / Y), the 24 process orders and
  all bus send taps.
- FX rack with the WING's devices: 26 premium effects, 32 standard effects, 5
  channel strips, and VST3 / AU / LV2 plugins in any slot.
- Solo system (LIVE, STUDIO, SIP), mute groups, DCAs, Sends on Fader, source solo,
  talkback (TALK A / B) and the monitoring paths (phones and speakers, each with
  source, dim, EQ, delay and limiter).
- Sources with gain, polarity, mute, stereo / mid-side pairing and mute-group tags;
  MAIN / ALT input select per channel and globally.

### Surface and screens

- The WING control surface on screen, resizable to any window, with a display-only
  mode (F10) and full screen (F11).
- Three fader sections with the WING's layers, user layers, the shift keys, auto
  width and spill (DCA, sends, FX parameters); fader strips with scribble strips and
  meters as on the console's 3.x firmware.
- The main display's screens: HOME (with the slot screens and model faceplates),
  EFFECTS, METERS, ROUTING (CHANNELS / SOURCES / OUTPUTS), SETUP (GENERAL, AUDIO,
  SURFACE, REMOTE, DAW, PLUGINS, LOG), LIBRARY, the VIEW screens (OVERVIEW, FADERS,
  SENDS, CONFIG, MUTE GROUPS), MONITORS, CC EDIT and UTILITY.
- Channel strip section, monitoring panel, 4-Channel section and custom controls
  (16 layers, USER, 2-TRACK and MULTI transports, mute groups, show control).

### Shows and recording

- Shows with scenes (recall scope, safes, fades, partial load / update) and snippets;
  FX and channel presets; autosave every 30 s and atomic show files with backups.
- Multitrack recorder: up to 64 tracks recorded and played back into the console,
  record all channels, virtual soundcheck, markers, loop, WAV / AIFF / FLAC / MP3
  formats, and a multitrack editor (clips, fades, crossfades, import, export).
- The WING's 2/4-track USB recorder and player with playlists, using a folder on disk.

### Remote control and interoperability

- MIDI remote control (channel control, FX parameters, custom controls, SysEx, scene
  and show control) and DAW remote control (Mackie Control with extenders, HUI) with
  DAW mode on the surface. Virtual MIDI ports on macOS and Linux.
- Experimental WING remote: discovery, OSC and the native protocol on the WING 3.1
  parameter tree (82.8 % of its parameters drive the engine); import of WING
  snapshots and shows, export of WING snapshots.

### Stability

- Realtime engine in Rust: no allocations, locks or file I/O on the audio thread,
  parallel strip processing on a realtime worker pool, unused channels cost nothing.
- Plugin faults (NaN / Inf, exceptions) bypass the plugin on the spot; plugin
  scanning in a separate process; plugins that crash while loading are blacklisted;
  safe mode after a crash; audio device watchdog; output safety clip.
- Audio drivers: ASIO and WASAPI (Windows), CoreAudio (macOS), ALSA and JACK
  (Linux), plus a virtual device for machines without an audio interface.

### Known limitations

See the [README](README.md#known-limitations).
