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
    Bus comp, NSTR, limiters, de-esser...), with sidechain filter and source
    (KEY SOURCE: the strip itself or any input channel, taken at the KEY TAP: IN,
    FILT, 3, 4, 5, PFL, AFL or POST), and a crossover band on the dynamics slot.
  - **EQ:** the 6-band WING EQ (8 bands on buses) or the analog models (SOUL, E88,
    E84, F110, PULSAR, MACH4, PIA on buses), with blend/mix.
  - **PRE INSERT and POST INSERT** use the 16 FX slots; the post insert can also join
    automix group X or Y (gain sharing; a channel is a member while its INS 2 is set
    to AUTO X / AUTO Y and ON, and its group's X / Y ENABLE is on).
  - Gate, EQ, dynamics and the pre insert run in any of the **24 process orders**, and
    bus sends tap at IN, FILT, 3, 4, 5, PFL, AFL or POST.
  - Fader, DCAs, mute groups, width and pan; sends to the 16 buses (PRE, POST or GRP
    mode, with pan), to the 4 mains and the 8 matrices. A mono channel pans like the
    WING: 0 dB on both sides at centre, the near side rising to +3 dB and the far side
    falling to off at the edge (a stereo channel's pan is a balance: unity at centre,
    the far side turned down).
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
  - **EXT:** a **VST3, AU (macOS) or LV2 (Linux) plugin**, or a **chain of up to 8
    plugins** in series (see *Plugin chains*).
- **Plugin chains:** an EXT slot can run several plugins one after the other, and
  shows them in one window (OPEN EDITOR, or tapping a plugin slot).
  - **Chain bar** across the top: **IN ▸ plugin ▸ plugin ▸ … ▸ OUT**, then **+**
    (adds a plugin from the plugin list; the FX slot menu has **Add to chain** too).
    Each chip shows the plugin's name, a dot (green: running, hollow: bypassed, red:
    faulted; click it to bypass) and its latency when it has one. Click a chip to go
    to that plugin; drag it to move the plugin in the chain; right-click it for
    Bypass, Clear fault, Move earlier / later, Zoom, Collapse and Remove.
  - **Cards:** below the bar each plugin's editor sits under a slim title row:
    collapse ▾, the name, **BYP**, zoom **−** / **%** / **+** and remove (✕). A
    collapsed plugin shrinks to a thin column (ROW) so the others move up close;
    click it to open it again. Bypassed and faulted plugins are dimmed.
  - **ROW / FOCUS** (right end of the bar): ROW shows the editors side by side,
    scrolling sideways; FOCUS shows one at a time, with the chips as its tabs. The
    choice and the window's size are kept per FX slot.
  - **Zoom:** 50, 67, 75, 85 or 100 % per plugin; **FIT** picks for every plugin the
    largest step at which the whole chain fits the window's height. Sidecar shrinks
    an editor the best way it allows: a resizable editor is resized; a VST3 / VST /
    LV2 editor is asked to draw at a smaller scale (kept only if it gets smaller);
    generic controls, the sandbox panel and other built-in editors are scaled down. A
    plugin whose own window can't be scaled (most fixed-size VST3/AU editors without
    scaling support) shows **FIXED** with its zoom keys greyed out: collapse it
    instead. For a sandboxed plugin the zoom applies to Sidecar's panel, not to the
    plugin's own window (SHOW PLUGIN WINDOW). Zoom and collapse are saved per plugin
    with the show.

  A plugin without an editor gets generic controls, and a sandboxed plugin shows SHOW
  PLUGIN WINDOW with its generic controls. The EFFECTS tile names the chain
  ("TAL-Reverb + Pro-Q 3", or the first plugin and "+2" for three or more). The slot's
  FX MIX, bypass and CPU meter cover the whole chain. **A plugin that fails bypasses
  only itself:** if a plugin of a chain of two or more throws, outputs NaN/Inf or (in
  the sandbox) misses its block or loses its helper, its input passes straight on to
  the next plugin and the rest of the chain keeps running. Its header and chip show
  FAULT in red, the EFFECTS tile shows "1 PLUGIN FAULTED", and the event log names it
  ("FX 3: Pro-Q 3 (plugin 2 of 3) output NaN/Inf; bypassed, the rest of the chain
  keeps running"). **CLEAR** in its header (or BYP off and on, the chip's menu, or
  clearing the slot's fault) runs it again; a sandboxed plugin's helper is restarted
  meanwhile as for a single plugin. A faulted plugin counts as bypassed for the
  latency, and faults aren't saved. A plugin on its own (or a chain of one) faults
  the whole slot as before. A plugin's BYP takes it out of the chain and out of the
  latency. Adding, removing and moving plugins swaps the new chain in without
  stopping the audio; the plugins that stay keep running with their state. Removing
  the last plugin empties the slot.
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
  and each loaded plugin's latency (a chain's is the sum of its plugins' that aren't
  bypassed or faulted). Compensation stops at 8192 samples (about 170
  ms at 48 kHz): a plugin reporting more is written to the event log and the panel
  shows the limit. Solo, the monitor paths, direct outs and the recorder's taps are
  not compensated, and a bypassed plugin adds no latency (the others are realigned
  within a tenth of a second).
- Processing models are recreated from the WING's parameter set (controls, ranges and
  behaviour), not circuit-modelled. The model tables are generated from the console's
  published parameter map.

## The surface

The window is a WING control surface and resizes to any screen, including ultrawides.
The smallest window, 1280 x 640, still holds every section of the surface, so a
1366 x 768 screen (the smallest supported) shows the whole console, maximized or not;
fader sections set wider than 12 + 8 + 4 faders (SETUP > SURFACE) need a wider window
for the whole custom controls and 4-Channel sections. The window first opens fitted
to the screen. In short windows the main display keeps a legible screen (as in a
1680 x 1000 window): the channel strip and the monitoring column narrow, and below
about 100 px of fader travel the strips fold their meter blocks away. For the
largest screen, show the display on its own (F10).
The screens follow the WING's own: the same panels, keys, colours and layout, set in
Barlow Condensed. The surface around them looks like the silver WING: a light grey
surface with mid grey decks (monitoring, channel strip, 4-Channel, custom controls),
light grey keys and dark grey SELECT / SOLO / MUTE keys, black fader caps, and the
scribble strips, meters and LCDs in black windows. SETUP > GENERAL > CONSOLE >
SURFACE FINISH **BLACK** shows the black WING instead: a black surface with printed
frames, keys backlit lavender in black bezels and silver fader caps. The finish is
an app preference (not saved in shows) and changes the surface at once; the
touchscreen looks the same in both.

- **Three fader sections:** left (12 faders), center (8) and right (4), each with the
  WING's layer column: **VIEW** (the section's OVERVIEW; hold for CONFIG), the layer
  keys (left `1-12` ... `37-40 AUX IN`, `BUS MASTER`; center `DCA`, `MAIN MATRIX`,
  `AUX IN`, `BUS MASTER`; right `MAIN MATRIX`, `DCA`, `INPUTS`, `AUX IN`,
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
- **Fader strips** follow the WING's: a colour light bar (the strip's colour), the
  scribble strip (one backlight colour on every strip; number,
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
    lights with SOLO MODE SIP, filled while it mutes; the sample rate and clock; the
    AES50 A B C and SC chips, grey here; the DSP load is on UTILITY), the screen's own
    keys (INIT / COPY / SETTINGS on the HOME tabs) and the clock.
  - The **parameter bar** under the screen: six controls that follow what the screen
    shows (drag, mouse wheel, double-click to reset), values coloured by type as on
    the console; touching one outlines the control it changes.
  - **HOME** for the selected strip: the toolbar (thumbnails at the WING's fixed rows,
    the fader bar with the TAP point, MAIN, SENDS; on Channel Home also the on keys
    and the wrench to drag the slots and the TAP point), the **OVERVIEW** (INPUT,
    FILTER, LEVEL, EQ and MAIN panels; INPUT has the source's +48V and GAIN; as on
    the console, hold the +48V key for 2 s to switch it: it fills up meanwhile, and a
    tap does nothing),
    **ICON/COLOR**, **NAME**, **TAGS** (DCAs, mute groups, the talk tags TALK ON / TALK
    SOLO for both talk keys, A or B) and the **SAFE** key. As on a factory-initialized
    WING the faders start at -oo (buses 9-16 at 0 dB), the EQs off, and channels and
    aux have **LINK CUSTOMIZATION TO SOURCE** on: the strip shows the name, colour and
    icon of the source it listens to, and ICON/COLOR and NAME edit the source (the link key on those tabs, or NONE /
    ALL for every channel, turns it off; the channel then shows its own). Touching the
    MAIN tile on the INPUT screen opens **SELECT MAIN INPUT**: the source groups, NONE,
    the source's M / ST / M/S pairing, and COPY CUSTOMIZATION between the channel and
    its source (<< and >>). The slot screens follow chapter 6 of the manual: INPUT (CHANNEL
    INPUT, TRIM & BALANCE pad, FILTER, DELAY), FILTER SETUP, GATE / COMP with the
    model's faceplate (the KEY FILTER and XOVER MODE `>` keys open GATE KEY FILTER /
    COMP XOVER with the filter's response, FREQ and, for a band pass, notch or PEAK, Q
    (the crossover's DEPTH too) in boxes and on the knobs; for PEAK the XOVER page draws
    the signal with the band taken down by DEPTH; KEY SOURCE opens SELECT CHANNEL;
    the model fields open the WING's picture lists), EQ, INS 1 / INS 2 (with
    automix), MAIN SENDS and BUS / MATRIX SENDS. On INS 1 / INS 2, FX PROCESSOR opens
    **SELECT FX PROCESSOR** (NONE and the 16 slots, who uses each and its device) and FX
    TYPE the device list (see EFFECTS). A new channel starts as a
    factory-set WING: GATE -40 dB, 1:3, RANGE 40 dB, 10 / 10 / 200 ms; COMP -10 dB,
    3:1, KNEE 3, 50 / 20 / 150 ms, RMS, AUTO ENV on (aux: the WING GATE at -36 dB);
    key filters 1k0 Hz Q 2.00; LC 100.2 Hz (24 dB/oct), HC 10k02 Hz; the EQ bands at
    80.2, 200, 601.4 Hz, 1k50, 3k99 and 12k00 at Q 1.00 (buses, mains and matrices
    from 60.1 Hz to 12k00). On MATRIX SENDS a channel's or aux's send has **PAN LINK** as on BUS SENDS
    (touch the pan ring: OFF, LINK or LINK REVERSED): linked (the default) its pan
    follows the channel's, LINK REVERSED mirrors it, OFF gives it its own PAN; buses and mains send their
    panned output. **INIT** and **COPY** open CHANNEL INIT / COPY (scope and destinations);
    a bus, main or matrix's feed grid (on HOME, or the INPUT screen's BUS FEED / FEED
    box) opens its **FEED CONFIGURATION** (WING 3.0.5): the CHANNEL, AUX and BUS
    sources (disabled sends greyed out), selected by touch and drag or with NONE /
    ALL / ALL CH / ALL AUX / ALL BUS, then SEND MODE, IGNORE CHANNEL MUTE, SEND
    PANNING, SEND LEVEL and SEND ENABLE, each with KEEP, changed on APPLY. Holding an
    EQ band key resets that band.
  - **EFFECTS:** the FX rack, slots 1-8 premium and 9-16 standard. Each slot shows its
    device and its main control (DECAY, TIME ...), and while a strip inserts it, the
    strip (PRE/POST) and ON; a device inserted nowhere is shown dimmed. Tapping a slot opens the
    device on its **faceplate**, drawn like the hardware it recreates (a LARC remote
    for the vintage reverbs, a black 1176-style limiter with its VU meter...). The
    device list shows a tile per device: NONE and EXTERNAL (a plugin) first, then the
    tabs **FX1-8** (the premium devices, slots 1-8 only), **STD** and **CH** (the EQs and
    channel strips), in the console's order. The GATE, EQ and
    DYNAMICS screens show their model's faceplate too.
  - **METERS:** meters of the channels in groups of eight, AUX, BUS 1-8 / 9-16, DCA,
    MAIN and MATRIX, with number keys in the strips' colours (select) and M keys (mute).
    **SETTINGS** in the status bar sets PRE or POST for CHANNELS (and aux), BUSES, MAINS,
    MATRICES and DCAS, as on the console: PRE for channels and DCAs, POST for the others
    at first (the same settings as SETUP > SURFACE > METERING). A group's header opens
    its section's FADERS screen. Bigger consoles page.
  - **ROUTING** (status-bar keys CHANNELS / SOURCES / OUTPUTS), laid out as the
    console's: pick a channel, then its source. **MAIN / ALT** (CHANNEL INPUT) choose
    which of the channel's two inputs you patch (the ALT input, WING INPUT SELECT, can
    be a recorder playback channel). **+1 AUTO** moves on to the next channel; the
    lock (red when locked, green when open) protects the patch. **ST** and **NONE**
    are Sidecar's: ST pairs a channel's input with the next one, NONE clears the
    patch. The **SOURCE GROUP** selector opens the console's group menu. Sidecar's
    ports sit in the groups they stand in for: **LOCAL IN** is your audio interface's
    inputs 1-48 (LCL 1 ..., 24 tiles as on the console, more for bigger interfaces;
    tiles past the interface's inputs read n/a but can be patched), inputs 49-96 and
    97-144 are **AES50 B** / **C** (as the WING remote protocol numbers them),
    **WLIVE PLAY** the multitrack recorder's 64 playback channels (PLAY 1 ...),
    **USB PLAYER** the USB player and **OSCILLATOR** OSC 1 / 2. The other groups
    (AUX IN, AES/EBU, AES50 A, ST CONNECT, USB AUDIO, DANTE, BUS, MAIN, MATRIX, USER
    SIGNAL) can be selected and show n/a tiles, like a console without that card.
    SOURCES sets each input's source:
    name (RENAME), colour and icon, GAIN, +48V (stored; hold the key 2 s), POL, MUTE
    and MONO / STEREO / MID-SIDE pairs (a channel patched to a pair becomes stereo),
    its mute groups (TAGS), with its meter and the channels using it (as tiles under
    the tabs); channels get the source's processing first. REMOTE (stage-box preamps) and RCV
    CUST are shown but have nothing to control here. With SOURCE SOLO on, **SRC
    SOLO** solos the selected source
    through CH39 or AUX7 (the solo follows the selection; CLR SOLO ends it). The
    **OSCILLATOR** source group holds the WING's signal generator, OSC 1 and OSC 2:
    patch them to channels like any input. Selecting one on SOURCES shows SINE /
    PINK / WHITE (in the REMOTE column), FREQ (the sine's frequency, 20 Hz to 20 kHz:
    click for the usual test frequencies, or use the wheel or the FREQ knob) and LEVEL
    (-40 to -6 dBFS, the sine's peak; the noises have the same RMS) in place of +48V,
    POL and GAIN; MUTE switches it off. STEREO pairs OSC 1 and 2 on OSC 1's settings
    (the sine in phase on both sides, the noises uncorrelated), MID/SIDE decodes them
    as for other sources. The settings are saved with the show. OUTPUTS works
    output-first: pick an output (OUTPUT GROUP **LOCAL OUT** is the interface's
    outputs; **RECORDER** taps a signal for the USB recorder), then the signal feeding
    it from the source group: **BUS**, **MAIN**, **MATRIX** (each L / R), **MONITOR**
    (PHONES 1L / 1R, SPEAKERS 2L / 2R) or **USER SIGNAL** (the channels' and aux
    channels' own outputs). Defaults: every interface input on its channel (the
    console patches its eight local inputs to channels 1-8) and MAIN 1 on LOCAL OUT
    1-2 (the console uses LOCAL OUT 7-8 and feeds buses 1-6 to 1-6).
  - **SETUP:** GENERAL (lighting, clock, console info and size, the surface's finish
    SILVER / BLACK, OPERATION, OPTIONS),
    AUDIO (clock rate, INPUT SELECT: the global MAIN / ALT switch and each channel's,
    preferences such as MAIN LINK, DCA MUTEGROUPS, MUTEGROUP/SIP OVERRIDE, GLOBAL INPUT
    SELECT OVERRIDE, SOURCE SOLO (CH39 / AUX7) and the TALKBACK channel, SOLO SYSTEM with SOLO MODE LIVE / STUDIO /
    SIP, PFL/AFL per strip type and the solo / select preferences, audio device,
    AUTOMIX), SURFACE (meter taps, SEL DBL CLICK, SOF and scribble options, CH
    AUTOSELECT, FADER SPEED, paging, user layer link, alternative SOF; the **SECTIONS**
    key in the FADERS header opens Sidecar's FADER SECTIONS: section widths, SHIFT STEP
    and FADER VALUES), REMOTE (MIDI REMOTE CONTROL with the DIN and USB MIDI ports under
    it, HA REMOTE, NETWORK, REMOTE LOCK and Sidecar's WING REMOTE and WEB REMOTE) and DAW (DAW CONTROL,
    DAW PORTS; see *MIDI and DAW remote control*), PLUGINS (scanning, delay compensation,
    RUN PLUGINS IN SANDBOX; see *Stability*) and LOG (system health and the
    event log), with INITIALIZE CONSOLE and SHUTDOWN. **INITIALIZE CONSOLE** opens a page
    like the LIBRARY's scope window: pick strips, the CONTENTS to reset, DCAs, mute
    groups, FX slots and SOURCE groups (LCL: the audio interface's inputs, CRD: the
    playback channels, PLAY: the USB player, OSC), then INIT. With everything picked it
    resets the whole console to a fresh 40-channel one, as before. The status bar has MONITORS (the
    toolbar with the talk keys, PHONES / SPEAKERS / PH+SPK, DIM, MUTE, MONO, LR SWAP;
    TALK A / B: switch mode, AUTO DIM, USE SEND LEVELS and the bus, matrix and main
    destinations, the talk side dim with TALKBACK DISABLED while no talkback channel is
    set (touch it to pick one); PHONES and SPEAKERS, each its own: SOURCE, SRC LEVEL,
    SRC MIX, DIM, PFL DIM, BAND SOLO, DIRECT IN (any channel, bus, main or matrix, at
    its own level), the EQ (MONITORS EQ: 8 bands, PAN, WIDTH, and an RTA of the path,
    PRE or POST its EQ as EQ/RTA SETTINGS' RTA SOURCE says), the DIST slider (0.1-100 m,
    linear) with INVERT and DELAY, LIMITER with its gain reduction beside the meter, and
    the level; laid out as on a WING with firmware 3.1) and
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
  - **VIEW screens** (a fader section's VIEW key), laid out as on a WING with firmware
    3.1: the layer column on the right picks the section's layer (DCA, MAIN, AUX,
    BUSES, USER1, USER2) and the section (LEFT / CENTER / RIGHT); the bank keys left of
    it (1-8 ... 33-40, AUX, BUS 1-8, BUS 9-16, MAIN, MTX, DCA 1-8, DCA 9-16) show one
    bank of eight instead of the layer, the arrows under them page. OVERVIEW (every
    strip's blocks, EXCL SOLO in the status bar on a page with channels; a DCA shows its
    members, and touching it opens its assign screen: the DCA's tile in the title, the
    eight DCAs as tabs, the channels, aux, buses, mains and matrices as tiles, members
    framed gold, UNASSIGN and CLOSE; touching the DCA's tile in the title opens the
    DCA's panel: LIGHT and the 18 colours in a row, the icon gallery, SOLO BUS (phones
    and/or speakers: where the members' solo goes in SOLO MODE LIVE), the name field
    and the keyboard, every change applying at once; the "DCA" at a DCA column's foot
    is yellow while the DCA is soloed), FADERS (a DCA's name opens its panel, its
    label at the foot is yellow while it's soloed; send faders of a layer, and Sends on
    Fader, show and set the send level, MUTE switching the send), SENDS (sends on faders
    to one bus, main or matrix), CONFIG (edit any layer key by dragging channels, buses,
    DCAs, send levels, bus-to-bus sends, FX parameters or MIDI CCs onto or between the
    page's slots; the arrows page, one page past the end to append; NEW and DELETE /
    RESTORE left of the slots; a **MIDI CC** fader sends its CC, 0-127, out of EXTERNAL
    MIDI CONTROL and follows the CC coming in) and MUTE GROUPS (the eight groups'
    members, a column red while its group is engaged, the group keys on the right; a
    column's header (MGRP.n over its number) opens the MUTE GROUP n window, a name
    field and the keyboard, the name changing as it's typed; touching the rest of a
    column opens the group's assign screen like a DCA's, with a SOURCE tab for the
    sources it mutes: one source group at a time as ROUTING's tiles, eight to a row,
    the groups (LOCAL IN ... USER SIGNAL) as tiles on the right).
  - **UTILITY:** system health and the event log (also on SETUP > LOG).
  - **RECORDER** (the status bar's **LIVE** card icon, red while recording): laid out
    like the WING-LIVE screens. See *Multitrack recording* below.
- **Channel strip section** (top left): the CHANNEL STRIP DETAILS LED panel (sources,
  filters, LEVEL and GR meters, bus sends, groups) around a small screen; CHANNEL
  PROCESSING with an encoder over each of INPUT, FILTER, GATE, COMP, INSERT and MAIN
  (press: show it on the screen, again: next parameter; hold: on/off); CHANNEL
  EQUALIZER with SHIFT, four band encoders, BLEND/MIX, EQ and GAIN / WIDTH / FREQ;
  VIEW opens the shown slot on HOME. With the WING EQ the encoders are bands 1-4 and
  SHIFT gives the shelves (bus EQs: bands 5-8 and the shelves); GAIN / WIDTH / FREQ
  choose what they set. With an analog EQ model the encoders set the model's
  parameters in the order of its screen, four at a time: each SHIFT press moves on to
  the next four (1-4, 5-8, 9-12, 13-16, then back to 1-4; SHIFT lights past the first
  four), the caption under SHIFT shows which four, and the names under the encoders
  show the parameter on each.
- **USB / monitoring panel** (top right): VIEW (the MONITORS screen; held: the USB
  recorder / player), TALK LEVEL,
  PHONES LEVEL, MONITOR LEVEL, **TALK A / TALK B** (AUTO: held talks while held, a
  short press latches; PUSH; LATCH), **DIM** (hold: mute the monitor) and **MONO**
  (hold: swap left and right) on the selected monitoring path; held functions blink,
  a press ends them. DCAs named MON A, MON B or MON LVL set the phones, speakers or
  both levels.
- **4-Channel section** (under the monitoring panel): GAIN, GATE, COMP, FILTER, F1-F3
  and PAN keys give the four strips of the right fader section an encoder and a key
  each (trim and the source's +48V, switched by holding the key 2 s; gate / comp
  threshold and on; low / high cut; bus sends; pan / width and MAIN 1), with their
  scribble strips on a four-way LCD.
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
  monitoring panel), laid out as the console's: the browser on the left (USB | PLAYLIST
  | ADD ALL | FOLDER | RENAME | DELETE | REC PATH; folders, WAV / FLAC / AIFF / MP3 files and playlists; a
  folder's `>` opens it, inside a folder its row at the top goes up a level, a double click plays;
  ADD ALL with no clip selected adds the folder shown, ADD a selected clip),
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
  paused). Choose a clip first (select or double-click it): the files of the browsed folder
  then play one at a time; until then PLAY is off and the player blank, as on the console.
- **Playlists:** CREATE PLAYLIST (or ADD a clip, ADD ALL for a folder), then on the
  PLAYLIST page (its key shows N SONGS while a playlist is open) REPEAT, REMOVE, SKIP (passed over), STOP (stop after the clip; held: all
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
  SELECT, automix enables); an optional **fade time**, and notes. A fade glides the
  continuous values: channel, aux, bus, main, matrix and DCA faders, pan and width,
  and the send levels and pans (bus, main and matrix sends). Levels move along the
  fader's travel, as a moving fader would (a fade up from -oo is heard from the start),
  pans and widths in a straight line. Everything else (mutes,
  ON keys, send modes, links, routing, processing) is set when the fade starts. Moving
  a fader or knob that is fading (on the surface, the screen, from MIDI, a DAW or a
  WING remote) takes it out of the fade: it stays where you put it while the rest
  carries on. A new recall finishes a running fade first. **Recall-safe** strips and the **GLOBAL safes** (DCAs, mute groups, FX
  slots, sections, console settings) are skipped. **PARTIAL LOAD** recalls a chosen
  scope (**NO GLOB** ignores the safes), **RESTORE** all of the scene, **PARTIAL
  UPDATE** stores a chosen scope into it; both partial keys keep their last scope. Use
  **GO >** and **<** (or `Ctrl+G`) to step through scenes during a show.
- A **snippet** belongs to a scene and recalls only the strips and sections you choose
  (and DCAs, mute groups, FX slots). **REC**, change parameters, **STOP** and **SAVE**:
  the snippet holds exactly the parameters you changed (within REC FOCUS). Fire it by
  hand, or mark it to **fire with its scene**.
- Plugins are saved with their state; a chain with all of its plugins in order, with
  each one's BYP, state, zoom and collapsed state (a single plugin is saved as before,
  so shows without chains still open in earlier versions). Scenes, snippets and FX presets recall a chain's
  states and BYP keys into the same plugins in the same order only, and report a
  slot whose chain differs. In safe mode no plugin is loaded; **Reload with plugins** loads each
  plugin of a chain on its own (one that fails is left out and reported), and a plugin
  that crashes the app while loading is blacklisted, as for single plugins.
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
  for the file name; the scene or snippet in the file takes that name.
- **IMPORT** on the SNAP tab adds the scene file selected at INT / USB (or chosen in a
  file dialog) after the selected scene; on the SNIP tab it adds the snippet file to
  the selected scene. Double-clicking a file in the list imports it too. Importing
  never changes the console: load the scene or snippet when you want it.
- The files are Sidecar's own XML, like show files and presets. They are not the WING's
  `.snap` / `.snip` files; a WING snapshot selected in the list is applied with LOAD (or
  IMPORT WING on the SHOW tab).
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
  computer: SETUP > REMOTE, under MIDI REMOTE CONTROL, picks an input and an output for DIN and for
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
- **Discovery** (UDP 2222) answers `WING?` with the console name, model `sidecar`,
  serial and firmware `3.1-...` (Sidecar speaks the WING 3.1 parameter tree).
- **OSC** (UDP 2223): get and set any parameter by path or by `#id`, toggles, node
  lists, node dumps and descriptions, node strings (`/ch.1.fdr=-3,mute=1`), and the
  `/*s`, `/*S` and `/*b` subscriptions (renew every 10 s). Changes from the UI, scene
  recalls and other clients are pushed to the subscriber. Replies and descriptions are
  formatted as a WING 3.1 formats them (checked against a factory-set console): values
  with the parameter's fixed decimals and kHz notation (`100.2`, `1k50`, `1k0`),
  running times as `0:00`, descriptions such as `lin [-15.0 .. +15.0 dB], 301 steps`,
  `int [0 .. 100 %]`, `list [48000.0, 44100.0]` and `(node)`, and an integer's number
  counted from the bottom of its range (`/ch/1/col` 1 replies `"1" 0.0 0`). Like the
  console, Sidecar keeps a float set over OSC or the Native protocol (or from a
  snapshot) on the nearest of the parameter's steps, so a Q of 1.00 reads back
  0.9979702 and 100 Hz on the TAP EQ 99.68543.
- **Native protocol** (TCP 2222): node definitions and data, set/get by id, navigation,
  change notifications to every connected client, and meters over UDP.
- **Comparing with a real WING:** `tools/wing/wing_compare.py` (Python 3.8 or newer,
  nothing to install) reads every parameter from a console over OSC, read-only, and
  reports the differences. With Sidecar's WING REMOTE enabled and a WING on the same
  network: `python3 wing_compare.py discover` lists both (Sidecar answers with model
  `sidecar`), then `python3 wing_compare.py compare WING_IP SIDECAR_IP` writes
  `wing.json`, `sidecar.json` and `report.md` to a new `wing_compare-...` folder. The
  report lists the parameters only one side has and every value, type, range and
  choice that differs, per top-level node. Initialize both consoles (or load the same
  show) first; `dump` and `diff` run the two steps separately.
- **Comparing the sound with a real WING:** `tools/wing/audio_compare` (Python 3.8+ with
  numpy and scipy) writes test signals to play on the WING's USB player, a test plan
  (EQ, filters, gate, compressor and other dynamics models, FX) with the settings for
  channel 1, and reports, from the WING's USB recordings, how Sidecar's processing of
  the same signals differs: frequency and phase response, distortion, static curves,
  attack and release, reverb decay, delay echoes. Sidecar's side is rendered offline
  by its engine (`cargo run --release -p livedaw-wing --example render`). See the
  kit's README for the steps.
- **What is tested:** libwing's own clients (`wingprop`, `wingmon`, `wingdrive`'s
  test battery and model capture, its OSC client), and every OSC path the Bitfocus
  Companion WING module builds that exists in the 3.1 tree. **WING Edit and WING
  Copilot have not been tried**, so they may not work.
- **What maps:** 88.2 % of the WING 3.1 tree's parameters (60,491 of 68,556,
  counting every model's parameters) drive the engine: channels, aux, buses, mains
  and matrices (name, colour, icon, input and ALT patch, trim, filters, gate, EQ and
  dynamics models with all their parameters, inserts, fader, mute, pan, width, sends,
  main sends, solo, TAP WIDTH and TAP EQ), DCAs (with their SOLO BUS), mute groups, the FX rack, output
  routing, the sources of the AES50 A/B/C and WING-LIVE card groups (name, colour,
  icon, gain, mute, polarity, +48V, mode, mute-group tags), the oscillator (the OSC
  group: its sources and `osc/mode`, `lvl`, `f`), SOLO MODE, the solo bus,
  MUTEGROUP/SIP OVERRIDE and SOURCE SOLO, talkback, both monitor paths (`/cfg/mon`:
  source, levels, DIM, PFL DIM, BAND SOLO, DIRECT IN, EQ, PAN, WIDTH, INVERT, DELAY,
  LIMITER), the RTA settings and other console settings. Clients can also use
  LISTEN (BAND SOLO, KEY SOLO and XOVER SOLO: `eq/$solo`, `$solobd`, `gatesc/$solo`,
  `dynsc/$solo`, `dynxo/$solo`), see the inserts' state (`$stat`) and which strip
  uses each FX slot (`/fx/N/$a_chn`, `$a_pos`), set a channel's source GAIN and
  +48V from the channel (`in/set/$g`, `$vph`), and recall scenes (`/$ctl/lib`:
  `$actionidx` with GO, GO NEXT, GO PREV, GO TAG; PREV / NEXT move the selection;
  `$active`, `$activeid` (the scene's tag) and `$actshow` show the current scene and
  show). They also drive the matrix sends' PAN LINK (`send/MXn/plink`), a strip's
  MUTEGROUP/SIP OVERRIDE state (`$muteovr`), a DCA's icon and scribble strip light,
  the USB recorder and player (`/rec`, `/play`: REC, PAUSE, STOP, NEW FILE, PLAY,
  NEXT, PREV, PLAYFILE with `$playfile` relative to the USB folder, the position,
  REPEAT, REC PATH and the recording format; state, file, time and the clip's format
  read back) and the recall safes (`/$ctl/safes`: `ch`, `aux`, `bus`, `main`, `mtx`
  are the strips' SAFE keys, `dca`, `mute`, `fx` the GLOBAL safes; lists such as
  `1,3,5-8`). Not mapped, with no meaning in Sidecar: a DCA's monitor bus (DCA solo
  goes to each member's monitor bus), a channel's TAP SOLO (`$presolo`: solo is
  PFL or AFL per strip type), the player's song tags (`$artist`, `$album`, `$songs`)
  and the safes of source and output groups, surface areas, custom controls and
  setup. The fader sections' layers (`/$ctl/layer/L`, `C`, `R`) and the custom
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
- **Files:** as on the WING, select a WING snapshot (`.snap`) at INT or USB on LIBRARY >
  SNAP and press **LOAD** (or double-click it) to apply it to the console. LIBRARY > SHOW
  > **IMPORT WING** does the same from a file dialog, or adds a WING show's (`.show`)
  snapshots as scenes. The log lists
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

## Web remote

Any browser on the local network (a tablet, a phone, another computer) can show
Sidecar's window and control it: the page shows exactly what the window shows, and
clicks, drags, the wheel and keys in the page act on Sidecar as if done here.

- **Turn it on:** SETUP > REMOTE > WEB REMOTE > ENABLE (off by default). **PORT** is
  8080 unless changed (touch the box). The column beside HA REMOTE lists the addresses
  to open, such as `http://192.168.1.20:8080`, and how many browsers are watching. The
  settings are saved in the app settings and the server starts with the app when enabled.
- **PIN:** touch the PIN box and enter 4 to 8 digits (empty: no PIN). The page asks for
  it before it shows anything; the PIN is checked by Sidecar, and wrong guesses lock the
  address out for a while (3 tries, then 5 s, doubling up to 5 minutes; 20 wrong PINs
  a minute from anywhere lock everybody out for a minute). Without a PIN anybody on the
  network can control the console.
- **VIEW ONLY:** browsers watch, their input is ignored.
- **In the page:** the screen is fitted to the browser window. A mouse works as here
  (right-click, double-click, wheel); keys go to Sidecar while the screen has the focus
  (the browser keeps F5, F11, F12 and its Ctrl+R, L, T, W, N, Q). On a touch screen one
  finger is the mouse (a press waits 90 ms, or until the finger moves, so that a second
  finger can still start a pinch); two fingers pinch and pan the page's view, and
  Ctrl+wheel (a trackpad pinch) zooms it. **FIT** shows the whole screen again,
  **RIGHT** makes the next touch a right-click, **FULL** fills the screen. The bar shows
  the frames and kilobytes per second and the round trip. The page reconnects by itself.
- **Limits:** one shared view: the page follows what the window shows and every browser
  sees the same. Menus and alerts over the window are shown; other windows (plugin
  editors, the scope, file choosers) are not. A browser's press moves this computer's
  mouse pointer there (menus open at the pointer); the last input wins when someone
  works here and in a browser at once. Up to 8 browsers at a time, at up to 30 frames a
  second: only what changed is sent (64-pixel tiles, PNG, JPEG for busy pictures), so a
  still screen costs nothing and a moving fader some 100-200 KB/s. The page is plain
  HTTP: use it on a network you trust. Nothing but the screen is served.

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
| Plugins | A plugin that outputs NaN/Inf or throws is **auto-bypassed on the spot**. Its input passes through, and the FX slot is flagged. In a plugin chain of two or more only that plugin is bypassed (the rest keeps running) and the event log names it. Two inserts on one slot never run it concurrently. Per-slot CPU is measured. |
| Plugin scanning | Scans run **in a separate process**, so a plugin that crashes during a scan only kills the scanner. |
| Plugin loading | The plugin being loaded is recorded on disk first. If it crashes the app, it's blacklisted on the next start. |
| Plugin sandbox (experimental) | With **SETUP > PLUGINS > RUN PLUGINS IN SANDBOX** on, each plugin loaded afterwards runs in its own helper process (see below): a plugin that crashes or hangs only takes its helper down. Its FX slot is bypassed, the event log says so, and the console keeps running. |
| Strips | Every strip checks its own output. NaN/Inf is silenced and the DSP reset before it can poison a bus. |
| Outputs | A brick-wall **safety clip** (adjustable ceiling) protects the amplifiers and speakers. |
| Device | A **watchdog** reopens the audio device if it errors, disappears or stops calling back. Retries back off. |
| Crashes | The show is autosaved every 30 s. After an unclean exit, the app restarts in **safe mode** (console restored without plugins) and offers to reload the plugins. |
| Files | Show files are written atomically (temp file, verify, rename) with a `.bak` of the previous version. A corrupt file falls back to the backup. |
| Shutdown | SIGTERM/SIGINT quit cleanly (state saved), so OS shutdowns aren't mistaken for crashes. |
| Testing | Unit, integration (real LV2 plugins) and multi-threaded stress tests run under AddressSanitizer, UndefinedBehaviorSanitizer and ThreadSanitizer in CI. |

The **UTILITY** screen shows device health, xruns, DSP load, fault counters and an event log.

### Plugin sandbox (experimental)

**SETUP > PLUGINS > RUN PLUGINS IN SANDBOX** (off by default) runs every plugin you
load from then on in its own helper process (`SidecarPluginHost`, installed with
Sidecar). Plugins already loaded keep running where they are until you load them
again (or reload the show); shows store only the plugin and its state, so they open
either way. Each plugin of a chain is sandboxed on its own (it may mix with plugins
running in-process, depending on when each was loaded).

- **Faults:** if the plugin crashes, or doesn't deliver a block in time, its FX slot is
  bypassed on the spot (the dry signal passes; the slot shows FAULT - BYPASSED) and
  the event log says what happened. In a chain of two or more only that plugin is
  bypassed, and CLEAR in its header runs it again. Sidecar restarts the helper with the plugin's last
  known state (from the last show save, or at most 10 s old); the slot stays bypassed
  until you clear the fault (EFFECTS, UTILITY). A plugin that fails three times in a
  minute stays off until you load it again.
- **Latency:** the sandbox adds one audio block (the buffer size, e.g. 128 samples) on
  top of the plugin's own latency (per sandboxed plugin of a chain). The plugin reports the sum, so delay compensation
  lines the other paths up with it and FX MIX delays its dry signal to match, as for
  any plugin with latency (EFFECTS and SETUP > PLUGINS show it). The insert's own
  strip still comes that much later through the uncompensated paths (solo, monitoring,
  direct outs, recorder taps), and while the slot is bypassed after a fault the
  console is realigned without it.
- **Editor:** opening the plugin shows a Sidecar window with SHOW PLUGIN WINDOW and
  generic controls for its parameters; the plugin's own editor opens in a separate
  window of the helper. If it can't open (no display, or the plugin refuses), the
  window says so and the generic controls still work.
- **State and automation:** saving and loading shows, scenes that recall plugin state
  and parameter changes from Sidecar all go through the sandbox. Plugins run in stereo
  and take no MIDI, as in-process.
- Each sandboxed plugin is a process of its own, and every block makes a round trip
  to it, so the sandbox costs some memory and CPU per plugin. Restarting a helper
  briefly holds up the screens while the plugin reloads.
