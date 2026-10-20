# Changelog

All notable changes to Sidecar are listed here. Versions follow
[semantic versioning](https://semver.org); 0.x releases are betas.

## [Unreleased]

### Fixed

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
- GATE / DYNAMICS: KEY TAP is drawn disabled; it looked active but the engine always
  keys from the source channel's input (listed under the known limitations).
- CHANNEL COPY / INIT: CONN now copies the ALT input and the MAIN / ALT switch too,
  SEND copies TAP WIDTH and TAP EQ, and a MACH EQ4 isn't copied onto an aux channel
  (which doesn't offer it).
- Channel strip section: the INPUT encoder's DELAY now has the INPUT screen's range
  (up to 500 ms in every unit, at the current sample rate); it stopped at 150 m,
  500 ft or 24000 samples, so the two controls disagreed.
- INPUT: the TRIM & BALANCE pad's balance handle follows the ST key while the screen
  is open (it stayed disabled, or enabled, until the screen was opened again).
- EFFECTS: the rack's slot numbers 10 and 14 showed as "..." (the number box was too
  narrow for two digits).

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
