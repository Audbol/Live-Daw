# Live DAW

Live DAW turns an ordinary computer into a digital mixing console for live sound,
laid out and working like a **Behringer WING**. Inputs from your audio interface
are channels on the console; they feed buses, mains and matrices, and anything can
be sent to any hardware output.

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
- **FX slots:** the WING's EXT device: each of the 16 slots hosts a **VST3, AU (macOS)
  or LV2 (Linux) plugin**, used by any strip's insert.
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
  which suits big channel counts.
- **Fader strips** follow the WING's: a colour light bar, the scribble strip (number,
  source ports or mono, icon, the name on an inverted bar), SELECT, SOLO, the meter
  block (DYN LED, a stereo six-segment input meter, GATE LED), MUTE and the fader with
  its printed scale. SELECT on the selected strip again opens its HOME screen.
- **SOF FLIP (Sends on Fader):** select a bus, main or matrix and the input faders set
  their send to it; select an input and the bus, main and matrix faders set its sends.
- Surface keys act on a press; keys with a second function act on **hold** (half a
  second, or right-click): slot on/off, VIEW for CONFIG, the shift keys.
- **Main display**, with the screen keys on its left:
  - The **status bar**: the selected channel's amber tab (number, icon, name; click for
    HOME), sample rate / clock / DSP / protection chips, the current scene (click for
    LIBRARY), **TOOLS** (UTILITY), **SETTINGS** (SETUP) and the clock.
  - The **parameter bar** under the screen: six controls that follow what the screen
    shows (drag, mouse wheel, double-click to reset).
  - **HOME** for the selected strip: the block column (INPUT, the GATE / EQ / DYN /
    INS blocks in the strip's process order with their on keys, POST INS, MAIN, SENDS,
    and the wrench for the process order and send tap), the **OVERVIEW** (INPUT,
    FILTER, LEVEL, EQ and MAIN panels), **ICON/COLOR**, **NAME**, **TAGS** and the
    **SAFE** key. Each block opens its screen with the model menu and the model's
    parameters; the EQ screen has the response graph with draggable bands and the
    band keys (LOW, 1-4, HIGH).
  - **EFFECTS:** the FX rack, slots 1-8 premium and 9-16 standard. A loaded slot shows
    its plugin, the strip using it (PRE/POST), ON and its mix; an empty slot a "+".
  - **METERS:** channels in groups of eight, AUX, BUS 1-8 / 9-16, DCA, MAIN and MATRIX,
    with number keys (select the strip) and M keys (mute). Bigger consoles page.
  - **ROUTING:** channel tiles on the left, a group of 48 sources on the right: pick a
    channel, then its source. **+1 AUTO** moves on to the next channel; the lock (red
    when locked, green when open) protects the patch. OUTPUTS patches the strips'
    outputs the same way; MONITOR sets the monitor outputs, PFL/AFL and level.
  - **SETUP:** GENERAL (console info, channel and bus count, preferences, clock,
    INIT CONSOLE), AUDIO, SURFACE, SAFES, DCA / MUTE names and PLUGINS.
  - **LIBRARY:** SHOW (name, NEW, OPEN, SAVE), SCENES and SNIPPETS as library lists
    with GO, GO+PREV, GO+NEXT, PREV and NEXT, and user BANKS.
  - **UTILITY:** system health and the event log.
- **Channel strip section** (top left): the CHANNEL STRIP DETAILS LED panel (sources,
  filters, LEVEL and GR meters, bus sends, groups) around a small screen; CHANNEL
  PROCESSING with an encoder over each of INPUT, FILTER, GATE, COMP, INSERT and MAIN
  (press: show it on the screen, again: next parameter; hold: on/off); CHANNEL
  EQUALIZER with SHIFT, four band encoders, BLEND/MIX, EQ and GAIN / WIDTH / FREQ;
  VIEW opens the shown slot on HOME.
- **USB / monitoring panel** (top right): VIEW, MONITOR LEVEL and DIM (talkback,
  phones and mono come later).
- **Custom controls** (between the center and right sections): the lower layers
  **MUTE GROUPS** (the eight mute groups) and **SHOW CNTRL** (scene back, GO, SCENES);
  the assignable upper part is empty for now.

## Shows, scenes and snippets

- A **show** is the file. It holds the console (channels, buses, FX slot plugins,
  patching, DCAs, mute groups, fader banks) and all of its scenes.
- A **scene** is a console snapshot with a **recall scope** (Name, Routing, Input,
  Gate, EQ, Dynamics, Inserts, Sends, Fader, DCA/Mute), an optional **fade time** for
  faders, pans and send levels, and notes. **Recall-safe** strips are skipped. Use
  **GO >** and **<** (or `Ctrl+G`) to step through scenes during a show.
- A **snippet** belongs to a scene and recalls only the strips and sections you choose.
  Fire it by hand, or mark it to **fire with its scene**.
- Recalling a scene or snippet never creates or destroys channels or plugins, so it
  can't cause a dropout.
- Shows from earlier Live DAW versions are converted when they load: channels stay
  channels, aux and group buses become buses, the master becomes Main 1, the gate,
  4-band EQ and compressor become the GATE, WING EQ and COMP models, and each strip's
  plugins move to FX slots on its inserts.

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

## Building

You need CMake 3.22 or newer, a C++20 compiler (MSVC 2022, Xcode 14+ or GCC 11+/Clang 14+) and
a stable Rust toolchain, 1.83 or newer ([rustup](https://rustup.rs)). The audio engine, DSP and sessions are written in Rust
(`rust/`) and linked into the JUCE app as a static library through
[Corrosion](https://github.com/corrosion-rs/corrosion). CMake fetches JUCE 8 and Corrosion for you. To use a local checkout instead, pass `-DLIVEDAW_JUCE_DIR=/path/to/JUCE`.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure   # engine, show, stability, plugin tests

cd rust && cargo test --workspace                       # Rust DSP, engine and session tests
```

### Linux

```bash
sudo apt install libasound2-dev libjack-jackd2-dev libfreetype-dev libfontconfig1-dev \
  libx11-dev libxcomposite-dev libxcursor-dev libxext-dev libxinerama-dev libxrandr-dev libxrender-dev
```

For low latency, add your user to the `audio` group and allow realtime scheduling
(`/etc/security/limits.d/audio.conf`: `@audio - rtprio 95` and `@audio - memlock unlimited`).
Use small ALSA periods or run through JACK.

### Windows (ASIO)

ASIO is built in by default. JUCE (8.0.11 and later) bundles the ASIO SDK, which
Steinberg relicensed under GPLv3 in October 2025, so there is nothing to download.
The `.exe` uses the static MSVC runtime, so it runs on any Windows 10/11 PC without
installing the VC++ redistributable.

```bat
cmake -S . -B build
cmake --build build --config Release
```

On first launch, Live DAW selects ASIO automatically if an ASIO driver is installed.
Use your interface's own ASIO driver for the lowest latency (ASIO4ALL or FlexASIO
also work). The driver's control panel (buffer size and so on) is in **Audio Setup**.

Options: `-DLIVEDAW_ASIO=OFF` builds without ASIO.
`-DLIVEDAW_ASIO_SDK_DIR=C:/path/to/asiosdk` uses a different copy of the SDK.

### Download a build

Every push builds Windows, macOS and Linux versions on GitHub Actions. Open the
repository's **Actions** tab, select the latest **CI** run and download
**LiveDAW-Windows** (or macOS/Linux) from *Artifacts*.

### macOS

CoreAudio and AU hosting work out of the box. The minimum deployment target is macOS 11.

### Sanitizer builds (GCC/Clang)

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DLIVEDAW_SANITIZE=address,undefined
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DLIVEDAW_SANITIZE=thread
# run the TSan tests with TSAN_OPTIONS=suppressions=tests/tsan.supp (the Rust part is not instrumented there)

# Rust engine under ThreadSanitizer (nightly):
cd rust && RUSTFLAGS=-Zsanitizer=thread cargo +nightly test -Zbuild-std --target x86_64-unknown-linux-gnu --workspace
```

### VST2 (optional)

VST3 hosting is built in. Steinberg no longer licenses the VST2 SDK. If you have a
copy, pass `-DLIVEDAW_VST2_SDK_DIR=/path/to/vstsdk` to enable VST2 hosting.

## Using it

1. **SETUP > AUDIO:** choose the driver (ASIO, CoreAudio, ALSA, JACK...), the device,
   the sample rate and the buffer size. **SETUP > GENERAL** sets the channel count
   (40 like a WING, or as many as your CPU allows) and adds buses.
2. **ROUTING:** pick a channel tile, then its source (right-click or shift-click sets
   the right source of a stereo channel). **OUTPUTS** gives mains, buses and matrices
   their outputs.
3. Pick layers on each fader section. **SELECT** a strip to edit it on the HOME screen
   and the channel strip section; the six knobs under the display follow the screen.
4. Load plugins into FX slots on **EFFECTS**, then assign the slots to inserts on
   **PRE INS** / **POST INS**.
5. **LIBRARY > SCENES:** **NEW** captures the console as a scene; set its recall scope,
   fade and notes. **LIBRARY > SNIPPETS** adds snippets to the selected scene. **GO**
   recalls, **GO+NEXT** steps through the show.
6. **LIBRARY > SHOW** saves and opens `.lshow` files (`Ctrl+S`, `Ctrl+O`).
   **LIBRARY > BANKS** builds your own fader banks.

## Project layout

```
rust/crates/livedaw-dsp/     Rust DSP: the WING models (filter, EQ models, 32 dynamics models), delay, biquads
rust/crates/livedaw-engine/  Rust realtime engine: WING topology, strips, sends, DCAs, mute groups, FX slots, solo, worker pool
rust/crates/livedaw-session/ Rust sessions: console state, Show > Scene > Snippet, fades, fader banks, show files, legacy import
rust/crates/livedaw-ffi/     C ABI over the engine and model tables + C++ header (include/livedaw_engine.h)
src/engine/   C++ side: facades over the Rust code, FX slot plugin hosting
src/ui/       JUCE UI: the WING surface (fader sections, main display screens, channel strip, monitor)
resources/    Barlow Condensed fonts (SIL Open Font License)
tools/wing/   Generator for the model tables (from libwing's parameter map)
tests/        Unit tests (run with ctest)
docs/         Architecture notes
```

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the signal flow and the realtime/threading design.

## Licensing note

Live DAW is built on [JUCE](https://juce.com). JUCE 8 is dual-licensed under AGPLv3
or a commercial JUCE licence. Distributing builds of this app requires one of the two. Windows builds also
include the ASIO SDK, which is GPLv3 (compatible with the AGPLv3).

The UI font, Barlow Condensed (© The Barlow Project Authors), is licensed under the SIL Open Font
License 1.1 (`resources/fonts/OFL.txt`).

Behringer and WING are trademarks of their owners. Live DAW is an independent project,
not affiliated with or endorsed by Behringer or Music Tribe.
