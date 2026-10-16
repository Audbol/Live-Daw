# Live DAW

Live DAW turns an ordinary computer into a digital mixing console for live sound.
Inputs from your audio interface are channels on a console. Each channel has
processing, it can be routed to buses, and it can be sent to any hardware output.

## Features

- **Audio drivers:** ASIO and WASAPI on Windows, CoreAudio on macOS, ALSA and JACK on Linux.
- **Any number of channels.** You pick the channel count and CPU is the only limit.
  Channels and buses are processed in parallel on a pool of realtime worker threads.
- **Each channel can be mono or stereo.** Switch with **MO/ST** on the strip and
  patch the left and right inputs independently.
- **Every strip (channels, buses and master) has:** trim, polarity, high-pass filter,
  **noise gate**, **4-band parametric EQ**, **compressor**, **8 plugin insert slots**
  (VST3, AU on macOS, LV2 on Linux), mute, fader, pan, solo and meters.
- **Processing handles on the mixer page.** You don't have to open the channel to work with it:
  - **Inserts:** click an empty slot to load a plugin, click a loaded one to open its
    editor, right-click to bypass, replace or remove. Faulted plugins show in red.
  - **Gate:** `G` switches it on and off. The bar shows the input level against the
    threshold, and dragging it sets the threshold.
  - **EQ:** a mini curve with draggable band handles (the mouse wheel sets Q), plus EQ
    and HPF on/off toggles.
  - **Compressor:** `C` switches it on and off. The bar shows gain reduction, and dragging
    it sets the threshold.
  - The **Channel** tab below the mixer has every parameter for the selected strip.
- **Buses:** any number of aux buses (monitor mixes, FX) and group buses (subgroups), limited by CPU like channels, with
  pre- or post-fader sends. PFL/AFL solo goes to a dedicated monitor bus. Every bus and
  each channel's direct out can be routed to any hardware output.
- **Layers (X32/Wing style):** a layer selector beside the faders, with the master
  always visible on the right.
  - **Default layers** are generated from the console and always cover every strip:
    input banks, aux banks, group banks and `All`. They follow the console as
    channels and buses are added or removed. The bank size is **Auto** by default:
    each bank holds as many strips as fit the window (e.g. 35 on a 3440-pixel
    ultrawide), re-banks when you resize while staying on the same channels, and
    stretches strips to fill the width. Fixed sizes (8–64) are available too.
  - **User layers** hold any strips on the board (inputs, auxes, groups, master) in any
    order. Build them in the **Layers** tab, or right-click a strip and choose
    *User layers*.
  - `Ctrl/Cmd + 1..9` selects layers, and the layer list scrolls. Layers are saved with
    the show. Only the strips actually on screen get UI components, and meters redraw
    only when they change, so even the `All` layer of a 4096-channel console stays
    responsive. The mouse wheel scrolls the mixer; it never moves a fader or pan.
- **Fully resizable interface:**
  - Drag the dividers to size the bottom panel and the layer list (double-click one to
    reset it).
  - Every panel stretches to fill the window.
  - Strips grow with the window and shed their least important parts (inserts, then
    the mini EQ, then the dynamics handles) on small screens, so the fader is always
    usable.
  - **View** sets the interface scale (75–200%) and strip width (Narrow, Normal, Wide,
    Extra wide), and can reset the layout. Window position and size, panel sizes and
    scale are remembered.
- **Unused channels are free:** a channel with no input patched and no plugins skips
  its DSP entirely, so a big console only costs CPU for the channels you use.
- **Show > Scene > Snippet:**
  - A **show** is the file. It holds the console setup (channels, buses, plugins,
    patching) and all of its scenes.
  - A **scene** is a console snapshot with a **recall scope** (which sections it
    recalls: Name, Routing, Preamp, Gate, EQ, Comp, Inserts, Sends, Fader), an optional
    **fade time** for faders, pans and sends, and notes. **Recall-safe** channels are
    skipped. Use **GO >** and **<** in the toolbar to step through scenes during a show.
  - A **snippet** belongs to a scene and recalls only the strips and sections you choose.
    You can fire it by hand, or mark it to **fire with its scene**.
  - Recalling a scene or snippet never creates or destroys channels or plugins, so it
    can't cause a dropout.

## Stability

Live sound gets no second take, so stability is designed in at every layer:

| Layer | Protection |
|---|---|
| Audio thread | No allocations, locks or file I/O. Denormals are flushed. Channel/bus/plugin changes are lock-free swaps with deferred deletion. |
| Plugins | A plugin that outputs NaN/Inf or throws is **auto-bypassed on the spot**. Its input passes through, and the slot is flagged red. Per-plugin CPU is measured. |
| Plugin scanning | Scans run **in a separate process**, so a plugin that crashes during a scan only kills the scanner. |
| Plugin loading | The plugin being loaded is recorded on disk first. If it crashes the app, it's blacklisted on the next start. |
| Strips | Every strip checks its own output. NaN/Inf is silenced and the DSP reset before it can poison a bus. |
| Outputs | A brick-wall **safety clip** (adjustable ceiling) protects the amplifiers and speakers. |
| Device | A **watchdog** reopens the audio device if it errors, disappears or stops calling back. Retries back off. |
| Crashes | The show is autosaved every 30 s. After an unclean exit, the app restarts in **safe mode** (console restored without plugins) and offers to reload the plugins. |
| Files | Show files are written atomically (temp file, verify, rename) with a `.bak` of the previous version. A corrupt file falls back to the backup. |
| Shutdown | SIGTERM/SIGINT quit cleanly (state saved), so OS shutdowns aren't mistaken for crashes. |
| Testing | Unit, integration (real LV2 plugins) and multi-threaded stress tests run under AddressSanitizer, UndefinedBehaviorSanitizer and ThreadSanitizer in CI. |

The **System** tab shows device health, xruns, DSP load, fault counters and an event log.

## Building

You need CMake 3.22 or newer, a C++20 compiler (MSVC 2022, Xcode 14+ or GCC 11+/Clang 14+) and
a stable Rust toolchain, 1.83 or newer ([rustup](https://rustup.rs)). The audio engine and DSP are written in Rust
(`rust/`) and linked into the JUCE app as a static library through
[Corrosion](https://github.com/corrosion-rs/corrosion). CMake fetches JUCE 8 and Corrosion for you. To use a local checkout instead, pass `-DLIVEDAW_JUCE_DIR=/path/to/JUCE`.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure   # engine, show, stability, plugin tests

cd rust && cargo test --workspace                       # Rust DSP + engine tests
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

1. **Audio Setup:** choose the driver (ASIO, CoreAudio, ALSA, JACK…), the device,
   the sample rate and the buffer size.
2. **Channels:** set the count with `-` / `+`, or double-click the number and type it.
   On each strip, the input button patches the channel and **MO/ST** switches it between
   mono and stereo.
3. Pick a layer on the left of the faders (or `Ctrl+1..9`). Build your own layers in
   the **Layers** tab: **+ New**, then double-click strips on the board to add them.
4. Work directly on the strips (insert slots, gate, mini EQ, compressor, pan, fader).
   **SEL** opens every parameter in the **Channel** tab.
5. **+ Aux** / **+ Group** add buses. Set sends in the *Sends* section of each channel,
   and give buses hardware outputs.
6. **Show tab:**
   - **+ New** captures the current console as a scene.
   - Set its recall scope, fade and notes.
   - Add snippets to it: choose the strips and sections, and optionally *Fire with scene*.
   - **GO** (or double-click) recalls.
7. Right-click a strip to rename it, recolour it, make it **recall safe** or remove it.
8. **Show > Save** writes a `.lshow` file. **Plugins** opens the scanner (it runs out of process).

## Project layout

```
rust/crates/livedaw-dsp/     Rust DSP: biquads, 4-band EQ, gate, compressor
rust/crates/livedaw-engine/  Rust realtime engine: strips, routing, sends, solo, worker pool, deferred frees
rust/crates/livedaw-ffi/     C ABI over the engine + C++ header (include/livedaw_engine.h)
src/engine/   C++ side: engine facade, plugin hosting, shows/scenes/snippets, layers
src/ui/       JUCE UI: console strips, selected-channel panel, EQ editor, meters
tests/        Unit tests (run with ctest)
docs/         Architecture notes
```

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the signal flow and the realtime/threading design.

## Licensing note

Live DAW is built on [JUCE](https://juce.com). JUCE 8 is dual-licensed under AGPLv3
or a commercial JUCE licence. Distributing builds of this app requires one of the two. Windows builds also
include the ASIO SDK, which is GPLv3 (compatible with the AGPLv3).
