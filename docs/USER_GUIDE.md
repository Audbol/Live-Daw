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
  - **INPUT:** trim, balance, polarity and delay (m, ft, ms or samples, converted as
    the WING does: 0.3 m per ms, 0.1 to 150 m, at least 16 samples).
  - **FILTER:** low cut (6-24 dB/oct), high cut (6/12 dB/oct) and the tool filter
    (TILT, MAX, AP1, AP2).
  - **GATE slot and DYNAMICS slot:** each holds any of the WING's **32 dynamics models**
    (gate, ducker, Even 88, Soul 9000, 160 VCA, LA leveler, 1176-style, Fair Kid,
    Bus comp, NSTR, limiters, de-esser...), with sidechain filter and source
    (KEY SOURCE: the strip itself or any input channel, taken at the KEY TAP: IN,
    FILT, 3, 4, 5, PFL, AFL or POST), and a crossover band on the dynamics slot. Aux,
    buses, mains and matrices key their DYNAMICS from the strip itself or any bus,
    main, matrix or aux, as on the WING, taken at BUS, DYN, PRE FDR, POST FDR, EQ or
    INS 2. A key from a strip that runs later in the block (a bus keyed by a main) is
    one audio block late.
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
- **Every bus feeds every other bus** (BUS → BUS, as the WING's 16): a bus runs after
  the buses sending to it, so a chain arrives in the same block; a loop (Bus 3 → Bus 7 →
  Bus 3) is cut at its lowest bus, whose loop send takes the previous audio block (the
  WING delays every bus to bus send by 8 samples). Matrices take the mains and a
  **DIRECT INPUT**: OFF, AES (no AES/EBU input here: silent), the PHONES (MON.PH), the
  SPEAKERS (MON.SPK) or the solo bus (MON.BUS), a block late, with its level and
  INVERT.
- **Buses, mains and matrices are channel sources** (ROUTING > CHANNELS, groups BUS,
  MAIN, MATRIX), a block late; **user signals** copy any strip and **user patches** any
  port (ROUTING > SOURCES, USER SIGNAL); any source plays on an output without a
  channel (ROUTING > OUTPUTS).
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
    plugins** in series (see *Plugin chains*), or the WING's **EXTERNAL** effect: an
    **outboard insert** through your interface's outputs and inputs (see *Outboard
    inserts*).
- **Outboard inserts** (WING manual 11.11, EXTERNAL): the EXT device's **SOURCE**
  key chooses **PLUGIN** or **OUTBOARD** (the FX slot menu has **Outboard insert
  (SEND / RETURN)** too). An outboard slot sends what its strip inserts to its **FX
  SEND** and takes its **RETURN** back in its place. Its page, **EXTERNAL EFFECT**,
  is laid out as the console's: **SEND** shows the two outputs FX SEND is patched to
  (tap it for ROUTING > OUTPUTS, source group FX SEND: slot n's send is FX SEND 2n-1
  / 2n, shown as FX nL / nR) and **TRIM** (the send level, -18 to +18 dB); **FX MODE**
  **MONO** (one send and one return input, on both sides), **STEREO** (two each) or
  **MID/SIDE** (sends mid and side, decodes the return back to left and right);
  **RETURN** the input the gear comes back on (tap it: LCL, A, B, C, CRD or PLAY and
  the first input; STEREO and MID/SIDE take it and the next one; OFF passes the
  signal through, the send still going out), with the round trip under it; and
  **LATENCY** (0-800 samples; the WING tree gives no unit and counts insert latency in
  samples, the manual says ms). The return can only come back in the next audio
  block, so the slot's latency is one block plus the latency your interface reports
  both ways, plus LATENCY for the gear's own delay or what the interface doesn't report; delay
  compensation and the FX MIX's dry signal use it as a plugin's. Measure the round
  trip and set LATENCY if the direct and returned signals don't line up. The return
  source's GAIN, MUTE and pairing (ROUTING > SOURCES) apply. Shows and scenes keep
  the EXT settings and the FX SEND outputs (FX presets leave the outputs out); an
  EXT slot from an older show is a plugin slot, as before.
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
    with the show, and with its state in scenes and snippets: recalling one puts them
    back (scenes stored before keep the current view).

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
  bus stages (buses into the buses they feed, buses into mains, mains into matrices;
  a bus send closing a loop and the other block-late paths aren't compensated) and
  the tap of each send (an insert counts only once the send's tap point is past it).
  A plugin's FX MIX delays its dry signal by the plugin's latency too; an outboard
  insert counts as a plugin with its round trip as latency. It is on by
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
  published parameter map. A model starts with the console's own defaults (the values
  WING Edit, which runs the WING's firmware, gives it: a HALL starts at PRE DLY 50 ms,
  DECAY 1.57 s, DAMPING 5k7 Hz), and FX RESET returns to them. Values read as on the
  console: fixed decimals per parameter, kHz with the k as the decimal point
  ("5k7 Hz", "2k99 Hz") and times in ms however long ("1503 ms").

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
touchscreen looks the same in both. WING remote clients see the black WING as model
`wing-bk`.

SETUP > GENERAL > CONSOLE > CONSOLE MODEL **COMPACT** shows a WING COMPACT instead of
the full-size WING. The surface changes at once, and the choice is an app preference
like the finish. The COMPACT is black, so it always shows the black finish. It fits a
1366 x 768 window.

- **Top row:** the display unit (screen keys, screen, knob and CLR SOLO, the six
  encoders) and, at its right, the panel with USB AUDIO/DATA and MONITORING/TALKBACK.
- **Fader section:** 12 faders in three modules of four, with the COMPACT's layer
  column: **VIEW**, `1-12`, `13-24`, `25-36`, `37-48 AUX IN`, `BUS MASTER`,
  `MAIN MATRIX`, `DCA`, `USER 1`, `USER 2` and the shift keys. The section width
  settings don't apply: it always has 12 faders.
- **Key section:** **BUS**, **DCA**, **MAIN MATRIX** and **USER** choose what the 16
  keys do.
  - BUS: keys 1-16 are BUS 1-16. A press turns on Sends on Fader to the bus; held, it
    spills the bus (the sources sending to it come onto the faders). Either way the
    bus goes on fader 13. SETUP > SURFACE > BUS SPILL swaps press and hold.
  - MAIN MATRIX: keys 1-4 are MAIN 1-4, keys 5-8 the custom control keys of the set
    M/M, keys 9-16 MATRIX 1-8. A MAIN key pressed turns on Sends on Fader to the main
    (the faders show the channels' main sends); held, it spills the main: the
    channels, aux and buses feeding it come onto the faders (MAIN SPILL swaps them). A
    MATRIX key pressed spills the matrix: the channels, aux, buses and mains sending to
    it come onto the faders as their send levels; held, it turns on Sends on Fader to
    the matrix (MATRIX SPILL, on at first as on a factory-set WING, off swaps them).
    Either way the main or matrix goes on fader 13.
  - DCA: a press (or a hold) spills the DCA, its members coming onto the faders, and
    puts it on fader 13, both, as on a WING COMPACT. With USE DCA BUTTONS 1-16 AS CC
    on, the keys are the custom control keys of the sets D1-D4.
  - USER: the custom control keys of the sets U1-U4 (keys 1-4 U1, 5-8 U2, 9-12 U3,
    13-16 U4).
  - A key pressed again lets its bus, main, matrix or DCA go: fader 13 goes back to
    MAIN FADER, and Sends on Fader or the spill ends.
  - DCA assignment, as on the console: press a DCA key, hold fader 13's SELECT,
    and press the SELECT keys of the strips to put in the DCA (or take out; the
    layer keys still work meanwhile). While SELECT is held, the strips' SELECT
    keys light on the DCA's members.
- **VIEW** opens the FADERS screen. Hold it for CUSTOM CONTROLS on the keys' set (USER
  1-4 in USER mode, MAIN/MTX 5-8 in MAIN MATRIX mode, DCA 1-4 in DCA mode, DAW KEYS
  1-4 in DAW mode); ‹ › step through the sets. Each set has four keys, each with a function, a name and a
  colour. The colour shows beside the key's name on the LCD. A key with a parameter
  (FX PAR) also hands it to the knob right of the display, as the COMPACT's "hold the
  custom control and use the knob to the right of the main display". **DAW
  LAYER** (CC EDIT > OTHER) is the COMPACT's way into DAW remote control: assign it
  to a key, and that key switches DAW mode on and off as the full-size WING's DAW key
  does. In DAW mode the 16 keys and the four mode keys do nothing until you assign
  them, as on a WING COMPACT: they play sets of their own, **DAW KEYS 1-4** .. **DAW
  KEYS 13-16** (keys 1-16) and **DAW MODE KEYS** (BUS, DCA, MAIN MATRIX, USER), empty
  at first. Hold VIEW in DAW mode to edit them on CC EDIT (any function: DAW BUTTONS,
  MIDI, mutes ...). A key acts as long as it's held, and the LCD (which reads DAW)
  names it. So that you can get out again, a key whose DAW control is empty but whose
  control in the mode's own set is DAW LAYER still switches DAW mode off. The sets are
  saved with the show (custom control layers 33-37); WING remote clients don't see
  them, as the WING has no node for them.
- **Fader 13** is the main fader: SETUP > SURFACE > MAIN FADER > SOURCE (OFF, a main,
  a matrix or a DCA), MAIN 1 at first. The BUS and MAIN MATRIX keys put another bus,
  main or matrix on it while their key is selected. KEEP MAIN FADER leaves it on MAIN
  FADER whatever key is pressed. It has no scribble strip, only its meter: the small
  LCD beside it shows what fader 13 controls and what the 16 keys are, a lit key's
  cell filled.
- SETUP > SURFACE has the COMPACT's own rows: USE DCA BUTTONS 1-16 AS CC, MAIN FADER,
  KEEP MAIN FADER, **DCA SPILL ON SELECT** (the COMPACT's one DCA spill setting, off
  at first: on, SELECT on a DCA strip spills it, as the full-size WING's DCA SPILL; a
  DCA key always spills its DCA and puts it on fader 13), **BUS SPILL**, **MAIN
  SPILL** and **MATRIX SPILL** (on: the key's press spills and its hold turns on Sends
  on Fader; off: the other way round; at first BUS and MAIN off, MATRIX on). Settings
  from an earlier Sidecar with its own DCA SELECT SPILL row on turn DCA SPILL ON SELECT
  on. The full-size WING's rows
  USE F1-F3 AS CUSTOM CONTROLS, RIGHT SECTION SENDS ON FADER, USER LAYER LINK and DCA
  SPILL are hidden.
- The COMPACT has no channel strip section, custom controls section, 4-Channel
  section, center or right fader section, wheel or DAW key; Sidecar leaves them out.
- The VIEW screens show the COMPACT's two sections at the foot of the layer column:
  **COMPACT** (the fader section) and **VIRTUAL**, which only the screens have (WING
  manual 5.7): eight layers of its own, eight faders wide, empty and unnamed at first
  (the empty keys in the column). Fill them on VIEW > CONFIG and name them with
  RENAME, then mix them on FADERS or OVERVIEW. They are saved with the show, and the
  full-size WING keeps them.
  ROUTING has no AUX IN or AUX OUT group, as the COMPACT has no local aux inputs or
  outputs. SETUP > SURFACE > CH AUTOSELECT has one key, **CMPCT**.
- WING remote clients see a COMPACT: Sidecar answers with model `wing-compact`.
  Fader 13, the keys' mode and CH AUTOSELECT CMPCT are `/$ctl/cfg/mfdr`, `cscmode`
  and `autosel_CMPCT`, both ways; KEEP MAIN FADER, DCA SPILL ON SELECT, BUS, MAIN and
  MATRIX SPILL and USE DCA BUTTONS 1-16 AS CC are `mfdrkeep`, `dcaspill`, `busspill`,
  `mainspill`, `mtxspill` and `dcacc`. The
  key sets are `/$ctl/user/U1`-`U4`, `MM` and `D1`-`D4`, so WING Edit and Mixing
  Station see and set them (on the full-size WING too, as WING Edit lists them there),
  and the virtual layers are `/$ctl/layer/VRT/1`-`8`.
- The COMPACT has 24 local MIDAS PRO preamps where the full-size WING has 8, no local
  aux inputs and outputs, and 1x2 GPIO instead of 2x2. Sidecar's sources are its
  audio interface's inputs either way, so this changes no routing; LCL 1-24 all have
  LOCAL IN's GAIN range (-2.5 to +45 dB in 2.5 dB steps).

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
  SURFACE, then SELECT on the DCA), a strip's sends (SPILL on BUS / MATRIX SENDS), the
  channels, aux and buses feeding a main (FADER SPILL > MAIN on a CC key), the sends of
  the strips sending to a matrix (FADER SPILL > MTX) or an FX device's parameters (FX
  SPILL on the insert screens; MUTE sets a parameter to its default, an EQ gain to
  0 dB). Spill from the right section goes to the center section, a FADER SPILL key
  to its AREA (LEFT, CENTER or RIGHT), everything else to the left; the same key again
  brings back the layer and its scroll position. With SETUP > AUDIO > BUS SoF/SPILL
  ACTIVATES SOLO a bus, main or matrix spilled is soloed.
- **Fader strips** follow the WING's: a colour light bar (the strip's colour), the
  scribble strip (one backlight colour on every strip; number,
  source ports or mono, icon, the name on an inverted bar), SELECT, SOLO, the meter
  block (DYN LED, a stereo six-segment meter, GATE LED), MUTE and the fader with
  its printed scale. The meter reads PRE or POST as SETUP > SURFACE > METERING sets
  for the kind of strip (PRE for channels and DCAs, POST for the others at first): PRE
  is after TRIM and before the EQ and dynamics (TRIM moves it, the EQ doesn't, as on
  the console), POST after the fader; a DCA's shows its loudest member. The LEDs are
  green below -18 dB, orange from -18 dB and red at CLIP. SELECT on the selected strip again opens its HOME screen. With
  SETUP > SURFACE > SEL DBL CLICK **BUSFX** (the default), a bus, main or matrix opens
  its INS 1 screen instead, or INS 2 when INS 1 has no FX. MUTE
  flashes while a mute group, a DCA mute or SIP mutes the strip; with MUTEGROUP/SIP
  OVERRIDE, pressing it unmutes the strip until that mute changes.
- **SOF FLIP (Sends on Fader):** select a bus, main or matrix and the input faders set
  their send to it; select an input and the bus, main and matrix faders set its sends.
  With SETUP > SURFACE > ALTERNATIVE SOF MODE, hold SOF FLIP for this standard SOF,
  hold a strip's SELECT for the alternative one (its destination stays), and press SOF
  FLIP to end either (manual 4.9).
- Surface keys act on a press; keys with a second function act on **hold** (half a
  second, or right-click): slot on/off, VIEW for CONFIG, the shift keys.
- **Main display**, a 16:10 screen with the seven screen keys on its left (over its
  full height) and, on its right, **CLR SOLO**, the main L/R meter, the knob for fine
  adjustments of the last touched control and the blank key. The meter shows the MAIN
  METER SOURCE of SETUP > SURFACE (or a right-click on it): MAIN 1-4, MTX 1-8, PHONES,
  SPEAKERS or SEL CH (the selected strip). The blank key taps the tempo of a delay
  open on EFFECTS or on an INS 1 / INS 2 screen: the time between two taps becomes its
  TIME (OILCAN's TIME and BBD's DELAY dials turn to that delay time). Its LED blinks
  at that tempo while it can tap, and is lit where the screen gives it another
  function.
  - **Surface lock:** hold **HOME** to lock the surface and the touchscreen; the audio
    and the remotes go on. Hold other keys beside the display together with HOME (or
    right-click them just before) and they become the password: only HOME with the
    same keys unlocks. Ctrl+L is HOME alone. With SETUP > GENERAL > SHOW METER PAGE
    WHEN LOCKED, the METERS screen shows while locked. Restarting Sidecar unlocks.
  - **Screen only** (F10, Ctrl+Shift+F, the maximize key under the right-hand
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
    tap does nothing; MAIN follows the SOLO MODE: SOLO BUS in LIVE, the mains in a row
    with their PRE badges in STUDIO, and in SIP the **SOLO SAFE** locks on input and aux
    channels: closed, SIP never mutes the channel),
    **ICON/COLOR**, **NAME**, **TAGS** (DCAs, mute groups, the talk tags TALK ON / TALK
    SOLO for both talk keys, A or B) and the **SAFE** key. As on a factory-initialized
    WING the faders start at -oo (buses 9-16 at 0 dB), the EQs off, and channels and
    aux have **LINK CUSTOMIZATION TO SOURCE** on: the strip shows the name, colour and
    icon of the source it listens to, and ICON/COLOR and NAME edit the source (the link key on those tabs, or NONE /
    ALL for every channel, turns it off; the channel then shows its own). Touching the
    MAIN tile on the INPUT screen opens **SELECT MAIN INPUT**: the source groups, NONE,
    the source's M / ST / M/S pairing, and COPY CUSTOMIZATION between the channel and
    its source (<< and >>). The slot screens follow chapter 6 of the manual: INPUT (CHANNEL
    INPUT with MAIN / ALT and INPUT SELECT INDIVIDUAL / GLOBAL, TRIM & BALANCE pad,
    FILTER, DELAY), FILTER SETUP, GATE / COMP with the
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
    meters on a page show the device's live signal: gain reduction, the C5-COMBINATOR's
    bands moving over the input's spectrum, the de-esser's band LEDs, the enhancer's,
    psycho bass's and tape machine's VUs, the velvet imager's meters, correlation and
    scope, the speaker manager's live DYN EQ curve, each channel-strip section's own
    reduction, the EXTERNAL EFFECT's SEND and RETURN; levels move like every other
    meter (see *How the meters move*). The PRECISION LIMITER's 1 ms look-ahead is
    compensated (delay compensation and FX MIX). On the reverbs, delays, modulation
    and pitch devices: VINTAGE ROOM's INPUTS / OUTPUTS, DIMENSION CRS's OUTPUT, TAPE
    DELAY's record-level VUs (0 VU = -18 dBFS), VINTAGE PLATE's VU, STEREO DELAY's
    tube, DUAL PITCH's output LEDs, TREMOLO/PANNER's L-C-R needle, the envelope of
    PHASER and TREMOLO/PANNER filling their envelope graph, and PITCH FIX's LED over
    the note it hears. The
    device list shows a tile per device: NONE and EXTERNAL (a plugin or an outboard
    insert, see *Outboard inserts*) first, then the
    tabs **FX1-8** (the premium devices, slots 1-8 only), **STD** and **CH** (the EQs and
    channel strips), in the console's order. The GATE, EQ and
    DYNAMICS screens show their model's faceplate too.
  - **METERS:** meters of the channels in groups of eight, AUX, BUS 1-8 / 9-16, DCA,
    MAIN and MATRIX, with number keys in the strips' colours (select) and M keys (mute).
    **SETTINGS** in the status bar sets PRE or POST for CHANNELS (and aux), BUSES, MAINS,
    MATRICES and DCAS, as on the console: PRE for channels and DCAs, POST for the others
    at first; PRE is after TRIM, before the EQ (as the surface's PRE). The surface's
    meters have their own (SETUP > SURFACE > METERING), as on the console. The
    segments are green below -18 dB, amber above and red at CLIP. A group's header opens
    its section's FADERS screen. Bigger consoles page.
  - **How the meters move:** every level meter (the screens, the fader strips' and the
    channel strip's LEDs, the main meter, ROUTING's tiles, the remote meter stream)
    reads the sample peak and moves the same way, as a real WING's: a peak shows in
    full at once (even a single click), the level then falls 60 dB a second (a steady
    -6 dB signal pulled out leaves the strip LEDs dark in 0.9 s), with no peak hold:
    after a drum hit the meters fall straight away and the surface's CLIP goes out at
    once. Only the METERS screen draws a peak-hold dash (red when it holds a CLIP) that
    stays 1 s before falling at the same rate. A meter's top segment (CLIP, red) lights
    from -0.1 dBFS (0 dBFS on the screens). The channel strip's LEVEL meter is after
    TRIM. The main meter and the LEDs are green below -18 dB and orange from -18 dB (no
    yellow). Gain reduction meters show the processor's own gain.
  - **ROUTING** (status-bar keys CHANNELS / SOURCES / OUTPUTS), laid out as the
    console's: pick a channel, then its source. **MAIN / ALT** (CHANNEL INPUT) choose
    which of the channel's two inputs you patch (the ALT input, WING INPUT SELECT, can
    be a recorder playback channel). **+1 AUTO** moves on to the next channel; the
    lock (red when locked, green when open) protects the patch; as on the console it
    is locked on a new console and after INITIALIZE CONSOLE. Unlocked, **NONE**
    (clears the patch) and **M** / **ST** / **M/S** (the pairing of the channel's
    source: mono, stereo or mid-side with its neighbour; the channels on it follow)
    appear; locked hides them, as on the console. Ports are named as on the WING:
    LCL 1 ..., B 1 ..., C 1 ... (on the source tiles, HOME, SELECT MAIN INPUT, the
    ALT list and the scribble strips' source tag); a name you give a source stays
    its name. The **SOURCE GROUP** selector opens the console's group menu. Sidecar's
    ports sit in the groups they stand in for: **LOCAL IN** is your audio interface's
    inputs 1-48 (LCL 1 ..., 24 tiles as on the console, more for bigger interfaces;
    tiles past the interface's inputs read n/a but can be patched), inputs 49-96 and
    97-144 are **AES50 B** / **C** (as the WING remote protocol numbers them),
    **WLIVE PLAY** the multitrack recorder's 64 playback channels (PLAY 1 ...),
    **USB PLAYER** the USB player and **OSCILLATOR** OSC 1 / 2. **BUS**, **MAIN** and
    **MATRIX** hold the strips as sources (manual 2.6), two tiles each (BUS 1L, BUS 1R
    ...) in the strip's colour with its name and icon: a touch patches the strip to the
    channel in stereo. The channel hears it one audio block later (the WING: 38
    samples); a channel sending to the bus it listens to is a feedback loop, as on the
    console. **USER SIGNAL** holds USR 1-56 (below). The other groups (AUX IN,
    AES/EBU, AES50 A, ST CONNECT, USB AUDIO, DANTE) can be selected and show n/a
    tiles, like a console without that card.
    SOURCES sets each input's source (drag over the tiles to select several: every
    change then goes to all of them, each GAIN in its group's range; the first one's
    settings show, and a touch selects one again):
    name (RENAME), colour and icon, GAIN (digital here, in the WING's range for the
    group: LOCAL IN -2.5 to +45 dB in 2.5 dB steps, AES50 B / C -3 to +45.5 dB in
    0.5 dB steps; WLIVE PLAY and the USB player have none, as on the WING), +48V
    (stored; hold the key 2 s), POL, MUTE
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
    as for other sources. The settings are saved with the show. The **USER SIGNAL**
    group (manual 2.6): USR 1-24 are **user signals**, USR 25-56 **user patches**.
    Selecting one on SOURCES shows SOURCE in place of +48V (click: the group, then
    the strip or port), POL and MUTE (no gain, as on the WING); a user signal also has
    L+R / L / R (which side of a stereo strip; L+R mixes both at half; a mono channel
    is the same either way) in the REMOTE column and TAP **PRE** (the strip's tap
    point) / **POST** (after its processing: an input channel before its pan, a bus,
    main or matrix its output) in place of RCV CUST. A user signal copies CH 1-40, AUX,
    BUS, MAIN or MTX; channels patched to it hear it a block late, outputs at once. A
    user patch copies a LOCAL IN, AES50, WLIVE PLAY or USB PLAYER port, in the same
    block: two adjacent USR sources set to STEREO or MID/SIDE make a pair from any two
    ports, each with its own gain on its port and its own polarity. OUTPUTS works
    output-first: pick an output (OUTPUT GROUP **LOCAL OUT** is the interface's
    outputs; **RECORDER** taps a signal for the USB recorder), then the signal feeding
    it from the source group: **BUS**, **MAIN**, **MATRIX** (each L / R), **MONITOR**
    (PHONES 1L / 1R, SPEAKERS 2L / 2R), **FX SEND** (FX 1L ... FX 16R: the FX
    slots' outboard insert sends), **USER SIGNAL** (USR 1-56) or any input group
    (LOCAL IN, AES50 B / C, OSCILLATOR, WLIVE PLAY, USB PLAYER: the source after its
    gain, mute and polarity, straight to the output, as the manual's "route any
    Source to any digital or analog output"). An output has one signal: patching one
    replaces what fed it. Direct outs of channels from older shows still play and show
    on their outputs (user signals now make them). Defaults: every interface input on its channel (the
    console patches its eight local inputs to channels 1-8) and MAIN 1 on LOCAL OUT
    1-2 (the console uses LOCAL OUT 7-8 and feeds buses 1-6 to 1-6).
  - **SETUP:** GENERAL (lighting, clock, console info and size, the surface's finish
    SILVER / BLACK, the console model FULL / COMPACT, OPERATION, OPTIONS),
    AUDIO (clock rate, INPUT SELECT: the global MAIN / ALT switch, and the channel grid,
    which opens the INPUT SELECT window: MAIN/ALT INPUT switches a channel, GLOBAL
    INPUT SELECT ENABLE puts it under the global switch, as HOME's INDIVIDUAL / GLOBAL;
    preferences such as MAIN LINK, DCA MUTEGROUPS, MUTEGROUP/SIP OVERRIDE, GLOBAL INPUT
    SELECT OVERRIDE (on: a GLOBAL channel's own MAIN / ALT still works; off: only the
    global switch moves it), SOURCE SOLO (CH39 / AUX7) and the TALKBACK channel, SOLO SYSTEM with SOLO MODE LIVE / STUDIO /
    SIP, PFL/AFL per strip type and the solo / select preferences (BUS SoF/SPILL ACTIVATES SOLO: Sends on Fader or a spill of a bus or main solos it), audio device,
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
    set (touch it to pick one); PHONES and SPEAKERS, each its own: SOURCE (OFF: only
    the solo bus and DIRECT IN; MAIN, MATRIX, BUS or AUX), SRC LEVEL,
    SRC MIX, DIM, PFL DIM, BAND SOLO, DIRECT IN (any channel, bus, main or matrix, at
    its own level), the EQ (MONITORS EQ: 8 bands, PAN, WIDTH, and an RTA of the path,
    PRE or POST its EQ as EQ/RTA SETTINGS' RTA SOURCE says), the DIST slider (0.1-100 m,
    linear) with INVERT and DELAY, LIMITER with its gain reduction beside the meter, and
    the level; laid out as on a WING with firmware 3.1) and
    CC EDIT (also SETUP pressed twice): the custom control layers, a function list and
    the target lists, COLOR (NONE turns the control's light off and keeps its colour,
    as COLOR > LIGHT), CLR ALL and NAME, saved with the show. The function lists
    follow WING Edit's order. Keys also have
    **CH INSERT PRE / POST ON/OFF** (a strip's INS 1 / INS 2), **FX PAR** (an FX
    slot's parameter or MIX: a press toggles a switch or steps a list; held, the
    encoder above it sets it; on a delay's TIME the key taps the tempo and flashes as
    SETUP > GENERAL > TAP TEMPO FLASH says: OFF, 8X for eight beats after the time
    changes, or ON), **OVERVIEW PAGE** (the OVERVIEW screen of a section on a layer
    key or bank) and the whole OTHER list: TALKBACK A / B (as the TALK keys), GLOBAL
    MAIN/ALT (the global input switch: the GLOBAL channels), DAW LAYER, MONITOR PHONES / SPEAKERS / PH+SPK
    (the monitoring path), MONITOR DIM / MONO / SWAP / MUTE, FADER -oo / -10 dB /
    0 dB, AUTOMIX X / Y, CHANNEL PREV / NEXT, CH / BUS / MAIN / MTX SOLO TAP (PFL /
    AFL, lit for AFL), CH / BUS / MAIN / MTX / DCA METER TAP (PRE / POST, lit for
    POST), and Sidecar's CC BANK UP / DOWN and SOF OFF. **FADER START** lights while
    its strip's fader is up (the console's fader start contact), **DAW V-POTS** on a
    key pushes a V-pot while held or turns it (or the jog wheel) a step per press,
    FADER SPILL has an **AREA** (the section it spills to), GLOBAL PAGE has SOURCE SETUP,
    OUTPUT ROUTING and USB PLAY/REC, USB PLAY/REC has TOGGLE PLAY/STOP and PLAY/PAUSE.
    Encoders: MONITORING sets the PHONES, SPEAKERS or TALKBACK level (or Sidecar's DIM
    AMOUNT), OTHER the LAMP, GLOW or PATCH brightness of SETUP > GENERAL. A control
    without a name shows its function as the console names it ("1 MUTE", "B1 SOF",
    "1 FDR", "SEND B1", "FX3 TIME", "TALK A", "SCN GO", "CC 1:7").
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
    is yellow while the DCA is soloed), FADERS (a DCA's name opens nothing, as on the console; its
    label at the foot is yellow while it's soloed; send faders of a layer, and Sends on
    Fader, show and set the send level, MUTE switching the send; AUTO RIDER in the COMP
    slot shows its square, lit while it rides), SENDS (sends on faders to one bus, main
    or matrix; with SETUP > AUDIO > SEND PAGE ACTIVATES SOLO the destination chosen is
    soloed), CONFIG (edit any layer key; see *Editing user layers*
    below; a **MIDI CC** fader sends its CC, 0-127, out of EXTERNAL MIDI CONTROL and
    follows the CC coming in; with SENDS or BUS -> BUS, **SOURCE CUSTOMIZATION** /
    **BUS CUSTOMIZATION** choose whether the send faders assigned show their source's
    or their destination's name, colour and icon, and the **S** item (a white S box,
    the selected channel's name over it) is the selected channel's send to the
    destination: on the surface it follows the selection) and MUTE GROUPS (the eight groups'
    members, a column red while its group is engaged, the group keys on the right; a
    column's header (MGRP.n over its number) opens the MUTE GROUP n window, a name
    field and the keyboard, the name changing as it's typed; touching the rest of a
    column opens the group's assign screen like a DCA's, with a SOURCE tab for the
    sources it mutes: one source group at a time as ROUTING's tiles, eight to a row,
    the groups (LOCAL IN ... USER SIGNAL) as tiles on the right).
  - **UTILITY:** system health and the event log (also on SETUP > LOG).
  - **RECORDER** (the status bar's **LIVE** card icon, red while recording): laid out
    like the WING-LIVE screens. See *Multitrack recording* below.
- **Editing user layers** (VIEW > CONFIG): as on the WING, a layer's slots are
  fixed, a slot can be blank, and the same channel can sit in more than one slot. A
  blank slot is an inactive strip on the surface: a dark scribble strip, light bar
  off, the fader doing nothing and SELECT / SOLO / MUTE unlit; on CONFIG it shows its
  number over EMPTY. Two slots holding the same channel are two faders on that
  channel: move one and the other follows. The page's slots sit at the bottom in
  groups of four (as many as the section has faders); the arrows page, one page past
  the end to add more.
  - **Assign:** pick items in the grid (CHANNELS, BUS/MAIN, DCA, SENDS, BUS -> BUS,
    FX, MIDI CC) and drag them onto a slot, or tap the slot: the item goes into
    exactly that slot, replacing what was there (several items fill that slot and
    the next ones). Nothing shifts, so you can leave gaps. An item that is already in
    the layer is added again; its other slot keeps it.
  - **Insert:** drop (or tap) near the boundary between two slots, where a gold bar
    with arrow heads appears: the items go in there and the slots after it move one
    place to the right. A blank slot at the end of the layer makes room; if the
    layer's last slot (1024) is in use the bar turns red and the drop is refused
    (LAYER FULL), so nothing is pushed off the end.
  - **Move:** drag a filled slot onto another slot. Onto a blank slot it moves there;
    onto a filled one the two swap. Dropped between two slots it is inserted there and
    its old slot turns blank.
  - **Slot menu:** right-click a slot (long-press on a touch screen). CLEAR SLOT
    blanks it; INSERT EMPTY SLOT puts a blank slot before it, moving it and the slots
    after it one place to the right. Dragging a slot off the slots blanks it too, and
    CLEAR in the top panel blanks the whole layer.
  - NEW copies the layer shown into a new user layer, blanks included; DELETE
    removes a user layer (RESTORE gives an edited layer key its own faders back).
    The layer keys other than USER 1 / USER 2 can be edited the same way. Blank
    slots and duplicates are saved with the show; older shows load unchanged.
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
  scribble strips on a four-way LCD. With SETUP > SURFACE > USE F1-F3 AS CUSTOM
  CONTROLS, F1-F3 mirror the custom control layers 1-3, as on the console: F1 gives
  the four encoders and the four upper keys of layer 1, F2 of layer 2, F3 of layer 3
  (CC EDIT assigns them; the LCD reads CC1-CC3). Shows saved by earlier versions,
  which kept F1-F3 on layers of their own, move those assignments to layers 1-3
  where those controls are free.
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
  edit the display. Holding a key with a parameter (FX PAR) gives it to the encoder
  above the key until it is let go (WING manual 4.8).

## Multitrack recording

Sidecar records up to 64 tracks and plays up to 64 tracks back into the console, like
a WING with a WING-LIVE card, and edits them in a DAW-style editor.

- **Projects (sessions).** A project is a folder, `Documents/Sidecar/Recordings/<name>/`,
  holding `<name>.ldproj` and the `Audio` folder. Recording into an unsaved project
  creates a dated one. Projects autosave every 30 s; unsaved changes are offered
  back when the project is opened next. The last project reopens at start-up.
  **FILE > Projects folder** in the editor puts new projects (and so their
  recordings) in another folder, on another drive for instance (Choose..., or back to
  the Default); existing projects stay where they are. A chosen folder whose drive
  isn't connected is not created anew: Sidecar asks for another one.
- **Recorder screen** (status-bar **LIVE** icon, or MULTI TRANSP > VIEW): the PLAYER
  (armed channels, rate, meters of the armed tracks, the red time, transport with the
  marker keys, free disk and record time left, timeline), the SESSIONS (click to open;
  NAME renames, DELETE moves a session to the trash), the MARKERS (click to jump;
  UPDATE moves the selected one to the position), the tracks (click to arm), and:
  - **RECORD ALL CHANNELS**: a track per input channel, named and coloured like it,
    recording its input port, armed, each on its own playback channels;
  - **VIRTUAL SOUNDCHECK**: each channel's ALT input becomes its track's playback
    channel and those channels go to ALT and into GLOBAL INPUT SELECT ENABLE, so the
    band's recording plays through the console and AUTO INPUT SELECT switches them;
    press again to go back to the live inputs (MAIN);
  - **EDIT** (status bar, header) opens the editor. The knobs move the position and
    the range (START, END).
  - **MODE** (next to the transport) steps PLAY, A->B and LOOP. PLAY plays on. A->B
    plays from the range's start and stops at its end. LOOP plays the range again and
    again. The mode is kept in the project.
  - **SETTINGS** (status bar): **SHOW METERS** shows or hides the meters. **AUTO
    INPUT SELECT** with **[STOP] / [PLAY] / [RECORD] INPUT** (KEEP, MAIN or ALT)
    switches the channels in GLOBAL INPUT SELECT ENABLE that are routed to or from
    the recorder (a playback channel as MAIN or ALT input, or a track recording the
    channel or its input) to MAIN or ALT when the recorder stops, plays or records
    (VIRTUAL SOUNDCHECK sets them up).
    For a virtual soundcheck: STOP and RECORD on MAIN, PLAY on ALT.
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
  - **Import:** FILE > Import or drop files from the desktop (WAV, AIFF, FLAC, MP3...),
    placed at the playhead (or where dropped), on the selected track and the ones
    below. **FILE > Import** chooses what happens to the files:
    - **COPY TO PROJECT** (the default): each file is converted to WAV at the
      project's rate in its Audio folder.
    - **KEEP IN PLACE**: the clips play the files where they are (a sample library on
      another drive, say), nothing is copied. This works for WAV, AIFF and FLAC files
      at the project's rate with one or two channels. Any other file still gets a
      converted copy, and the event log says why for each one ("pad: resampled copy:
      44.1 kHz file in a 48 kHz project"; MP3 and other compressed files get a
      decoded copy, files with more than two channels a copy of the first two).
    - **ASK EACH TIME**: every import asks KEEP IN PLACE or COPY TO PROJECT;
      *Remember my choice* makes that the setting.

    A referenced file keeps its full path in the project, so Save As to another
    folder keeps it playing. If it goes missing (a drive unplugged, a file moved),
    its clips show **OFFLINE** (hatched red, with the path) and play silence while
    the rest plays on; they play again as soon as the file is back. **FILE > Collect
    files into project** copies every file the clips play from outside the project's
    folder into its Audio folder and points the clips to the copies (the originals
    stay; undoable), so the project folder can be moved or archived on its own.
    Recordings always go into the project's Audio folder, and Sidecar never writes
    to a referenced file.
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
- **Recorder:** records the RECORDER output group (four ports, OFF on a new console
  as on the WING; ROUTING > OUTPUTS > group RECORDER taps any strip's output without moving
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
- **FADER SPILL** on any CC key: spills a DCA, the sources sending to a bus, the
  strips feeding a main, an FX slot's parameters, automix group X or Y, or the selected channel's sends onto the
  left fader section (lit while it shows; pressed again, the section's layer comes
  back). SD RECORDER and GPIO need the console's hardware and stay disabled.

## Shows, scenes and snippets

- A **show** is the file. It holds the console (channels, buses, FX devices and plugins,
  patching, DCAs, mute groups, fader banks) and all of its scenes.
- A **scene** is a console snapshot with a **recall scope**: the sections (Name,
  Routing, Input, Gate, EQ, Dynamics, Inserts, Sends, Fader, DCA/Mute), which strips,
  DCAs, mute groups and FX slots, and the console settings (monitor setup, INPUT
  SELECT, automix enables; with Routing, the sources: their name, GAIN, MUTE,
  polarity, pairing and look, as the WING's SOURCE groups); an optional **fade
  time**, and notes. A fade glides the
  continuous values: channel, aux, bus, main, matrix and DCA faders, pan and width,
  and the send levels and pans (bus, main and matrix sends). Levels move along the
  fader's travel, as a moving fader would (a fade up from -oo is heard from the start),
  pans and widths in a straight line. Everything else (mutes,
  ON keys, send modes, links, routing, processing) is set when the fade starts. Moving
  a fader or knob that is fading (on the surface, the screen, from MIDI, a DAW or a
  WING remote) takes it out of the fade: it stays where you put it while the rest
  carries on. A new recall finishes a running fade first. **Recall-safe** strips and the **GLOBAL safes** (DCAs, mute groups, FX
  slots, sections, console settings, and the **SOURCE** and **OUTPUT** groups) are
  skipped. GLOBAL's SOURCE and OUTPUT tiles (LCL, A, B, C, and CRD, PLAY and OSC for
  sources; dimly lit when only part of a group is safe) protect sources and output
  patches: a safe source keeps its settings and name, a safe output keeps the signal
  that feeds it (a strip, the monitor or an FX SEND). They belong to the console,
  like the strips' SAFE keys: saved with the show and cleared by a new one. Scenes
  saved before sources were recalled leave them alone. **PARTIAL LOAD** recalls a chosen
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
- **Files in a show.** As on the WING, a scene can also be a file: a snippet, an audio
  clip, an FX preset or a channel preset. **ADD ITEM TO SHOW** (SHOW tab, or **ADD**
  in the status bar) adds the file selected at INT or USB after the selected scene.
  An FX preset loads into the slot chosen on the FX tab, a channel preset onto the
  selected strip. A scene file comes in as a scene. **SAVE + ADD** on SNIP, FX and
  CHAN saves and adds in one go (on SNAP, SAVE already adds the scene). Recalling the
  scene loads the file: the snippet, the preset, or the clip on the USB player (patch
  the USB PLAYER sources to hear it). The list shows the type (SNIP, WAV, FX, CHAN);
  a renamed one also shows its file's name. The show keeps the snippet and presets;
  a clip stays where it is.
- **STARTUP.** A show, scene and snippet whose names start with `STARTUP` (for
  example `STARTUP_gig.lshow`, `STARTUP.lsnip`) in `Documents/Sidecar/Library/STARTUP`
  load when Sidecar starts: the show first, then the scene and the snippet. WING
  `.show` and `.snap` files work too. Hold SHIFT while Sidecar starts (the WING:
  LIBRARY) or start it with `--no-startup` to skip them.
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
- **Discovery** (UDP 2222) answers `WING?` with the console name, the model, serial
  `SIDECAR0001` and firmware `3.1-...` (Sidecar speaks the WING 3.1 parameter tree).
  The model is the WING's own: `ngc-full` for the full-size console, `wing-bk` for
  the black one (SURFACE FINISH BLACK), `wing-compact` with CONSOLE MODEL COMPACT
  (`/$syscfg/$cnscfg`, which WING Edit reads for its ACTIVE WING MODEL:
  `wing-fullsize`, `wing-bk` or `wing-compact`), so WING Edit and Mixing Station show
  the right one. The
  serial and the firmware's `sidecar` tell Sidecar apart.
- **OSC** (UDP 2223): get and set any parameter by path or by `#id`, toggles, node
  lists, node dumps and descriptions, node strings (`/ch.1.fdr=-3,mute=1`), and the
  `/*s`, `/*S` and `/*b` subscriptions (renew every 10 s). Changes from the UI, scene
  recalls and other clients are pushed to the subscribers: unlike the console, which
  serves only the last client to subscribe, Sidecar serves every client that does, so
  two OSC remotes don't take the event stream from each other. Events after a scene
  recall are paced (8 per millisecond) so a phone or tablet doesn't drop them. A slow
  Native client (a tablet on busy Wi-Fi) waits in its own queue and holds up no one
  else. Replies and descriptions are
  formatted as a WING 3.1 formats them (checked against a factory-set console): values
  with the parameter's fixed decimals and kHz notation (`100.2`, `1k50`, `1k0`),
  running times as `0:00`, descriptions such as `lin [-15.0 .. +15.0 dB], 301 steps`,
  `int [0 .. 100 %]`, `list [48000.0, 44100.0]` and `(node)`, and an integer's number
  counted from the bottom of its range (`/ch/1/col` 1 replies `"1" 0.0 0`). Like the
  console, Sidecar keeps a float set over OSC or the Native protocol (or from a
  snapshot) on the nearest of the parameter's steps, so a Q of 1.00 reads back
  0.9979702 and 100 Hz on the TAP EQ 99.68543.
- **Native protocol** (TCP 2222): node definitions and data, set/get by id, navigation,
  change notifications to every connected client, and meters over UDP: a frame every
  50 ms, as the console, with the levels as Sidecar's meters show them (the full peak
  at once, a 60 dB/s fall, no hold: WING Edit draws the stream's values as they come,
  so the console's stream carries its meters' ballistics). Channel, aux, bus, main and
  matrix meters (V1 and V2), DCAs (the loudest member PRE, after its trim, and after
  its fader), the
  monitor (solo as SPEAKERS, MON 1 = PHONES, MON 2 = SPEAKERS) and FX band gains; the
  WING gate's gain reduction on its 60 dB scale, the other models' on 20 dB. Source,
  output and RTA meters read silence.
- **Comparing with a real WING:** `tools/wing/wing_compare.py` (Python 3.8 or newer,
  nothing to install) reads every parameter from a console over OSC, read-only, and
  reports the differences. With Sidecar's WING REMOTE enabled and a WING on the same
  network: `python3 wing_compare.py discover` lists both (Sidecar answers with serial
  `SIDECAR0001`), then `python3 wing_compare.py compare WING_IP SIDECAR_IP` writes
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
  Companion WING module builds that exists in the 3.1 tree. Testers use **Mixing
  Station** with it; **WING Copilot has not been tried**, so it may not work.
- **WING Edit 3.3.4** (Behringer's desktop editor) was tested against Sidecar on both
  console models. What works: discovery (SETUP > CONNECT > RESCAN, or MANUAL IP
  ADDRESS), CONNECT, SYNCHRONIZE in both directions (MIXER -> PC reads the whole
  console; PC -> MIXER writes it back and changes nothing that was read), the ACTIVE
  WING MODEL (`wing-fullsize` / `wing-compact`, also when CONSOLE MODEL is switched
  while connected), and live control both ways: faders, mutes, names, colours and
  icons (with LINK CUSTOMIZATION TO SOURCE on, the channel shows its source's), the
  GATE / EQ / DYNAMICS models and their parameters, the FX rack (models in the
  premium slots 1-8 and standard slots 9-16, parameters, FX MIX) and the inserts (INS
  1 / INS 2 take an FX slot, and the engine runs it there). An FX slot hosting a
  plugin shows as an **EXTERNAL** effect; choosing another model for it in WING Edit
  is refused and WING Edit is told the slot is still EXT (load and remove plugins on
  Sidecar's EFFECTS screen). Choosing EXTERNAL for another slot in WING Edit makes it
  an outboard insert, and WING Edit's EXTERNAL EFFECT editor drives it: TRIM, FX MODE,
  LATENCY, the RETURN source window, and SEND shows the outputs patched to its FX SEND. Limits: WING Edit 3.3.4 knows a few hundred parameters
  of firmware newer than 3.1 (a monitor level, StageConnect source settings, a surface
  block); Sidecar ignores them, as a 3.1 console would. LOCAL OUT n and AES50 A n are
  one port in Sidecar, so a WING Edit file made on a real WING that patches them
  differently ends up with A n's patch after PC -> MIXER.
- **What maps:** every parameter of the WING 3.1 tree but one answers (79,325 of
  79,326, counting every processing model's parameters and every custom control
  mode's function nodes). 73,812 of them (93.0 %) drive the engine or the show, 89
  are Sidecar's SETUP settings, and the 5,424 Sidecar has no equivalent for answer what a factory-initialized WING answers (see below). Driving the engine: channels, aux, buses, mains
  and matrices (name, colour, icon, input and ALT patch, INPUT SELECT with `srcauto`, trim, filters, gate, EQ and
  dynamics models with all their parameters, inserts, fader, mute, pan, width, sends,
  main sends, solo, TAP WIDTH and TAP EQ, every bus's sends to the other buses, the
  matrices' `dir/in`), DCAs (with their SOLO BUS), mute groups, the FX rack, output
  routing (LOCAL OUT is A 1-8, as on ROUTING; any input group's source too), the
  USER SIGNAL group (`/io/in/USR/1-56`: the source settings and `user/grp`, `in`,
  `lr`, `tap`), the USB recorder's RECORDER ports
  (`/io/out/REC/1-4`: any bus, main, matrix or source), the sources of the LOCAL IN
  (the same ports as AES50 A), AES50 A/B/C, WING-LIVE card and USB player (`PLAY`)
  groups (name, colour, icon, gain, mute, polarity, +48V, mode, mute-group tags),
  the buses, mains and matrices as sources (`/io/in/$BUS`, `$MAIN`, `$MTX`: name,
  colour, icon, tags, read only), the oscillator (the OSC
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
  are the strips' SAFE keys, `dca`, `mute`, `fx` the GLOBAL safes, `source/LCL`,
  `A`, `B`, `C`, `CRD`, `PLAY`, `OSC` and `output/LCL`, `A`, `B`, `C` the safe
  sources and outputs; in the console's format, a `+` or a space per item, as `+ +`
  for 1 and 3; lists such as `1,3,5-8` are taken too). The EXTERNAL effect is the
  outboard insert: selecting `EXT` as an FX slot's `mdl` makes it one, its
  `egrp` / `ein` (RETURN), `emode` (FX MODE), `lat` (LATENCY) and `trim` drive it, and
  an output patched to `SEND` n (`/io/out/../grp`, `in`) carries FX slot (n+1)/2's
  send; WING Edit's EXTERNAL EFFECT editor works on it. A slot holding a plugin
  reads as EXT too, and a remote can't replace its plugin. SETUP's options are
  Sidecar's settings, both ways: METERING's taps
  (`/cfg/mtr/mtrpage`, `mtrsfc`: one setting per strip type for both), MAIN METER
  (`/cfg/mtr/mainmtr`: every source) and MAIN METER TAP,
  the GENERAL, SURFACE and SOLO SYSTEM options (`/$ctl/cfg`, `/$ctl/$globals`:
  confirm load, wheel, touch fader select and resolution, fader speed, SOF button,
  frame and mode, autoselect, user layer link, DCA spill, solo exclusive and
  follows, the brightness tiles, the time and date format, SHOW METER PAGE WHEN
  LOCKED...), the surface lock's string (`/$ctl/$stat/cnslock`, read only), MIDI REMOTE CONTROL
  (`/$ctl/midi`) and DAW (`/$ctl/daw`: enable, connection, emulation, upper CC,
  wheel, preset) and the rows of features Sidecar doesn't have (MOUSE DISABLES
  TOUCH, the OSC and TCP remote locks), which are only kept. `/$globals/clkrate` is the audio device's rate. Not mapped:
  DAW CONFIGURATION (`/$ctl/daw/config`), whose 3.1 choices (CC, MSTR) can't show
  Sidecar's MCU with extenders. The fader sections' layers (`/$ctl/layer/L`, `C`, `R`) and the custom
  controls (`/$ctl/user`) are the show's, both ways: clients select layer keys
  (`sel`), spill (`spidx`: DCA, FX, a bus's sources, a matrix's, a main's, automix X
  / Y), scroll the
  shown layer (`ofs`), name the USER keys' user layers and assign faders (`type`,
  `i`, `dst`: channels, buses, DCAs, sends, FX parameters, MIDI CCs; a default key
  becomes an edited key as on VIEW > CONFIG), and set each custom control's
  functions (`mode`) with their targets and options (the mode's own nodes, as WING
  Edit's CC EDIT sets them: `ch`, `fx` / `par`, `other` / `CCBK`, `area` / `bank`,
  `pg`, `tgt`, `btn`, `send`, `mgrp`, `dca`, `tag`, `action`, `usbpr`, MIDI `ch` /
  `cc` / `note`, `enc`), name and colour and the CC layer (`sel`); `$fname` reads
  each function's name as the console writes it ("|1 MUTE", "|DAW STOP"); the console's
  changes show to clients within a tenth of a second. As on the console, a
  layer's slots are fixed: OFF blanks a slot, and a fader assigned past the end
  leaves blank slots before it. Only the shown layer has a scroll position, and a new
  function starts at its first target. The encoding of fader assignments is an
  educated guess. What Sidecar has no equivalent for
  (the cards, the console's hardware, network and status, the other WING models'
  layers, the 4-Channel and button section modes, the input groups Sidecar doesn't
  have (AUX, AES, SC, USB, MOD, USR), a DCA's monitor bus, a channel's TAP SOLO,
  the player's song tags, the safes of source and output groups Sidecar doesn't have, bus-to-bus sends
  the engine doesn't have, ...) starts at the factory-initialized WING's value
  (`livedaw-wing/data/wing_factory.tsv`) and is remembered so clients can read back
  what they wrote (read-only ones as the WING marks them), but does nothing.
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
