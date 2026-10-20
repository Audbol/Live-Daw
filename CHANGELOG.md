# Changelog

All notable changes to Sidecar are listed here. Versions follow
[semantic versioning](https://semver.org); 0.x releases are betas.

## [Unreleased]

### Added

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
- WING remote: more of the WING 3.1 parameter tree drives the console: 87.4 % of its parameters
  (59,950 of 68,556, up from 82.8 %). New: LISTEN from remote apps (BAND SOLO with
  its band, KEY SOLO of the GATE and DYNAMICS slots, XOVER SOLO on channels, aux,
  buses, mains and matrices), the inserts' state, which strip uses each FX slot, a
  channel's source GAIN and +48V, the active RTA source and settings, the monitor
  paths' actual level, scene control (`/$ctl/lib`: GO, GO NEXT, GO PREV, GO TAG,
  PREV / NEXT, the current scene and show), and the show's fader layers and custom
  controls both ways (`/$ctl/layer`, `/$ctl/user`: layer keys, spill, scroll
  position, user layer names and fader assignments, edited layer keys, each custom
  control's functions, name and colour, the CC layer).
- Plugin delay compensation: the latency each hosted plugin reports (and its
  changes) is compensated so that every source summed into a bus, main or matrix
  arrives in time, across bus stages; a plugin's FX MIX delays its dry signal to
  match. On by default; SETUP > PLUGINS turns it off and shows the total
  compensation and each loaded plugin's latency. Compensation stops at 8192
  samples; a plugin reporting more is logged and the limit is shown.

### Changed

- Only the custom controls' GPIO and SD functions (no hardware) are still drawn
  disabled; the start-up notice and the known limitations say so.

### Fixed

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
