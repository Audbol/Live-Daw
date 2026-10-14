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
- **Buses:** up to 32 aux buses (monitor mixes, FX) and group buses (subgroups), with
  pre- or post-fader sends. PFL/AFL solo goes to a dedicated monitor bus. Every bus and
  each channel's direct out can be routed to any hardware output.
- **Layers (X32/Wing style):** a layer selector beside the faders, with the master
  always visible on the right.
  - **Default layers** are generated from the console and always cover every strip:
    input banks (`Ch 1-16`, `Ch 17-32`, …), aux banks, group banks and `All`. They
    follow the console as channels and buses are added or removed. The bank size
    (8–48) can be changed.
  - **User layers** hold any strips on the board (inputs, auxes, groups, master) in any
    order. Build them in the **Layers** tab, or right-click a strip and choose
    *User layers*.
  - `Ctrl/Cmd + 1..9` selects layers. Layers are saved with the show. Only the visible
    layer's strips are drawn, so very large consoles stay responsive.
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

You need CMake 3.22 or newer and a C++20 compiler: MSVC 2022, Xcode 14+ or GCC 11+/Clang 14+.
CMake fetches JUCE 8 for you. To use a local checkout instead, pass `-DLIVEDAW_JUCE_DIR=/path/to/JUCE`.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure   # DSP, engine, show, stability, plugin tests
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

The ASIO SDK isn't bundled with this repository. Download it from Steinberg
(<https://www.steinberg.net/developers/>) and point CMake at the extracted folder:

```bat
cmake -S . -B build -DLIVEDAW_ASIO_SDK_DIR=C:/SDKs/asiosdk_2.3.3
```

If you don't set it, the app builds with WASAPI only and CMake prints a warning.

### macOS

CoreAudio and AU hosting work out of the box. The minimum deployment target is macOS 11.

### Sanitizer builds (GCC/Clang)

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DLIVEDAW_SANITIZE=address,undefined
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DLIVEDAW_SANITIZE=thread
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
src/dsp/      Pure C++ DSP (no JUCE): biquads, 4-band EQ, gate, compressor
src/engine/   Mixer engine, strips, plugin chains, realtime worker pool, sessions
src/ui/       JUCE UI: console strips, selected-channel panel, EQ editor, meters
tests/        Unit tests (run with ctest)
docs/         Architecture notes
```

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the signal flow and the realtime/threading design.

## Licensing note

Live DAW is built on [JUCE](https://juce.com). JUCE 8 is dual-licensed under AGPLv3
or a commercial JUCE licence. Distributing builds of this app requires one of the two.
