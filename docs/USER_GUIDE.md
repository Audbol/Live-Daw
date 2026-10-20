# Sidecar user guide

A tour of what Sidecar does, section by section. For installing, the first steps
and building from source, see the [README](../README.md); for how it works inside,
see [ARCHITECTURE.md](ARCHITECTURE.md).

Sidecar follows the Behringer WING's own screens and terms, so the WING's manual is
a good companion; this guide says where Sidecar differs. Sidecar is not affiliated
with or endorsed by Behringer or Music Tribe; "WING" is their trademark.

## A WING on your computer

- **Console topology:** 40 input channels, 8 aux channels, 16 stereo buses, 4 stereo
  mains, 8 stereo matrices, 16 DCAs, 8 mute groups and 16 FX slots. Unlike the
  hardware, the channel count is only limited by CPU and more buses can be added.
- **Channel processing, in the WING's order:**
  - **INPUT:** trim, balance, polarity and delay (m, ft, ms or samples).
  - **FILTER:** low cut (6-24 dB/oct), high cut (6/12 dB/oct) and the tool filter
    (TILT, MAX, AP1, AP2).
  - **GATE slot and DYNAMICS slot:** each holds any of the WING's **32 dynamics models**
    (gate, ducker, Even 88, Soul 9000, 160 VCA, LA leveler, 1176-style, Fair Kid,
    Bus comp, NSTR, limiters, de-esser...), with sidechain filter, source and tap,
    and a crossover band on the dynamics slot.
  - **EQ:** the 6-band WING EQ (8 bands on buses) or the analog models (SOUL, E88,
    E84, F110, PULSAR, MACH4, PIA on buses), with blend/mix.
  - **PRE INSERT and POST INSERT** use the 16 FX slots; the post insert can also join
    automix group X or Y (gain sharing).
  - Gate, EQ, dynamics and the pre insert run in any of the **24 process orders**, and
    bus sends tap at IN, FILT, 3, 4, 5, PFL, AFL or POST.
  - Fader, DCAs, mute groups, width and pan; sends to the 16 buses (PRE, POST or GRP
    mode, with pan), to the 4 mains and the 8 matrices.
- **Buses 9+ feed buses 1-8**, matrices take the mains and a **direct input** from any
  channel (invert it for a mix-minus).
- **FX rack:** 16 slots, used by any strip's insert, each holding one of the WING's
  devices:
  - **26 premium effects** (slots 1-8): 16 reverbs (hall, room, chamber, plate,
    concert, ambience, vintage room/reverb/plate, gated, reverse, delay+reverb,
    shimmer, spring, VSS3, blue plate), dimension chorus, stereo chorus and flanger,
    stereo, ultratap, tape, oil can and BBD delays, stereo and dual pitch.
  - **32 standard effects:** graphic EQs, the analog EQs (Soul, Even 88/84,
    Fortissimo 110, Pulsar, Mach EQ4), dynamic EQ, multiband compressor, limiter,
    de-esser, enhancer, exciter, psycho bass, sub octaver and sub monster, phaser,
    tremolo/panner, rotary speaker, mood filter, tape machine, double vocal, pitch fix,
    five guitar/bass amps, body resonance, stereo imager and speaker manager.
  - **5 channel strips:** EVEN, SOUL, VINTAGE, BUS and MASTER.
  - **EXT:** a **VST3, AU (macOS) or LV2 (Linux) plugin**.
- **Plugin delay compensation:** plugins that report a latency (lookahead limiters,
  linear-phase EQs...) delay their signal. Sidecar reads each plugin's latency and
  delays the other sources summed into the same bus, main or matrix by the
  difference, so parallel paths stay in time: a channel going to Main 1 directly and
  through a bus with a plugin arrives once, not twice. The compensation follows the
  bus stages (buses 9-16 into buses 1-8, buses into mains, mains into matrices) and
  the tap of each send (an insert counts only once the send's tap point is past it).
  A plugin's FX MIX delays its dry signal by the plugin's latency too. It is on by
  default; **SETUP > PLUGINS > DELAY COMPENSATION** turns it off, and the panel
  below shows the total compensation (the longest path the console is aligned to)
  and each loaded plugin's latency. Compensation stops at 8192 samples (about 170
  ms at 48 kHz): a plugin reporting more is written to the event log and the panel
  shows the limit. Solo, the monitor paths, direct outs and the recorder's taps are
  not compensated, and a bypassed plugin adds no latency (the others are realigned
  within a tenth of a second).
- Processing models are recreated from the WING's parameter set (controls, ranges and
  behaviour), not circuit-modelled. The model tables are generated from the console's
  published parameter map.

## The surface

The window is a WING control surface and resizes to any screen, including ultrawides.
The screens follow the WING's own: the same panels, keys, colours and layout, set in
Barlow Condensed.

- **Three fader sections:** left (12 faders), center (8) and right (4), each with the
  WING's layer column: **VIEW** (the section's OVERVIEW; hold for CONFIG), seven layer
  keys (left `1-12` ... `37-40 AUX IN`, `BUS MASTER`; center `DCA`, `MAIN MATRIX`,
  `AUX IN`, `INPUTS`, `BUS MASTER`; right `MAIN MATRIX`, `DCA`, `INPUTS`, `AUX IN`,
  `BUS MASTER`; all with `USER 1` / `USER 2` for your own banks) and the shift keys
  **◄4 / 4►**, which scroll longer layers four faders at a time (hold: to the start /
  end). Set a section to **Auto** (SETUP > SURFACE) and it shows as many faders as fit,
  which suits big channel counts. Layers hold strips, DCAs, **send levels** and **FX
  parameters**.
- **Spill** puts a computed layer on a section: a DCA's members (DCA SPILL in SETUP >
  SURFACE, then SELECT on the DCA), a strip's sends (SPILL on BUS / MATRIX SENDS) or an
  FX device's parameters (FX SPILL on the insert screens; MUTE sets a parameter to its
  default, an EQ gain to 0 dB). Spill from the right section goes to the center
  section, everything else to the left; the same key again brings back the layer and
  its scroll position.
- **Fader strips** follow the WING's: a colour light bar, the scribble strip (number,
  source ports or mono, icon, the name on an inverted bar), SELECT, SOLO, the meter
  block (DYN LED, a stereo six-segment input meter, GATE LED), MUTE and the fader with
  its printed scale. SELECT on the selected strip again opens its HOME screen. MUTE
  flashes while a mute group, a DCA mute or SIP mutes the strip; with MUTEGROUP/SIP
  OVERRIDE, pressing it unmutes the strip until that mute changes.
- **SOF FLIP (Sends on Fader):** select a bus, main or matrix and the input faders set
  their send to it; select an input and the bus, main and matrix faders set its sends.
- Surface keys act on a press; keys with a second function act on **hold** (half a
  second, or right-click): slot on/off, VIEW for CONFIG, the shift keys.
- **Main display**, a 16:10 screen with the seven screen keys on its left (over its
  full height) and, on its right, **CLR SOLO**, the main L/R meter (right-click: MONITOR
  or MAIN 1-4), the knob for fine adjustments of the last touched control and the
  blank key. **Screen only** (F10, Ctrl+Shift+F, the maximize key under the right-hand
  column or a double-click on an empty part of the status bar; Esc returns) shows just
  the display with its keys and knobs, as large as the window allows; **F11** makes the
  window full screen. Both work together and are remembered.
  - The **status bar**: the selected channel's tab in its colour (number box, icon,
    name; the number opens **CHANNEL SELECT**, the rest Channel Home; a red TALKBACK
    frame on the talkback channel and while talking), the status tiles (the SIP badge
    lights with SOLO MODE SIP, filled while it mutes), the screen's own keys (INIT /
    COPY / SETTINGS on HOME) and the clock.
  - The **parameter bar** under the screen: six controls that follow what the screen
    shows (drag, mouse wheel, double-click to reset), values coloured by type as on
    the console; touching one outlines the control it changes.
  - **HOME** for the selected strip: the toolbar (thumbnails at the WING's fixed rows,
    the fader bar with the TAP point, MAIN, SENDS; on Channel Home also the on keys
    and the wrench to drag the slots and the TAP point), the **OVERVIEW** (INPUT,
    FILTER, LEVEL, EQ and MAIN panels), **ICON/COLOR**, **NAME**, **TAGS** (DCAs, mute
    groups, the talk tags TALK ON / TALK SOLO for both talk keys, A or B) and the
    **SAFE** key. The slot screens follow chapter 6 of the manual: INPUT (CHANNEL
    INPUT, TRIM & BALANCE pad, FILTER, DELAY), FILTER SETUP, GATE / COMP with the
    model's faceplate, EQ, INS 1 / INS 2 (with automix), MAIN SENDS and BUS / MATRIX
    SENDS. **INIT** and **COPY** open CHANNEL INIT / COPY (scope and destinations);
    a bus, main or matrix's feed grid (on HOME, or the INPUT screen's BUS FEED / FEED
    box) opens its **FEED CONFIGURATION** (WING 3.0.5): the CHANNEL, AUX and BUS
    sources (disabled sends greyed out), selected by touch and drag or with NONE /
    ALL / ALL CH / ALL AUX / ALL BUS, then SEND MODE, IGNORE CHANNEL MUTE, SEND
    PANNING, SEND LEVEL and SEND ENABLE, each with KEEP, changed on APPLY. Holding an
    EQ band key resets that band.
  - **EFFECTS:** the FX rack, slots 1-8 premium and 9-16 standard. Each slot shows its
    device, the strip using it (PRE/POST), ON and its mix. Tapping a slot opens the
    device on its **faceplate**, drawn like the hardware it recreates (a LARC remote
    for the vintage reverbs, a black 1176-style limiter with its VU meter...), with the
    device menu grouped PREMIUM / STANDARD / CHANNEL STRIPS / PLUGIN. The GATE, EQ and
    DYNAMICS screens show their model's faceplate too.
  - **METERS:** pre-fader meters of the channels in groups of eight, AUX, BUS 1-8 / 9-16,
    DCA, MAIN and MATRIX, with number keys in the strips' colours (select) and mute
    squares. A group's header opens its section's FADERS screen. Bigger consoles page.
  - **ROUTING** (status-bar keys CHANNELS / SOURCES / OUTPUTS): pick a channel, then its
    source. **MAIN / ALT** choose which of the channel's two inputs you patch (the ALT
    input, WING INPUT SELECT, can be a recorder playback channel: source groups
    PLAY 1-48 / 49-64). **+1 AUTO** moves on to the next channel; the lock (red when
    locked, green when open) protects the patch. SOURCES sets each input's source:
    name (RENAME), colour and icon, GAIN, +48V (stored), POL, MUTE and MONO / STEREO /
    MID-SIDE pairs (a channel patched to a pair becomes stereo), its mute groups
    (TAGS), with its meter and the channels using it; channels get the source's
    processing first. With SOURCE SOLO on, **SRC SOLO** solos the selected source
    through CH39 or AUX7 (the solo follows the selection; CLR SOLO ends it). OUTPUTS
    works output-first: pick an output, then the signal feeding it (a strip's L / R, or
    the MONITOR group's PHONES 1L / 1R and SPEAKERS 2L / 2R).
  - **SETUP:** GENERAL (lighting, clock, console info and size, OPERATION, OPTIONS),
    AUDIO (clock rate, INPUT SELECT: the global MAIN / ALT switch and each channel's,
    preferences such as MAIN LINK, DCA MUTEGROUPS, MUTEGROUP/SIP OVERRIDE, GLOBAL INPUT
    SELECT OVERRIDE, SOURCE SOLO (CH39 / AUX7) and the TALKBACK channel, SOLO SYSTEM with SOLO MODE LIVE / STUDIO /
    SIP, PFL/AFL per strip type and the solo / select preferences, audio device,
    AUTOMIX), SURFACE (meter taps, SEL DBL CLICK, SOF and scribble options, CH
    AUTOSELECT, FADER SPEED, paging, user layer link, alternative SOF, section widths),
    REMOTE (MIDI REMOTE CONTROL and MIDI PORTS, WING REMOTE) and DAW (DAW CONTROL, DAW
    PORTS; see *MIDI and DAW remote control*), PLUGINS (scanning, delay compensation) and LOG (system health and the
    event log), with INITIALIZE CONSOLE and SHUTDOWN. The status bar has MONITORS (the
    toolbar with the talk keys, PHONES / SPEAKERS / PH+SPK, DIM, MUTE, MONO, LR SWAP;
    TALK A / B: switch mode, talkback channel, AUTO DIM, USE SEND LEVELS and the bus,
    matrix and main destinations; PHONES and SPEAKERS, each its own: SOURCE, SRC LEVEL,
    SRC MIX, DIM, PFL DIM, BAND SOLO, DIRECT IN (any channel, bus, main or matrix, at
    its own level), the EQ (MONITORS EQ: 8 bands, PAN, WIDTH, and an RTA of the path,
    PRE or POST its EQ as EQ/RTA SETTINGS' RTA SOURCE says), the DIST slider with
    INVERT and DELAY, LIMITER with its gain reduction on the meter, and the level) and
    CC EDIT (also SETUP pressed twice): the custom
    control layers, a function list and the target lists, COLOR, CLR ALL and NAME,
    saved with the show.
  - **LIBRARY:** a file browser with the locations INT (`Documents/Sidecar/Library`),
    USB (a folder of your choice) and SHOW (the show's scenes and snippets: tags, SKIP,
    LINK, REMOVE, RENAME, UP / DOWN), with FOLDER, RENAME, DELETE and sorting. Tabs:
    SHOW (open and save shows, also from the list; **SEND MIDI**: MIDI data the
    selected scene sends each time it is recalled, see *MIDI and DAW remote control*),
    SNAP (SAVE, UPDATE, PARTIAL UPDATE,
    LOAD, PARTIAL LOAD with NO GLOB, RESTORE, EDIT SCOPE, IMPORT / EXPORT scene files),
    SNIP (REC / STOP with the counter of changed parameters, SAVE, UPDATE, PARTIAL LOAD,
    REC FOCUS, ADD / REMOVE ITEMS, CLEAR ALL, IMPORT / EXPORT snippet files), CLIP (the
    clip player, see [Scene and snippet files, clips](#scene-and-snippet-files-clips)),
    FX and CHAN presets, GO PREV / GO NEXT /
    PREV / NEXT / GO, the SCOPE window (strips, DCAs, mute groups, FX slots, the console
    settings and contents) and the GLOBAL safes in the status bar.
  - **VIEW screens** (a fader section's VIEW key): OVERVIEW (every strip's blocks;
    EXCL SOLO in the status bar), FADERS (send faders of a layer, and Sends on
    Fader, show and set the send level, MUTE switching the send), SENDS (sends on faders to one bus, main or matrix), CONFIG (edit any layer
    key by dragging channels, buses, DCAs, send levels, bus-to-bus sends, FX
    parameters or MIDI CCs onto or between slots, with a page map; a **MIDI CC** fader
    sends its CC, 0-127, out of EXTERNAL MIDI CONTROL and follows the CC coming in) and
    MUTE GROUPS (members, names,
    group keys).
  - **UTILITY:** system health and the event log (also on SETUP > LOG).
  - **RECORDER** (the status bar's **LIVE** card icon, red while recording): laid out
    like the WING-LIVE screens. See *Multitrack recording* below.
- **Channel strip section** (top left): the CHANNEL STRIP DETAILS LED panel (sources,
  filters, LEVEL and GR meters, bus sends, groups) around a small screen; CHANNEL
  PROCESSING with an encoder over each of INPUT, FILTER, GATE, COMP, INSERT and MAIN
  (press: show it on the screen, again: next parameter; hold: on/off); CHANNEL
  EQUALIZER with SHIFT, four band encoders, BLEND/MIX, EQ and GAIN / WIDTH / FREQ;
  VIEW opens the shown slot on HOME.
- **USB / monitoring panel** (top right): VIEW (the MONITORS screen; held: the USB
  recorder / player), TALK LEVEL,
  PHONES LEVEL, MONITOR LEVEL, **TALK A / TALK B** (AUTO: held talks while held, a
  short press latches; PUSH; LATCH), **DIM** (hold: mute the monitor) and **MONO**
  (hold: swap left and right) on the selected monitoring path; held functions blink,
  a press ends them. DCAs named MON A, MON B or MON LVL set the phones, speakers or
  both levels.
- **4-Channel section** (under the monitoring panel): GAIN, GATE, COMP, FILTER, F1-F3
  and PAN keys give the four strips of the right fader section an encoder and a key
  each (trim; gate / comp threshold and on; low / high cut; bus sends; pan / width and
  MAIN 1), with their scribble strips on a four-way LCD.
- **Custom controls** (between the center and right sections): the upper part's 16
  layers of four encoders and eight keys (assigned on CC EDIT; UP / DOWN; names and
  colours on the LCD), the lower layers **USER** (eight assignable keys), **2-TRACK
  TRANSP** (the USB recorder / player: previous / next clip, NEW FILE, REPEAT, PAUSE,
  STOP, PLAY, REC; VIEW opens its screen), **MULTI
  TRANSP** (the recorder's transport: previous / next marker, MARK, LOOP over START,
  STOP, PLAY, REC; VIEW opens the recorder screen), **MUTE GROUPS** (the eight mute
  groups) and **SHOW CNTRL** (scene back, GO, SCENES; the wheel picks the scene GO
  recalls); the **DAW** key switches DAW remote control on and off (held, or with DAW
  control off: the multitrack editor). With USE CRSR/WHEEL the arrows and the wheel
  edit the display.

## Multitrack recording

Sidecar records up to 64 tracks and plays up to 64 tracks back into the console, like
a WING with a WING-LIVE card, and edits them in a DAW-style editor.

- **Projects (sessions).** A project is a folder, `Documents/Sidecar/Recordings/<name>/`,
  holding `<name>.ldproj` and the `Audio` folder. Recording into an unsaved project
  creates a dated one. Projects autosave every 30 s; unsaved changes are offered
  back when the project is opened next. The last project reopens at start-up.
- **Recorder screen** (status-bar **LIVE** icon, or MULTI TRANSP > VIEW): the PLAYER
  (armed channels, rate, meters of the armed tracks, the red time, transport with the
  marker keys, free disk and record time left, timeline), the SESSIONS (click to open;
  NAME renames, DELETE moves a session to the trash), the MARKERS (click to jump;
  UPDATE moves the selected one to the position), the tracks (click to arm), and:
  - **RECORD ALL CHANNELS**: a track per input channel, named and coloured like it,
    recording its input port, armed, each on its own playback channels;
  - **VIRTUAL SOUNDCHECK**: each channel's ALT input becomes its track's playback
    channel and INPUT SELECT switches to GLOBAL ALT, so the band's recording plays
    through the console; press again to go back to the live inputs;
  - **EDIT** (status bar, header) opens the editor. The knobs move the position and
    the loop.
  - **Record format** (the header's format key, or FILE > Record format in the
    editor): WAV 32-bit float (the default), WAV / AIFF / FLAC 24-bit, or MP3 320 /
    256 / 192 / 128 kbit/s (MP3 needs 32, 44.1 or 48 kHz). Takes get the format's
    extension (`Audio/CH_1.flac`) and play back like any clip.
- **Editor** (Ctrl+E, EDIT, holding the DAW key; Esc or CONSOLE goes back): the top bar
  (FILE menu, transport, position in min:sec or bars|beats, tempo and time signature
  of the grid, snap OFF / GRID / BARS / BEATS / SECONDS, zoom, follow, undo / redo,
  disk load, dropouts and record time left), the track headers (colour, name, R / M /
  S, record source: hardware input, channel / aux pre or post fader, bus / main /
  matrix post; playback channel, gain, record meter, height; drag the name to reorder,
  right-click for more), the ruler (click to locate, drag to set the loop range,
  markers below it) and the clips.
  - **Clips:** click / shift / ctrl-click / rubber band to select; drag to move (also
    to another track; ctrl-drag copies); drag the edges to trim; drag the handles at
    the top corners for fades (right-click: linear, equal power, S-curve); drag the
    gain line or alt + wheel for clip gain. Moves snap to the grid, clip edges,
    markers and the playhead (hold shift to place freely). Overlapping clips are
    summed, and overlaps get crossfades automatically (FILE menu option).
  - **Import:** FILE > Import or drop files from the desktop (WAV, AIFF, FLAC, MP3...):
    converted to WAV at the project's rate in its Audio folder, placed at the playhead
    (or where dropped), on the selected track and the ones below.
  - **Export:** FILE > Export tracks renders every track (clip gains, fades, track
    gain) from the start of the timeline into the project's Export folder, as WAV
    (32-bit float, 24, 16), AIFF (24, 16), FLAC (24, 16) or MP3 (320 - 128 kbit/s).
  - **Keys:** space play / stop, R record (again: stop), Home / End, L loop, M marker,
    S split at the playhead (shift+S at the mouse), Delete, Ctrl+Z / Ctrl+Shift+Z,
    Ctrl+D duplicate, Ctrl+X / C / V (paste at the playhead), Ctrl+A, Ctrl+M mute clips,
    arrows nudge (alt: fine; without a selection they move the playhead), Ctrl+arrows
    previous / next marker, up / down select the track, F zoom to fit, + / - zoom,
    Ctrl+wheel zoom, shift+wheel scroll, Ctrl+S / Ctrl+Shift+S / Ctrl+O / Ctrl+I.

## USB recorder / player

The WING's 2/4-track USB recorder and player (manual 8.2), with a folder on the
computer as the "USB stick": `Documents/Sidecar/USB` by default (hold or right-click
the USB key on the screen to choose another folder).

- **Screen** (the status bar's **USB** icon, red while recording, or hold VIEW on the
  monitoring panel), laid out as the console's: the browser on the left (USB | N SONGS
  | ADD | FOLDER | RENAME | DELETE | REC PATH; folders, WAV / FLAC / AIFF / MP3 files and playlists; the
  folder row goes up a level, `>` opens the selected folder, a double click plays),
  the **4-TRACK RECORDER**, the playlist keys and the **4-TRACK PLAYER** with its
  timeline on the right.
- **Recorder:** records the RECORDER output group (four ports; MAIN 1 L/R on 1-2 by
  default; ROUTING > OUTPUTS > group RECORDER taps any strip's output without moving
  its own outputs) to a 2-CH or 4-CH, 16 or 24 bit file `R_YYYYMMDD-HHMMSS.<ext>` in
  the REC PATH folder, at the console's rate. **FORMAT** (a list next to 2-CH | 4-CH)
  picks WAV, FLAC, AIFF or MP3; for MP3 a **BIT RATE** list (128 - 320 kbit/s)
  replaces 16 | 24 bit and the recording has two channels. REC, PAUSE (the file stays
  open), STOP (REC then starts a new file) and NEW FILE (the next file starts on the
  next sample). SETUP > GENERAL > USB HOST SPEED FS limits it to 2 channels at 16 bit,
  as on the console. The format is kept in the app settings.
- **Player:** plays 1- to 4-channel WAV (16/24/32 bit), FLAC, AIFF and MP3 files (a file at another sample
  rate is resampled and its rate chip turns orange) into the **USB PLAYER** source
  group, which channels patch on ROUTING like any input (PLAYER 1-4, with source
  settings). PLAY, PAUSE, STOP, `<` `>`, and the timeline (drag the cursor while
  paused). The files of the browsed folder play one at a time.
- **Playlists:** CREATE PLAYLIST (or ADD a clip, ADD ALL for a folder), then on the
  N SONGS page REPEAT, REMOVE, SKIP (passed over), STOP (stop after the clip; held: all
  clips), UP / DOWN; clips follow each other without a gap. SAVE / OPEN / CLOSE
  PLAYLIST store M3U files in the USB folder.
- **Custom controls:** the USB PLAY/REC functions (PLAY, PAUSE, STOP, PREV, NEXT,
  REC, REC PAUSE, REC STOP, NEW FILE, REPEAT) on any CC key, and **CC BANK 1-16**
  (WING 3.0.6): a CC key that switches the custom controls to a bank.
- **FADER SPILL** on any CC key: spills a DCA, the sources sending to a bus, an FX
  slot's parameters, automix group X or Y, or the selected channel's sends onto the
  left fader section (lit while it shows; pressed again, the section's layer comes
  back). SD RECORDER and GPIO need the console's hardware and stay disabled.

## Shows, scenes and snippets

- A **show** is the file. It holds the console (channels, buses, FX devices and plugins,
  patching, DCAs, mute groups, fader banks) and all of its scenes.
- A **scene** is a console snapshot with a **recall scope**: the sections (Name,
  Routing, Input, Gate, EQ, Dynamics, Inserts, Sends, Fader, DCA/Mute), which strips,
  DCAs, mute groups and FX slots, and the console settings (monitor setup, INPUT
  SELECT, automix enables); an optional **fade time** for faders, pans and send levels,
  and notes. **Recall-safe** strips and the **GLOBAL safes** (DCAs, mute groups, FX
  slots, sections, console settings) are skipped. **PARTIAL LOAD** recalls a chosen
  scope (**NO GLOB** ignores the safes), **RESTORE** all of the scene, **PARTIAL
  UPDATE** stores a chosen scope into it; both partial keys keep their last scope. Use
  **GO >** and **<** (or `Ctrl+G`) to step through scenes during a show.
- A **snippet** belongs to a scene and recalls only the strips and sections you choose
  (and DCAs, mute groups, FX slots). **REC**, change parameters, **STOP** and **SAVE**:
  the snippet holds exactly the parameters you changed (within REC FOCUS). Fire it by
  hand, or mark it to **fire with its scene**.
- **Presets** are files: an FX slot's device and settings (`.lfx`, loads into any slot)
  and a strip's settings with a scope (`.lchn`, loads onto another strip of the same
  family; UPDATE SCOPE changes what it loads).
- Recalling a scene or snippet never creates or destroys channels or plugins, so it
  can't cause a dropout. Built-in FX devices and their settings are recalled.
- Shows from earlier Sidecar versions are converted when they load: channels stay
  channels, aux and group buses become buses, the master becomes Main 1, the gate,
  4-band EQ and compressor become the GATE, WING EQ and COMP models, and each strip's
  plugins move to FX slots on its inserts.

### Scene and snippet files, clips

On the WING, LIBRARY saves single scenes and snippets to a USB stick and loads them
into another show. In Sidecar the "stick" is the INT folder (`Documents/Sidecar/Library`)
or the USB folder of the file list.

- **EXPORT** on the SNAP tab saves the selected scene, with its snippets, as a scene
  file (`.lsnap`) in the folder shown at INT / USB (at SHOW: the INT folder); on the
  SNIP tab it saves the selected snippet as a snippet file (`.lsnip`). You are asked
  for the file name.
- **IMPORT** on the SNAP tab adds the scene file selected at INT / USB (or chosen in a
  file dialog) after the selected scene; on the SNIP tab it adds the snippet file to
  the selected scene. Double-clicking a file in the list imports it too. Importing
  never changes the console: load the scene or snippet when you want it.
- The files are Sidecar's own XML, like show files and presets. They are not the WING's
  `.snap` / `.snip` files; a WING snapshot is imported with IMPORT WING on the SHOW tab.
  A scene refers to strips by number, so strips the console doesn't have are skipped
  when it is recalled.
- **CLIP** plays an audio clip (WAV, FLAC, AIFF or MP3) selected in the file list at
  INT / USB: PLAY (or double-click the clip), PAUSE (pause / go on) and STOP, with the
  player's meter, time, time left and format chips. The clip plays on the USB player,
  so it comes in on the **USB PLAYER** sources: patch PLAYER 1-4 to channels on
  ROUTING to hear it. The USB RECORDER / PLAYER screen shows and controls the same
  playback (its browser moves to the clip's folder). If a playlist with unsaved
  changes is open, you are asked before the clip replaces it.

## MIDI and DAW remote control

The WING's MIDI remote control and DAW remote control (WING MIDI and DAW
documentation, firmware 1.07+), on the computer's MIDI devices.

- **Ports.** The WING's DIN socket and its USB MIDI ports become MIDI devices of the
  computer: SETUP > REMOTE > MIDI PORTS picks an input and an output for DIN and for
  USB (the WING's "MIDI Control" port), SETUP > DAW > DAW PORTS for MIDI DAW 1-3. Where
  the system allows it (Linux, macOS) a port can be the app's own **virtual port**
  ("Sidecar MIDI Control", "Sidecar MIDI DAW 1"...), which other apps then see like a
  connected WING; on Windows use a loopback driver (e.g. loopMIDI). Devices that are
  unplugged come back on their own.
- **MIDI REMOTE CONTROL** (SETUP > REMOTE; each row OFF / DIN / USB, touch its name for
  its MIDI map): **CH CONTROL** faders on MIDI channel 1, mutes on 2, pans on 3 (CC
  12-31 CH 1-20, 44-63 CH 21-40, 70-77 AUX, 78-93 BUS, 94/95 and 102/103 MAIN, 104-111
  MTX), DCA faders on 4, DCA mutes and mute groups on 5, CC 9 requests all values;
  **FX PARAMETER CONTROL** on channels 9-16 (insert on, mix, model, parameters);
  **CUSTOM CONTROLS (RX ONLY)** on channel 6 (encoders of layers 1-16 by CC, keys by
  note); **SYSEX CONTROL** (`F0 00 20 32 57 <cmd> <data> F7`: identity, execute node
  strings like `/ch.1.fdr=-3,mute=1`, dump and describe nodes of the WING tree);
  **EXTERNAL MIDI CONTROL** (where the custom controls' MIDI CC encoders and MIDI CC /
  NOTE / PGM keys, the MIDI CC faders of the layers and the scenes' SEND MIDI send); **SCENE CHANGE** (channel 7: program change, bank for scenes
  past 128); **SHOW CONTROL** (channel 8: scene tags; channel 9: GO, PREV, NEXT, GO
  PREV, GO NEXT); **SCENE MIDI TX** (recalls go out as channel 7 program changes).
  Console changes made anywhere go back out for CH CONTROL and FX PARAMETER CONTROL.
- **DAW remote control** (SETUP > DAW): DAW ENABLE, CONNECTION (USB: up to three units
  on MIDI DAW 1-3; DIN: one), EMULATION **MCU** (Mackie Control, with extenders) or
  **HUI**, CONFIGURATION (CUSTOM CONTROLS ONLY, SINGLE MCU, MCU + EXTENDER, MCU + 2x
  EXTENDER), USE UPPER CC FOR DAW, DISABLE WHEEL DURING PLAY, and PRESET + LOAD (Cubase,
  Live, Logic X, Nuendo, Pro Tools, Reaper, Studio One) for the four DAW layers. Set up
  the DAW as for a WING: a Mackie Control (or HUI) on MIDI DAW 1 and extenders on MIDI
  DAW 2 and 3.
- **DAW mode** (the **DAW** key): the fader sections become the DAW's 24 channels
  (left 1-12, center 13-20, right 21-24) with the DAW's track names, SELECT / SOLO /
  MUTE, meters and motor faders; their layer keys become NAME/VALUE, TRACK, PAN, SEND,
  EQ, PLUG-IN (HUI: SEND A-E), the banks of eight (USER 1 / 2) and single channels
  (◄4 / 4►). The lower custom controls' layer keys become **DAW 1-4** (DAW BUTTONS:
  transport, automation, markers, ... assigned on CC EDIT, its VIEW key opens it); with
  USE UPPER CC FOR DAW the upper part is four channels' V-pots (push on the upper keys,
  REC on the lower ones, UP / DOWN by four, the DAW's two-digit display on the layer
  display). The wheel is the jog wheel, the arrows the DAW's cursor keys.
- **Custom controls:** DAW BUTTONS (the WING's MCU / HUI command list), DAW V-POTS,
  MIDI CC encoders and MIDI CC / NOTE (toggle or push) / PGM CHANGE keys.
- **MIDI CC faders:** VIEW > CONFIG's MIDI CC field picks a MIDI channel and 32 of its
  CCs; drag them into any layer. The fader sends its CC (0-127) on EXTERNAL MIDI
  CONTROL's port; the scribble strip shows the CC, its value and the MIDI channel.
- **SEND MIDI** (LIBRARY > SHOW, for the selected scene): the EDIT MIDI TEXT STRING
  window takes hexadecimal MIDI bytes, with or without separators (`C002`,
  `B0,01,7F`, several messages in a row, SysEx as `F0 ... F7`); `U:` or `D:` before
  the bytes sends them (and what follows) to the USB or DIN port instead of EXTERNAL
  MIDI CONTROL's (that port has to be in use by a MIDI REMOTE CONTROL row). They go
  out each time the scene is recalled (GO, GO NEXT / PREV, by tag, by MIDI; a linked
  scene sends its own after the first), with EXTERNAL MIDI CONTROL on DIN or USB.
  Data that isn't MIDI is refused with the reason.

## WING remote apps and WING files

Sidecar can answer the Behringer WING's network protocols, so WING remote software
can find it and drive the console. This is clean-room work from public documentation
and open-source projects (see below), and it is **experimental**.

- **Turn it on:** SETUP > REMOTE > WING REMOTE > ENABLE. The console name (also
  SETUP > GENERAL > CONSOLE NAME), the ports and which servers run are saved in the
  app settings, and the servers start with the app when enabled. The panel below
  shows what is listening and which clients are connected.
- **Discovery** (UDP 2222) answers `WING?` with the console name, model `live-daw`,
  serial and firmware `3.1-...` (Sidecar speaks the WING 3.1 parameter tree).
- **OSC** (UDP 2223): get and set any parameter by path or by `#id`, toggles, node
  lists, node dumps and descriptions, node strings (`/ch.1.fdr=-3,mute=1`), and the
  `/*s`, `/*S` and `/*b` subscriptions (renew every 10 s). Changes from the UI, scene
  recalls and other clients are pushed to the subscriber.
- **Native protocol** (TCP 2222): node definitions and data, set/get by id, navigation,
  change notifications to every connected client, and meters over UDP.
- **What is tested:** libwing's own clients (`wingprop`, `wingmon`, `wingdrive`'s
  test battery and model capture, its OSC client), and every OSC path the Bitfocus
  Companion WING module builds that exists in the 3.1 tree. **WING Edit and WING
  Copilot have not been tried**, so they may not work.
- **What maps:** 87.4 % of the WING 3.1 tree's parameters (59,950 of 68,556,
  counting every model's parameters) drive the engine: channels, aux, buses, mains
  and matrices (name, colour, icon, input and ALT patch, trim, filters, gate, EQ and
  dynamics models with all their parameters, inserts, fader, mute, pan, width, sends,
  main sends, solo, TAP WIDTH and TAP EQ), DCAs, mute groups, the FX rack, output
  routing, the sources of the AES50 A/B/C and WING-LIVE card groups (name, colour,
  icon, gain, mute, polarity, +48V, mode, mute-group tags), SOLO MODE, the solo bus,
  MUTEGROUP/SIP OVERRIDE and SOURCE SOLO, talkback, both monitor paths (`/cfg/mon`:
  source, levels, DIM, PFL DIM, BAND SOLO, DIRECT IN, EQ, PAN, WIDTH, INVERT, DELAY,
  LIMITER), the RTA settings and other console settings. Clients can also use
  LISTEN (BAND SOLO, KEY SOLO and XOVER SOLO: `eq/$solo`, `$solobd`, `gatesc/$solo`,
  `dynsc/$solo`, `dynxo/$solo`), see the inserts' state (`$stat`) and which strip
  uses each FX slot (`/fx/N/$a_chn`, `$a_pos`), set a channel's source GAIN and
  +48V from the channel (`in/set/$g`, `$vph`), and recall scenes (`/$ctl/lib`:
  `$actionidx` with GO, GO NEXT, GO PREV, GO TAG; PREV / NEXT move the selection;
  `$active`, `$activeid` (the scene's tag) and `$actshow` show the current scene and
  show). The fader sections' layers (`/$ctl/layer/L`, `C`, `R`) and the custom
  controls (`/$ctl/user`) are the show's, both ways: clients select layer keys
  (`sel`), spill (`spidx`: DCA, FX, a bus's sources, automix X / Y), scroll the
  shown layer (`ofs`), name the USER keys' user layers and assign faders (`type`,
  `i`, `dst`: channels, buses, DCAs, sends, FX parameters, MIDI CCs; a default key
  becomes an edited key as on VIEW > CONFIG), and set each custom control's
  functions (`mode`), name and colour and the CC layer (`sel`); the console's
  changes show to clients within a tenth of a second. Sidecar's layers have no
  gaps (a fader assigned past the end is appended, OFF removes one), only the shown
  layer has a scroll position, and a new function starts at its first target
  (clients can't choose it: the WING's tree only has the function). The encoding of
  fader assignments and the custom controls of `U1`-`U3` (taken as the 4-Channel
  section's F1-F3 layers) are educated guesses. The rest (the other WING models'
  layers, the 4-Channel and button section modes, cards, ...) is remembered so
  clients can read back what they wrote, but does nothing.
  `cargo run -p livedaw-wing --example wing_coverage` prints the full report.
- **Files:** LIBRARY > SHOW > **IMPORT WING** applies a WING snapshot (`.snap`) to
  the console, or adds a WING show's (`.show`) snapshots as scenes. The log lists
  what could not be applied. **EXPORT WING** writes the console as a
  WING snapshot (experimental: its format follows snapshots saved by WING consoles on
  firmware 2.1 and 3.1, and it reads back into Sidecar, but it has not been loaded
  into a WING or WING Edit).
- Sources: Patrick-Gilles Maillot's *WING Remote Protocols* document, libwing (its
  parameter map is compiled into the app), the Companion WING module, and snapshot and
  show files that WING users have published.

Testing against libwing: `tools/wing/libwing_tools.sh DIR` builds its clients, then
`LIBWING_TOOLS=DIR/tools/target/release cargo test -p livedaw-wing --test libwing`.
`cargo run -p livedaw-wing --example wing_emulator` runs the protocols without the app.

## Drivers and performance

- **Audio drivers:** ASIO and WASAPI on Windows, CoreAudio on macOS, ALSA and JACK on Linux.
- Strips are processed in parallel on a pool of realtime worker threads. A full
  40-channel console with gates, EQs, dynamics and 16 bus sends per channel takes about
  0.45 ms of a 2.7 ms block (48 kHz, 128 samples) in the test suite.
- **Unused channels are free:** a channel with no input patched skips its DSP, so a big
  console only costs CPU for the channels you use.

## Stability

Live sound gets no second take, so stability is designed in at every layer:

| Layer | Protection |
|---|---|
| Audio thread | No allocations, locks or file I/O. Denormals are flushed. Channel/bus/plugin changes are lock-free swaps with deferred deletion. |
| Plugins | A plugin that outputs NaN/Inf or throws is **auto-bypassed on the spot**. Its input passes through, and the FX slot is flagged. Two inserts on one slot never run it concurrently. Per-slot CPU is measured. |
| Plugin scanning | Scans run **in a separate process**, so a plugin that crashes during a scan only kills the scanner. |
| Plugin loading | The plugin being loaded is recorded on disk first. If it crashes the app, it's blacklisted on the next start. |
| Strips | Every strip checks its own output. NaN/Inf is silenced and the DSP reset before it can poison a bus. |
| Outputs | A brick-wall **safety clip** (adjustable ceiling) protects the amplifiers and speakers. |
| Device | A **watchdog** reopens the audio device if it errors, disappears or stops calling back. Retries back off. |
| Crashes | The show is autosaved every 30 s. After an unclean exit, the app restarts in **safe mode** (console restored without plugins) and offers to reload the plugins. |
| Files | Show files are written atomically (temp file, verify, rename) with a `.bak` of the previous version. A corrupt file falls back to the backup. |
| Shutdown | SIGTERM/SIGINT quit cleanly (state saved), so OS shutdowns aren't mistaken for crashes. |
| Testing | Unit, integration (real LV2 plugins) and multi-threaded stress tests run under AddressSanitizer, UndefinedBehaviorSanitizer and ThreadSanitizer in CI. |

The **UTILITY** screen shows device health, xruns, DSP load, fault counters and an event log.
