# Architecture

Live DAW emulates a Behringer WING: the console topology, the processing models and
the control surface with its main display. The audio engine, DSP and sessions are
Rust; audio devices, plugin hosting and the UI are C++ with JUCE.

## Signal flow

```
 hardware inputs
       │
       ▼
┌──────────── channels and aux (parallel stage 1) ───────────────────────────────────────┐
│ INPUT: trim · balance · Ø · delay ─ IN                                                 │
│ FILTER: low cut · high cut · tool (channels) ─ FILT                                    │
│ G / E / D / I in one of 24 process orders (aux: E D I) ─ taps 3, 4, 5                  │
│ ─ PFL ─ fader (+ DCAs; mute, mute groups, DCA mutes) ─ AFL ─ POST INSERT ─ POST         │
│ ─ width / pan ─► out                                                                   │
└───┬──────────────────────────────────┬──────────────────────────────┬──────────────────┘
    │ bus sends (PRE at the tap point,  │ main sends (pre: tap, else out) │ matrix sends, direct outs
    │ POST, GRP)                        │                                │
    ▼                                   │                                │
 buses 9+  (parallel stage 2) ──► sends │                                │
    ▼                                   │                                │
 buses 1-8 (parallel stage 3)           │                                │
    ▼                                   ▼                                │
 mains 1-4 (parallel stage 4: inputs + buses)                            │
    ▼                                                                    ▼
 matrices 1-8 (parallel stage 5: inputs, buses, mains + a direct input from any channel)
    │
 solo (PFL/AFL) ─► monitor (follows Main 1 when nothing is soloed) ─► monitor outputs
 every strip ─► its hardware outputs ─► output protection (safety clip)
```

- Buses, mains and matrices run the same strip code with the EQ / DYN / INS blocks
  (`"EDI"`), the 8-band bus EQ table and an output delay instead of the input delay.
- Bus sends are stored in a paged `SendBank`, so buses can be added beyond the WING's
  16. Buses 9 and up can feed buses 1-8 (`BUS_TO_BUS_TARGETS`).
- A matrix's direct input takes any channel's PFL or output; inverted, it builds a
  mix-minus.
- The post insert can run an FX slot or join automix group X or Y. The audio thread
  computes the gain sharing between stages 1 and 2 (`engine.rs`, `automix`).

## Processing models

`rust/crates/livedaw-dsp/src/wing/models_gen.rs` holds the WING's model tables: the 32
dynamics models, the channel and bus EQ models, the filter tools, the 24 process
orders and the tap points. `tools/wing/gen_models.py` generates the file from libwing's
parameter map (`propmap.jsonl`, WING firmware 3.1). Each model is a list of parameters
(id, kind, range, unit, choices, default). A block stores its values as a flat array
of atomics in table order (`DynSlotParams.values`, `EqBlockParams.values`), and
choosing a model loads its defaults. The DSP recreates each model from its controls,
ranges and behaviour; nothing is circuit-modelled. The C++ side and the UI read the
same tables through `ld_model_*` (`livedaw-ffi/src/models.rs`, `src/engine/Models.*`).

## Rust engine and C++ host

| Layer | Language | Where |
|---|---|---|
| DSP (filters, EQ models, dynamics models, delay) | Rust | `rust/crates/livedaw-dsp` |
| Realtime engine (WING topology, strips, sends, DCAs, mute groups, FX slots, automix, solo, monitor, output protection, worker pool, deferred frees) | Rust | `rust/crates/livedaw-engine` |
| Sessions (console state, Show > Scene > Snippet, fades, fader banks, show files, legacy import) | Rust | `rust/crates/livedaw-session` |
| C ABI | Rust + C++ header | `rust/crates/livedaw-ffi`, `include/livedaw_engine.h` |
| Audio devices (ASIO, CoreAudio, ALSA, JACK, WASAPI), plugin hosting (VST3/AU/LV2), UI | C++ / JUCE | `src/` |

`src/engine/MixerEngine`, `Strip`, `FxSlots`, `Models`, `Show` and `LayerSet` are thin
C++ facades over the Rust code. `Strip` and `FxSlots` are stateless views: names,
colours and slot state live in the engine, and `MixerEngine::syncStrips()` rebuilds the
views after a show load changes the console from Rust.

- **Shared parameters.** Strip parameters, meters, sends, DCAs, FX slot controls and
  engine controls are `#[repr(C)]` blocks of atomics owned by Rust. C++ sees the same
  memory as `std::atomic<float/bool/int32_t>` structs, and the UI writes them directly
  with no FFI call. Both sides describe the layout; `ld::verifyLayout()` compares the
  two at startup and in the tests (ABI version 3), and the app refuses to run if they
  differ.
- **FX slots.** The 16 slots are the WING's FX rack, holding VST3/AU/LV2 plugins (the
  "EXT" device). Plugin instances stay in C++ (`FxSlots`). Each loaded slot registers
  `process` and `release` callbacks with the engine (`ld_fx_set`). Rust calls `process`
  from whichever thread runs the strip whose PRE or POST insert uses the slot, keeps a
  dry copy for the slot's mix, and auto-bypasses the slot if the callback reports an
  error (a C++ exception is caught in the callback) or returns NaN/inf. A slot runs for
  one strip per block: a second insert on the same slot passes its signal through and
  the slot is flagged as double assigned. The engine measures each slot's load. A
  replaced plugin's `release` runs on the message thread once no audio callback can
  still reach it.
- **Plugins in sessions.** The session reaches the plugins through a *plugin bridge*
  implemented by `Show`: capture a slot (description XML + base64 state), restore one
  (state only, or by creating the plugin on show load) and clear one.
- **Panics.** Every FFI entry point and every worker task runs under `catch_unwind`.
  A panic in the audio path outputs silence for that block and increments
  `engineFaults`. It never unwinds into C++.

## Threads

| Thread | Does |
|---|---|
| Message (UI) thread | Adds/removes strips, loads plugins, writes parameters, collects garbage (`ld_engine_collect`, 10 Hz), runs scene fades (`ld_show_tick`, 60 Hz). |
| Audio thread | `ld_engine_process`. Never allocates, locks or frees. |
| DSP workers | Realtime-priority JUCE threads lent to the Rust pool (`ld_engine_run_worker`). They process strips in parallel with the audio thread. |

### Parameters
The UI writes the shared atomics and the audio thread snapshots them once per block.
Gains are ramped across the block, and filter and EQ coefficients are smoothed.

### Structure changes
The engine keeps an immutable topology (`Arc<Strip>` lists) behind an `AtomicPtr`.
Adding or removing a strip builds a new topology on the message thread and swaps the
pointer. The old topology, removed strips and released plugins go to an epoch-based
`Garbage` list. It frees them on the message thread once the audio callback has
completed a full cycle, so no running callback can still hold a pointer to them.

### Parallel processing
`WorkerPool::parallel_for` is a fork/join with no allocation. The audio thread
publishes a job (generation, task count and next index packed into one 64-bit atomic),
wakes the workers (spin, then park/unpark) and helps with the work itself. Each block
has five parallel stages: channels and aux, buses 9+, buses 1-8, mains, matrices.
Automix, the monitor and writing the outputs run on the audio thread.

## Idle channels

An input with no hardware source patched and no inserts takes a fast path: its
buffers stay zeroed, its chain is skipped and the bus, main and matrix sums skip it
too. Its DSP state is reset when it becomes active again, so it can't click. A big
console only costs CPU for the channels in use.

## Hardware channels
JUCE delivers only the *active* device channels, packed together. The engine maps
hardware channel numbers to active-buffer indices when the device starts. Strips
always refer to hardware channel numbers, so enabling or disabling channels in the
audio setup doesn't move your patching.

## Shows, scenes and snippets

`livedaw-session/src/console.rs` serialises strips **per section** (Name, Routing,
Input, Gate, EQ, Dynamics, Inserts, Sends, Fader, DCA/Mute). Model parameters are saved
by id with the model's token, so a show doesn't depend on table order. Every captured
strip records which sections it holds, and `apply()` only touches the intersection of
*captured* and *requested* sections. Strips are referenced by a stable `StripRef`
(kind + id), so references survive removing channels and reloading the show. Loading
a console runs in two passes (structure first, then state), so sends to buses added by
the same load are kept.

`livedaw-session/src/show.rs` (show format version 3):

```
Show
 ├─ Console     full structure, DCAs, mute groups, FX slots (via the bridge)
 ├─ Layers      section widths, user banks, selected banks
 └─ Scene[]     State (all sections) + recall scope + fade time + notes
     └─ Snippet[]   State (chosen strips × chosen sections) + "fire with scene"
```

- Scene recall applies the state within the scene's scope and skips recall-safe strips.
  It then applies the scene's auto-fire snippets.
- Snippets ignore recall safes, which makes them the tool for deliberately changing a
  protected channel.
- Fades collect fade targets (fader, pan and send levels) instead of setting them, and
  `ld_show_tick` interpolates the atomics. A fade target holds its strip (`Arc`), so
  removing a strip mid-fade is safe. A new recall finishes any fade still running.
- FX slots are recalled as *plugin state only*, and only when the same plugin is
  already in that slot. Recalls never instantiate plugins.
- Show files are XML. Saving writes a temporary file, verifies it, renames it over the
  show and keeps the previous version as `.bak`; loading falls back to the `.bak` if
  the show is damaged.
- `livedaw-session/src/legacy.rs` converts shows from earlier versions (v1 sessions
  and v2 shows) on load: inputs become channels, aux and group buses become buses, the
  master becomes Main 1, the gate / 4-band EQ / compressor become the GATE, WING EQ and
  COMP models, and each strip's first two plugins move to FX slots on its inserts
  (any more are dropped with a warning).

## Fader banks

`livedaw-session/src/layers.rs` (`LayerSet`, owned by the show; C++ view in
`src/engine/Layers.*`):

- Three sections like the WING's surface: left 12 faders, center 8, right 4. A width
  of 0 means Auto: the UI reports how many faders fit (`setFittedWidth`).
- Each section has the WING's seven layer keys, computed from the console whenever
  they're asked for so they can't go stale: left `1-12` `13-24` `25-36`
  `37-40 AUX IN` `BUS MASTER` `USER 1` `USER 2`; center `DCA` `MAIN MATRIX` `AUX IN`
  `INPUTS` `BUS MASTER` `USER 1` `USER 2`; right `MAIN MATRIX` `DCA` `INPUTS` `AUX IN`
  `BUS MASTER` `USER 1` `USER 2`. A layer longer than its section stays one key; the
  surface scrolls it with the shift keys. Ids are stable, and ids of older shows
  (`bus-2`, `dca-2`, `all`, ...) resolve to the key that holds those faders now.
- A bank holds `FaderRef`s: a strip or a DCA. User banks are saved with the show; the
  first two are on the USER 1 / USER 2 keys, further ones get keys of their own under
  the shift keys. A reference to a removed strip is skipped.

## The UI

`src/ui/` draws the WING surface: three fader sections (`FaderSection.*`,
`FaderStrip.*`) with VIEW, the layer and shift keys, colour bars and scribble strips,
SELECT / SOLO / MUTE, LED meters and scaled faders; the custom controls section
between the center and right sections (mute groups, show control); the channel strip
section with its LED panel and screen (top left); the USB / monitoring panel (top
right, `SurfaceSections.*`); and the main display with the screen keys. Surface keys
(`SurfaceKey` in `WingWidgets.h`) know press, hold (500 ms or right-click) and
blinking LEDs.

- **Look.** `ConsoleLookAndFeel` (`LookAndFeel.*`) holds the WING palette (black
  screen, grey panels with header bars, amber channel tab, yellow ON keys, lime knob
  rings, cyan selections) and uses Barlow Condensed (SIL OFL, `resources/fonts/`,
  embedded with `juce_add_binary_data`). `Icons.*` draws the screen icons as vectors;
  `WingWidgets.h` has the screen keys, ON chips and segmented keys.
- **Main display** (`MainDisplay.*`): the status bar (channel tab, sample rate /
  clock / DSP / protection chips, scene, TOOLS, SETTINGS, clock), the screen, and the
  parameter bar with six controls that follow the screen (`DisplayPage::getKnobs`).
- **Screens:** HOME (`HomePage.*`: the block column, OVERVIEW, ICON/COLOR, NAME, TAGS;
  `HomeScreens.*`: INPUT, FILTER, GATE, EQ, DYNAMICS, PRE/POST INSERT, MAIN, SENDS),
  EFFECTS, METERS, ROUTING, SETUP, LIBRARY (`ShowPanel.*` for scenes and snippets) and
  UTILITY.
- Views poll the shared atomics on timers (4-25 Hz); meters read `MeterBank`, the one
  reader of the engine's "peak since last read" meters.

## Stability mechanisms

- The engine checks every strip's output: NaN/Inf is silenced, the strip's DSP reset
  and `dspFaults` incremented, before it can reach a bus.
- FX slots: errors and non-finite output restore the dry signal and bypass the slot
  until the fault is cleared (EFFECTS, UTILITY).
- Output protection clips every hardware output at the ceiling and zeroes any
  non-finite block (`outputClipEvents` counts the blocks it engaged).
- `OutOfProcessScanner` / `PluginScannerWorker` scan plugins in a child copy of the app
  (`ChildProcessCoordinator`). If the child dies, that plugin is marked failed and a new
  scanner is started.
- `PluginHost::createInstance` writes a "plugin being loaded" marker. On the next start,
  `checkForCrashDuringPluginLoad()` blacklists the plugin if the marker is still there.
- `DeviceWatchdog` (Main.cpp) watches the device manager and the engine's
  last-callback timestamp. It reopens the device with a back-off.
- A `running.lock` file marks an unclean exit. The next start loads the autosave
  without plugins (safe mode) and autosaves to a separate file until the user reloads
  the plugins.

## Known limitations / roadmap
- Plugins run inside the mixer process. Bad output and exceptions are contained, but
  a plugin that segfaults takes the app down (safe mode then recovers the show).
  Hosting each plugin in a sandbox process is the next step for full isolation.
- The WING's own FX algorithms (reverbs, delays, channel FX) aren't recreated; FX
  slots host plugins.
- No plugin delay compensation.
- Plugins always run in stereo. A mono strip feeds both plugin inputs and takes the
  left output back.
- Solo safe and the monitor A/B bus choice are saved with the show but not used by the
  engine yet; there is one monitor output pair.
- Not yet: the alternate (ALT) channel input, assignable custom controls (the
  section's upper part and USER layer are empty), the 4-Channel section, talkback,
  OSC/MIDI remote control and recording.
