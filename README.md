# Live DAW

Live DAW turns an ordinary computer into a digital mixing console for live sound.
Inputs from your audio interface are channels on a console. Each channel has
processing, it can be routed to buses, and it can be sent to any hardware output.

## Features

- **Audio drivers:** ASIO and WASAPI on Windows, CoreAudio on macOS, ALSA and JACK on Linux.
- **Any number of channels.** You pick the channel count and CPU is the only limit.
  Channels and buses are processed in parallel on a pool of realtime worker threads.
- **Every strip (channels, buses and master) has:**
  - input trim, polarity invert and a 12 dB/oct high-pass filter
  - a **noise gate** (threshold, range, attack, hold, release, with hysteresis)
  - a **4-band parametric EQ**. The low and high bands switch between shelf, bell and cut.
    The two mid bands are fully parametric. You can edit it graphically or with knobs.
  - a **compressor** (threshold, ratio, soft knee, attack, release, makeup)
  - **8 plugin insert slots** for VST3, AU (macOS) and LV2 (Linux), plus VST2 if you
    supply the legacy SDK yourself
  - mute, fader, pan/balance, solo, and peak and gain-reduction meters
- **Buses:** up to 32 aux buses (monitor mixes, FX sends; mono or stereo) and group
  buses (subgroups routed to the master). Channel sends can be pre- or post-fader.
- **Solo/monitor bus:** PFL or AFL solo replaces the master on a dedicated pair of
  monitor outputs. The main mix is never interrupted.
- **Routing to any hardware output** for the master, each bus, each channel's direct
  out (for multitrack recording) and the monitor.
- **Sessions:** the whole console is saved as a `.livedaw` XML file, including plugin
  state. It's also autosaved every 30 s and restored at startup.
- Click-free parameter changes. Gains, sends and EQ moves are smoothed.

## Building

You need CMake 3.22 or newer and a C++20 compiler: MSVC 2022, Xcode 14+ or GCC 11+/Clang 14+.
CMake fetches JUCE 8 for you. To use a local checkout instead, pass `-DLIVEDAW_JUCE_DIR=/path/to/JUCE`.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure   # DSP, worker pool and engine tests
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

### VST2 (optional)

VST3 hosting is built in. Steinberg no longer licenses the VST2 SDK. If you have a
copy, pass `-DLIVEDAW_VST2_SDK_DIR=/path/to/vstsdk` to enable VST2 hosting.

## Using it

1. **Audio Setup:** choose the driver (ASIO, CoreAudio, ALSA, JACK…), the device,
   the sample rate and the buffer size. Enable the input and output channels you need.
2. **Channels:** set the channel count with `-` / `+`, or double-click the number and type it.
   Channel *N* is patched to hardware input *N*. Click a strip's input button to repatch it
   or to make it a stereo pair.
3. **SEL** on a strip opens the selected-channel panel at the bottom, with the input stage,
   gate, EQ, compressor, inserts, sends and outputs.
4. **+ Aux** / **+ Group** add buses. Give an aux a hardware output (wedge or IEM) in its
   output section. Then set the channel sends in each channel's *Sends* section.
5. Select the **Master** to set the main outputs and the monitor/solo outputs, and to
   switch between PFL and AFL.
6. **Plugins → Scan** finds installed VST3/AU/LV2 plugins. Click an insert slot to load one.

Right-click a strip to rename it, change its colour or remove it.

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
