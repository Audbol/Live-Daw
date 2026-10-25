# Changelog

All notable changes to Sidecar are listed here. Versions follow
[semantic versioning](https://semver.org); 0.x releases are betas.

## [Unreleased]

### Added

- Every FX device and processing model now has its own front panel, so none fall back
  to the generic knob grid. Before this, 49 of them did: 47 FX devices, the PSE/LA
  COMBO dynamics model and the PIA 560 bus EQ. Each new panel has the controls of
  every parameter, with the console's captions and value text, laid out in the
  colours, knobs and print of the unit it recreates. Units with many controls (VSS3,
  COMBINATOR, SPEAKER MANAGER, TRIPLE DEQ, the channel strips) have pages.
- WING REMOTE: the parameter tree is finished. Every parameter of the WING 3.1 tree
  but one now answers (68,555 of 68,556, up from 60,555, 88.3 %):
  - Newly driving Sidecar: the LOCAL IN sources (the same ports as AES50 A, as on
    ROUTING) and the USB player's (the `PLAY` group: name, colour, icon, mute,
    polarity, mode; channels patch to `PLAY` 1-4), LOCAL OUT (A 1-8), the USB
    recorder's RECORDER ports (`/io/out/REC/1-4`: a bus, main or matrix signal or a
    source), the buses, mains and matrices as sources (`/io/in/$BUS`, `$MAIN`, `$MTX`,
    read only) and `/$globals/clkrate` (the audio device's rate).
  - SETUP's options are Sidecar's settings, both ways (C ABI: `ld_wing_host_setting_keys`,
    `ld_wing_set_host_setting`, `ld_wing_take_host_setting`, `ld_wing_clear_host_settings`;
    `WingRemote::setHostSettings`): METERING's taps (`/cfg/mtr/mtrpage`, `mtrsfc`) and
    MAIN METER, the GENERAL, SURFACE and SOLO SYSTEM options and brightness tiles
    (`/$ctl/cfg`, `/$ctl/$globals`), MIDI REMOTE CONTROL (`/$ctl/midi`) and DAW
    (`/$ctl/daw`). Rows Sidecar shows fixed (MAIN METER TAP POST, TAP TEMPO FLASH ON,
    no screen lock...) answer that and refuse other values.
  - What Sidecar has no equivalent for (cards, the console's hardware, network and
    status, other models' surfaces, input and output groups Sidecar doesn't have,
    bus-to-bus sends the engine doesn't have, source and output safes, the player's
    song tags...) answers what a factory-initialized WING answers
    (`livedaw-wing/data/wing_factory.tsv`, from a dump of a real console) and keeps
    what a client sets, read-only where the WING marks it. Unbound nodes and missing
    strips answer the factory value too, instead of a neutral one.
  - Left unmapped: `/$ctl/daw/config`, whose 3.1 choices (CC, MSTR) can't show
    Sidecar's MCU with extenders.
  - Defaults that deliberately stay Sidecar's (the WING's factory value in brackets):
    MAIN METER the monitor (MAIN.1), MAIN METER TAP POST (AUTO), RIGHT SECTION SOF on
    (L/C), EXCLUSIVE SOLO and SELECT FOLLOWS SOLO off (on), FADER/SCREEN LAYER LINK on
    (off), the RECORDER ports on Main 1 (OFF), the USB player's sources unnamed,
    purple, mono and without an icon ("2TR", 8, stereo, 608) and the local sources
    without an icon (1: a local input is A's source here).
  - A unit test compares the description of every parameter a factory-set WING
    described (`tests/data/wing_desc.tsv`, 29,871 parameters) with Sidecar's; a
    protocol test sets and reads the new mappings over OSC and the native protocol.

### Fixed

- WING REMOTE: the recall safes (`/$ctl/safes`) answer in the console's format, a "+"
  or a space per strip, DCA, mute group or FX slot (they were lists like `1,3-5`,
  which are still taken). A string set over OSC keeps its leading and trailing spaces.
  CH 3's input delay is described in meters like the other channels (the tree had
  caught it in ms). `/play/$actidx` reads 0 with nothing queued, inside its range.
- The panels have graphs where the unit has something to show:
  - EQ curves for the graphic EQs, PIA 560, SPEAKER MANAGER and the channel strips'
    EQ sections. These are computed by the DSP itself, using the new C ABI function
    `ld_model_response`. No structure changes.
  - Echoes for the delays.
  - Envelopes for the reverbs, including gated and reverse.
  - The LFO for FLANGER, PHASER and TREMOLO.
  - The filter of the MOOD FILTER.
  - Limiter and compressor curves.
  - Band levels for COMBINATOR and SUB MONSTER.
  - The dynamic EQs (DYNAMIC EQ, DUAL and TRIPLE) now show the range each band can
    move through and, for cuts, where the gain reduction has it now.
  - The BDX 902 DE-ESSER shows the band it works on and its live cut.
- A test checks that every model has a panel and that every parameter has a control
  on it that moves. With `SIDECAR_FACEPLATE_SNAPSHOT_DIR` set, it also saves every
  page as a picture. LiveDAWTests now takes one test's name as its argument
  (`LiveDAWTests Faceplates`).

### Fixed

- Rates below 10 Hz now show two decimals ("0.50 Hz"). LFO speeds used to read
  "0 Hz".
- BLUE PLATE's damping readout is labelled kHz. It read HZ.
- The infinity sign on the BDX 560 and PIA 2250 ratio scales now draws correctly.

- User layers can have blank slots, as on the WING. On VIEW > CONFIG an item dragged
  (or tapped) onto a slot now goes into exactly that slot instead of being packed to
  the left with everything after it shifting; a filled slot dragged onto another swaps
  with it (or moves into a blank one); dragging a slot off the slots, right-click or
  long-press > CLEAR SLOT blanks it. A blank slot is an inactive strip on the surface
  (dark scribble strip, light bar off, the fader and keys doing nothing), also on the
  VIEW screens, the custom controls, Sends on Fader, spill and the remote controls.
  Blank slots are saved with the show (as "-"); older shows load unchanged. The WING
  remote maps a blank slot to OFF both ways (OFF used to remove the fader and shift
  the rest). C ABI 14: `kFaderEmpty` and the user layer slot functions. CONFIG's NEW
  and DELETE keys no longer cover the first slots of a 12-fader section.

- WING REMOTE: dropouts with Mixing Station and WING Edit, found with simulated
  clients run for minutes (a soak test, `livedaw-wing/tests/soak.rs`, now runs a
  short version in CI):
  - A Native client (WING Edit) that read slowly, on busy Wi-Fi or while syncing the
    tree, held up everything: every change pushed to it waited for its connection,
    so OSC replies (Mixing Station) stopped for seconds, meters stopped for every
    client and, after 2 s, the slow client was disconnected (and reconnected, and
    synced again...). Each Native client now has its own send queue and writer; a
    client is dropped only when its connection takes nothing for 10 s.
  - Two OSC remotes (Mixing Station on two tablets, or with Companion) took the
    event subscription from each other at every renewal, so each stopped seeing
    changes for up to 10 s at a time. Every OSC client that subscribes is now served,
    and one that subscribed with a reply port (`/%<port>/*S`) no longer gets its own
    changes echoed back.
  - A meter renewal sent as the protocol document shows it (`df d3 d4 <id>`, nothing
    after it) was applied only at the client's next channel switch: meters froze for
    about a second after the first 5 s, and again whenever the client was idle.
  - After a scene recall, the hundreds of OSC events left in one burst and overflowed
    the client's receive buffer (a quarter of them were lost even on the same
    computer): they are now paced at 8 per millisecond.
  - Subscriptions last 20% longer than the console's 10 s (OSC) and 5 s (meters), so
    a renewal delayed by the network doesn't open a gap.
  - Windows: a client that went away (its port unreachable) paused the OSC and
    discovery servers on each error report, delaying the other clients' replies.
- 1366 x 768 screens: the window opened at 1680 x 1000, off the top and left of a
  laptop screen with the start-up notice's buttons out of reach. It now opens fitted
  and centred in the main display's work area (a size saved on a bigger screen is
  fitted too, title bar included), and the smallest window is 1280 x 640 (a 1280 x 720
  screen less taskbar and title bar; was 1344 x 680).
- Short windows keep the main display's screen legible (360 px tall, body text about
  9 px, as in a 1680 x 1000 window) instead of shrinking it to about 285 px: the
  channel strip and the monitoring / 4-Channel column narrow to make room, the fader
  sections get the rest and fold their strips' meter blocks away below 100 px of
  fader travel (the screen, the channel strip and the main meter still show levels),
  and the layer keys pack closer so all of them stay on the surface. Large windows
  are unchanged.
- A sandboxed plugin's editor window larger than the screen opens with its title bar
  on screen (and shrinks to the work area when the editor is resizable).
- Found by testing with real plugins (the LSP plugins, VST3 and LV2, now covered by
  a test that runs them when they are installed):
  - Linux: a show with a VST3 plugin loaded in-process didn't load it back ("No
    compatible plug-in format exists"): the show stored the library inside the
    plugin's bundle. Such shows now load (also with the sandbox on), and shows now
    store the description the plugin was loaded from (the bundle). Sidecar no
    longer asks a loaded plugin for its description (the EFFECTS screen did on every
    repaint), which made LSP's VST3 plugins leak a small host object each.
  - A mono plugin (no stereo layout, such as LSP's "Mono" versions) on a stereo
    channel silenced the right side. It now gets both sides mixed to mono and its
    output feeds both (in-process, in a chain and in the sandbox).
  - The plugin window's zoom ended at FIXED for VST3 editors on Linux, which take
    their real size only once shown: they now shrink by the host scale. An editor
    that goes back to its own size after being resized smaller (LSP's LV2 UIs) now
    tries the host scale, then shows FIXED, instead of claiming the zoom.
  - The plugin window grew off the screen when the plugin's editor took its real
    size after the window was placed (a 1366 x 768 screen); it now stays on screen.
  - Sandbox: a parameter set right after a plugin loaded (a scene recall, a
    remote) snapped back to its old value in Sidecar 300 ms later, though the
    plugin had it.
  - Sandbox: the helper no longer asks a plugin whether it has an editor while
    loading it (Sidecar didn't use the answer); LSP's VST3 plugins crashed there
    without a display.

## [0.1.4] - beta

The second round of matching Sidecar to a real WING, from screenshots, photos and a
parameter dump of a console: the MONITOR / TALKBACK, MUTE GROUPS and DCA screens (with
renaming, the DCA SOLO BUS and the SOURCE tab), METERS with its PRE / POST settings, the
USB recorder and player, the EFFECTS rack and the COMP page with its KEY FILTER and
XOVER pages now follow the console's own screens; the monitor, solo and RTA settings
start as a factory-initialized WING; the surface's keys, LEDs and light bars are lit
like the console's, with a black finish beside the silver one; and a WING snapshot
loads from LIBRARY as on the console. New: plugin chains, up to 8 plugins in one FX
slot, shown side by side in one window with zoom, collapse and per-plugin fault
bypass. The screens use less CPU. Known limitations: see the
[README](https://github.com/Audbol/Live-Daw/blob/main/README.md#known-limitations).

### Added

- Plugin chains: an EXT FX slot runs up to 8 plugins in series (VST3, AU, LV2, each one
  sandboxed or not), shown in one window: a chain bar (IN ▸ chips ▸ OUT, +; a chip
  shows the name, a bypass dot, a fault mark and the latency, scrolls to its plugin,
  drags to reorder and has a right-click menu) over the editors as cards with a slim
  title row (collapse, name, BYP, zoom − % +, remove). ROW shows the editors side by
  side, FOCUS one at a time with the chips as tabs; the choice and the window size are
  kept per slot. Each plugin can be zoomed to 50-100 % (resized, host-scaled for VST3 /
  VST / LV2, or transform-scaled for generic and built-in editors; fixed-size native
  editors say FIXED) or collapsed, and FIT fits the whole chain to the window's
  height; zoom and collapse are saved per plugin with the show. A plugin that throws,
  outputs NaN/Inf or fails in the sandbox bypasses only itself: its input passes on,
  the rest of the chain keeps running, its header shows FAULT with CLEAR, and the
  event log names it ("FX 3: Pro-Q 3 (plugin 2 of 3) output NaN/Inf; bypassed, the rest
  of the chain keeps running"); a chain of one faults the slot as a single plugin does.
  The FX slot menu has Add to chain, and the EFFECTS tile names the chain ("A + B",
  "A +2"). A plugin's latency counts unless it is bypassed or faulted. Changing the
  chain keeps the other plugins running with their state. Shows, scenes, snippets and
  FX presets store and recall the whole chain; a single plugin is saved as before.
- `tools/wing/audio_compare` compares the sound of a WING's processing with Sidecar's:
  test signals for the WING's USB player (sweeps, level steps, level jumps, tone steps,
  noise, clicks, a synthetic band) with sync chirps and an ID code, a test plan of 65
  channel settings in order of importance (WING EQ, filters, COMP and GATE first, then
  the other EQ and dynamics models, key filter, crossover and FX), an optional OSC
  helper that sets the WING up and reads the values back, and an analysis that aligns
  the recordings (latency, clock drift, level) and reports the differences with
  tolerances: magnitude and phase, harmonics, static curves, attack / hold / release,
  reverb decay, delay echoes, modulation. Sidecar's side is rendered offline by the
  engine with the new `render` example of livedaw-wing (WING parameter paths in, WAV
  out).
- SETUP > GENERAL > CONSOLE > SURFACE FINISH: the surface view as the silver WING
  (default) or the black WING BK (black surface, printed frames, frosted light keys,
  silver fader caps). It is kept in the app settings, not the show, and switches the
  surface at once; the touchscreen is the same in both.
- Web remote: SETUP > REMOTE > WEB REMOTE serves Sidecar's screen to browsers on the
  local network (`http://<this computer>:8080/`, port configurable). The page mirrors
  the window and sends the mouse, touch, wheel and keys back; optional 4-8 digit PIN
  (checked by Sidecar, wrong guesses rate-limited) and VIEW ONLY. Off by default.

### Changed

- Lower CPU use with the VIEW screens (OVERVIEW, FADERS, SENDS, CONFIG, MUTE GROUPS),
  METERS and CHANNEL SELECT open: they redraw only when something they show changes,
  instead of the whole screen 15-25 times a second.
- The monitors start with a WING's factory settings, read from a console's parameter dump
  after INITIALIZE: SOLO MODE LIVE, the solo bus PH (phones), mains and matrices solo PFL,
  PFL DIM 12 dB and BAND SOLO 0 dB on both paths, the monitor EQs' bands at 60.1, 129.9,
  299.2, 699.5 Hz, 1.50, 2.99 and 6.01 kHz with Q 2.0, talkback AUTO DIM (BUS DIM, MON
  DIM) 0 dB, and the RTAs at 30 dB range, MED decay and PEAK detector. Saved shows keep
  their settings.
- DCAs have a SOLO BUS (`/dca/n/mon`: A = phones, B = speakers, A+B), saved with the
  show: in SOLO MODE LIVE a soloed DCA sends its members' solo there (their own solo
  still goes to their own SOLO BUS).
- Renaming DCAs and mute groups as on the WING, instead of touching the selected tab of an
  assign screen again: a DCA's title tile on its assign screen (and its name on FADERS)
  opens the DCA's panel (LIGHT and the 18 colours in the console's order, the icon
  gallery, SOLO BUS, the name field and the keyboard); a mute group's header on MUTE
  GROUPS opens the MUTE GROUP n window. The SOURCE tab of a mute group's assign screen
  shows one source group at a time, eight sources to a row, with the source groups as
  tiles on the right, instead of 64 sources a page.
- The DCA and mute group columns of OVERVIEW and MUTE GROUPS fill a member's cell in the
  member's own colour, and a DCA's "DCA" label (OVERVIEW, FADERS) turns yellow while the
  DCA is soloed. The layer column's section key is darker olive while a bank is shown.
- METERS, the USB recorder / player, the EFFECTS rack and a channel's COMP page with
  its KEY FILTER and XOVER pages laid out and coloured like a WING's (firmware 3.1,
  measured off the console's screens). METERS: the WING's group and scale geometry,
  61-segment meters (yellow from -18 dB), M keys, DCAs in pairs, and a **SETTINGS**
  key that sets PRE or POST per strip type (channels and DCAs PRE, the others POST, as
  a factory-set console; the METERS screen used to show every meter pre-fader). USB:
  the PLAYLIST tab, ADD ALL (with nothing selected, the folder shown), folder rows in
  blue with their own `>` key, upright red counters without colons, a yellow square
  while stopped, keys framed while enabled, and a blank player with PLAY off until a
  clip is chosen. EFFECTS: no parameter bar, the slots where the console has them, a
  scratched-metal rack, and a device inserted nowhere shown dimmed (no ON or source
  column); reverb decays show two decimals ("1.50 s"). COMP: the transfer curve blue
  while on, the ENVELOPE grey for the compressor whether on or not, the dot grid and
  ticks as the console's; the KEY FILTER / XOVER pages have the plot where the console
  has it, an open FREQ ring, Q and DEPTH boxes, DEPTH, Q and FREQ on knobs 3-5, and the
  XOVER curve for PEAK shows the band taken down by DEPTH (a dip at FREQ). Q on the
  parameter bar is a green fill either side of the centre, wider for a lower Q; the
  X/O tag lights blue; the ticks right of the COMP and GATE transfer graphs are plain grey
  lines, not an output meter, and the yellow level dot trails a short fading tail;
  double-clicking an EQ band's handle on the graph, or its band key, with the mouse resets
  the band to the model's defaults (touches don't, so taps can't reset one by accident); lit status-bar keys are #707070; the HOME screens keep 8 px
  margins left and right.
- MONITOR / TALKBACK, MUTE GROUPS and the VIEW screens with a DCA layer (OVERVIEW,
  FADERS, SENDS, CONFIG) are laid out like a WING's (firmware 3.1, measured off the
  console's screens): the layer column with bank keys (1-8 ... DCA 9-16), DCA and mute
  group columns showing their members, assign screens for a DCA's or mute group's members
  (tiles framed gold, UNASSIGN, CLOSE; a mute group's SOURCE tab), the talk side dim
  while no talkback channel is set, and a linear DIST slider. The monitor parameter bar
  no longer has TALK LVL (the surface's TALK LEVEL knob sets it).
- SETUP starts with the WING's factory settings (checked on a console after INITIALIZE):
  USE CRSR/WHEEL, SHOW SOF FRAME, CH STRIP TOUCH SELECT and TAP TEMPO FLASH on, SEL DBL
  CLICK BUSFX, MAIN METER TAP POST. Settings you've changed are kept.
- The 18 strip colours have the WING's numbers, read from a console swatch by swatch (they
  had been guessed apart from colour 1): shows and WING snapshots now come up in the
  console's colours, and new buses are Salmon (1-8), Lime (9-10) and Magenta (11-16).
- The surface's SELECT, SOLO, MUTE and SOF FLIP keys light as on the console: a dark face in
  a backlit rim, pale lavender unlit (light grey on the silver finish), the rim and print
  green, orange or red lit while the face stays dark. The meter block's GATE LED is red
  (gate closed) and its DYN LED orange (dynamics working); the center section's USER,
  2-TRACK, MULTI, MUTE GROUPS and SHOW CNTRL keys are light keys, as on the console.
- The surface's keys and LCDs as in photos of a silver WING: lit layer, function and mute
  group keys and the SOF FLIP rim are light orange (brighter in the middle), lit VIEW keys
  bright green; the screen keys beside the display (HOME, EFFECTS...) are backlit keys, a
  black face in a lavender rim with lit print, rim and print green for the screen shown,
  and CLR SOLO the same with an orange rim that blinks while anything is soloed; the light bars light DARK BLUE (colour 1, the
  inputs' and mains' default) plain blue, as the console's LEDs do; in MUTE GROUPS the custom controls' lower LCD shows "MUTE
  GROUPS" over the eight group names in four columns, the active ones inverted, over
  blank keys; the 4-Channel section's LCD leaves the key row empty under PAN for a main or
  matrix (no MAIN 1 send); the CHANNEL STRIP DETAILS and GROUPS LEDs are green. On the black finish the keys are backlit lavender with
  dark navy print (a key with nothing to do shows its grey cap), as on a black WING.
- A WING snapshot loads the console's way: on LIBRARY > SNAP, select the `.snap` file at INT
  or USB and press LOAD (or double-click it). IMPORT WING on the SHOW tab still works.
- The center fader section has no INPUTS layer key, as on the console (DCA, MAIN MATRIX,
  AUX IN, BUS MASTER, USER 1, USER 2); shows that had it selected there start on DCA.
- The channel number in the HOME header is drawn in exactly the strip's colour, as on the
  console (dark colours were lightened); the factory colour is named Dark Blue, as on the WING.
- WING COMP and WING GATE time and bend as recordings of a WING do: ENVELOPE LIN moves the
  gain in straight lines of 10 dB per ATTACK / RELEASE time and LOG exponentially with
  them as time constants, the release gets going over twice the HOLD time, AUTO has its own
  program-dependent attack and release, the RMS detector is faster (5 ms) and KNEE bends
  over 7.2 dB per step; the GATE opens and closes on a linear gain scale (ATTACK and
  RELEASE are 10-90 % times), ratio GATE closes in a straight line over RELEASE and RANGE
  60 closes completely.
- The channel filter's HIGH CUT keeps its slope up to the top of the band, as on the console
  (a 5 kHz cut at 6 dB/oct is -13 dB at 20 kHz, not -21).
- The FILTER tool's TILT goes the console's way and as far: TILT +3 lifts the lows by 3 dB
  and lowers the highs by 3 dB on a gentle slope through about 700 Hz (Sidecar tilted the
  other way, by half as much).
- The WING EQ's bells are as wide as a WING's: the set Q is the resonance of the boosting
  (or, for a cut, the cutting) section, so the filter's Q is the set Q over 10^(|gain| / 40)
  (measured on a console: Q 1.00 at +6 dB is as wide as a Q 0.707 peaking filter). High
  bells keep their shape up to the top of the band, as on the console (a 12 kHz bell no
  longer drops 1.8 dB short at 16-20 kHz). The shelves' frequency is where the boost (or
  cut) section turns, as on the console, so a shelf's midpoint lies 10^(|gain| / 80) further
  out (a +6 dB low shelf at 80 Hz is half way at 95 Hz), and their slope is always Q 0.707;
  a shelf band has no Q knob, as on the console.
- Scribble strip LCDs have one backlight colour on every strip, as on the console; only
  the light bar above each strip shows the strip's colour.
- The +48V keys (HOME OVERVIEW and INPUT, ROUTING > SOURCES, and the 4-Channel
  section's keys in GAIN / 48 V mode) switch phantom power only when held for 2 s, as
  on the console; the key fills up while held, and a tap does nothing. Remote control,
  snapshots and scenes still switch it at once.
- A patched channel's scribble strip with neither name nor icon shows its source's glyph
  (the XLR connector for a local input), as on the console.
- The control surface looks like the silver WING, matched to photos of a real console:
  a light grey surface with mid grey decks and a black display unit; light grey keys
  and dark grey SELECT / SOLO / MUTE / SOF FLIP keys in a light bevel, with the console's
  smaller, wider print; black fader caps with a white line and dark printed scales;
  frosted grey unlit LEDs with the printed dashes beside the meter labels; paler
  periwinkle scribble LCDs under thicker light bars; larger channel strip keys and
  captions; blue digits on the custom controls' layer display; framed TALK A / B
  letters. The 4-Channel LCD has no light bars, as on the console.
- A mono channel's pan follows the WING's law, measured on a console: unity on both
  sides at centre, the near side rising to +3 dB and the far side falling to off at the
  edge (Sidecar used a -3 dB centre).

### Fixed

- A slot's plugin window closes when its plugins are removed right after its chain was
  changed from elsewhere (it could stay open for a moment and outlive the editors' plugins).
- An FX slot auto-bypassed by a failing plugin ran that plugin again as soon as its
  chain was edited: zooming or collapsing the plugin in the plugin window, bypassing it,
  or adding or moving a plugin cleared the slot's fault. The slot now stays bypassed
  until you clear the fault.

## [0.1.3] - beta

The first round of matching Sidecar to a real WING, side by side with the console:
HOME, the input page, INSERT and the FX menus, the EFFECTS rack, ROUTING, SETUP,
LIBRARY, CUSTOM CONTROLS, CHANNEL COPY / INIT, a full INITIALIZE CONSOLE page, and the
GATE, EQ, FILTER and COMP pages now follow the console's own screens; the scribble
strips are lit like the console's; a new console starts as a factory-initialized WING
(faders, EQ, names, colours, gate, compressor, filter and EQ settings read from a real
console); and the WING remote answers in the console's exact format. A tool to compare
Sidecar with a WING parameter by parameter is included. Known limitations: see the
[README](https://github.com/Audbol/Live-Daw/blob/main/README.md#known-limitations).

### Added

- `tools/wing/wing_compare.py` reads every parameter from a WING and from Sidecar
  over OSC (read-only) and reports what differs, for checking Sidecar against a real
  console.
- INITIALIZE CONSOLE is a page: choose the strips, contents, DCAs, mute groups, FX
  slots and source groups to reset (everything: a fresh 40-channel console, as before).

### Changed

- A new console starts as a factory-initialized WING (checked against a parameter
  dump of one): every fader at -oo except buses 9-16 (0 dB, into Main 1), every EQ
  off, strips and DCAs unnamed (mute groups MGRP.1 ...), DCA lights off, channels and
  aux with LINK CUSTOMIZATION TO SOURCE on (the strip shows its source's name, colour
  and icon; ICON/COLOR and NAME then edit the source), and the input delay in METERS
  at 0.1 m (the WING's range, 0.1 to 150 m, in every unit). Shows saved before keep
  their strips' own customization. Engine ABI version 12.
- The 18 strip colours are the full-size WING's (measured on its ICON/COLOR screen and
  laid out as there); colour 1, the factory colour of channels, aux, mains, matrices,
  sources and DCAs, is Purple; buses start Pink (1-8), Olive (9-10) and Violet (11-16).
- HOME matches the console's screens more closely: the source's GAIN and +48V on the
  OVERVIEW and GAIN in the parameter bar of all four HOME tabs (which all have
  SETTINGS), the redrawn general icons, the WING's TAGS tiles (MGRP1 ...), the status
  tile's A B C / SC chips (the DSP load stays on UTILITY), the LEVEL scale without
  -40, the olive SOLO BUS keys, NAME's keyboard starting in capitals with source-based
  suggestions, an empty ALT tile with a plus, and SELECT MAIN INPUT (touch the MAIN
  tile on INPUT: source groups, NONE, M / ST / M/S, COPY CUSTOMIZATION) in place of the
  ST key.
- Scribble strips are lit in the strip's colour with large two-row names; an unlit
  colour light bar is light grey. The MAIN LCD brightness default no longer dims the
  screen.
- WING remote: OSC replies and node descriptions are formatted as on a real WING
  (firmware 3.1): no "r/o", nodes as "(node)", ranges and values with the
  parameter's fixed decimals, "+" on signed ranges and kHz notation ("lin [-15.0 ..
  +15.0 dB]", "log [20 .. 20k0 Hz]", "1k0"), units on integer ranges, float choices
  as `list [...]`, running times as "0:00", and an integer reply's number counted
  from the bottom of its range. Floats set over OSC, the Native protocol or from a
  snapshot land on the parameter's nearest step, as on the console, and the 0..1
  positions are the steps'. The fader law below -60 dB follows the console (-90 dB
  is the bottom of the travel).
- The strip LED meters light -6 to -18 in orange, as on the console.
- SET CUST names the channel after the device's short name (DIMCRS) and gives it the
  FX icon; the INS tile shows the premium diamond for a device in FX 1-8; the EFFECTS
  rack has no FX MIX knobs.
- INS 1 / INS 2, the FX TYPE list and the EFFECTS rack look like the WING's 3.1
  screens: FX PROCESSOR opens the SELECT FX PROCESSOR window (the 16 slots in four
  columns, who uses each), FX TYPE a list of device tiles with the FX1-8 / STD / CH
  tabs in the console's order, and FX TYPE, RESET and FX SPILL only show once a slot
  is chosen. SET CUST is on every strip's INS. A few device names follow the console's
  screen (DELAY/REVERB, DIMENSION CRS, 2-BAND DEESSER, TREMOLO/PANNER).
- **ROUTING** is laid out like the console's screen (firmware 3.1): its proportions,
  colours and fonts, the CHANNEL INPUT MAIN | ALT keys, +1 AUTO and the lock, the
  tiles (patched channels show their source's icon and label, unpatched ones only
  their number), the SOURCES settings boxes and the OUTPUTS tiles with their meter
  scale. The group selectors open the console's icon menus with its groups: the
  interface's inputs and outputs are **LOCAL IN** / **LOCAL OUT** (LCL 1 ...; ports
  past 48 in AES50 B / C), the recorder's playback is **WLIVE PLAY**, the channels'
  own outputs are under **USER SIGNAL**; groups Sidecar doesn't have show n/a tiles.
  +1 AUTO is off by default, as on the console.
- SETUP, LIBRARY, CUSTOM CONTROLS and CHANNEL COPY / INIT are laid out like the
  console's own screens at 1280 x 800: SETUP's 64 px headers, 52 / 56 px rows and
  key sizes, the GENERAL lighting tiles (CH STRIP, GLOW), the option order and colours
  of every tab (a lit OFF is grey, PFL / PRE red, HS and USB green), SEND PAGE
  ACTIVATES SOLO (not there yet); LIBRARY's toolbar, file rows, tabs, key bands and
  its orange title tile; the USER key on CUSTOM CONTROLS; CHANNEL COPY / INIT as a
  window over the page.
- SURFACE's section widths, SHIFT STEP and FADER VALUES moved to FADER SECTIONS (the
  SECTIONS key in the FADERS header); REMOTE's MIDI ports sit under MIDI REMOTE CONTROL
  and show whether each device is open.
- GATE, COMP, EQ and FILTER SETUP look like a WING at factory settings: the panel
  rows and positions, KEY TAP only while a channel keys the slot, the console's
  names (FLAT / LOWPASS ..., WING GATE, LMT COMPRESSOR ...), DETECTOR / AUTO ENV
  keys in blue, the EQ header's LO CUT / HI CUT / TILT EQ keys and FILTER field,
  band 4 selected, the TOOL FILTER icons and "0.00 dB" for TILT.
- The KEY FILTER and XOVER MODE `>` keys open GATE KEY FILTER / COMP XOVER (the
  filter's response filled green or blue, its type, the solo key, FREQ on the knobs);
  KEY SOURCE opens SELECT CHANNEL (SELF and the channel tiles); the dynamics and EQ
  model lists are the console's picture grids.
- New channels start with a factory-set WING's values: GATE -40 dB, 1:3, RANGE
  40 dB, ATTACK 10 ms, HOLD 10 ms; COMP -10 dB, KNEE 3, ATTACK 50 ms, HOLD 20 ms,
  AUTO ENV on; aux channels' dynamics slot the WING GATE at -36 dB; key filters 1k0 Hz
  Q 2.00, crossover 1k0 Hz Q 0.40 DEPTH 6 dB; LC 100.2 Hz at 24 dB/oct, HC 10k02 Hz;
  the WING EQ's bands at 80.2, 200, 601.4 Hz, 1k50, 3k99, 12k00 (buses, mains and
  matrices 60.1, 129.9, 299.2, 699.5 Hz, 1k50, 2k99, 6k01, 12k00), every band at
  Q 1.00; the TAP EQ at Q 1.00. The values are the console's own (on its parameter
  steps), read from a factory-set WING. Saved shows keep their values.

## [0.1.2] - beta

A fix for a crash in 0.1.1 and earlier: scrolling the start-up notice could close
Sidecar. Everything else is as in 0.1.1; see its notes below. Known limitations: see
the [README](https://github.com/Audbol/Live-Daw/blob/main/README.md#known-limitations).

### Fixed

- Scrolling the start-up notice (or the ABOUT text) could crash Sidecar: a scroll the
  text couldn't use (at its end, or sideways on a trackpad) went back and forth
  between the window and its scroll view until the stack ran out.

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
