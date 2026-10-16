# Architecture

## Signal flow

```
 hardware inputs
       │
       ▼
┌──────────────── input strip (× N, processed in parallel) ────────────────┐
│ trim/Ø → HPF → gate → 4-band EQ → compressor → inserts ─┬─ mute ─┬─ fader → pan ──► main L/R
│                                                         │        │
│                                                     PFL tap   pre-fader tap   post-fader tap
└─────────────────────────────────────────────────────────┼────────┼──────────────┼──────────┘
                                                          │        └──► sends ◄───┘
                                                          │              │
                         ┌────────────── aux / group buses (parallel) ◄──┘
                         │  same chain as an input strip
                         │  aux → hardware outputs     group → master
                         ▼
                      master  (sum of assigned channels + groups, same chain) → main outputs
                         │
 solo (PFL/AFL) ─────────┴──► monitor bus (follows master when nothing is soloed) → monitor outputs
```

Every strip type uses the same strip code. The engine decides what feeds a strip and
where its output goes.

## Rust engine and C++ host

Live DAW is a hybrid:

| Layer | Language | Where |
|---|---|---|
| DSP (biquads, EQ, gate, compressor) | Rust | `rust/crates/livedaw-dsp` |
| Realtime engine (strips, routing, sends, solo, monitor, output protection, worker pool, deferred frees) | Rust | `rust/crates/livedaw-engine` |
| C ABI | Rust + C++ header | `rust/crates/livedaw-ffi`, `include/livedaw_engine.h` |
| Audio devices (ASIO, CoreAudio, ALSA, JACK, WASAPI), plugin hosting (VST3/AU/LV2), UI, shows/scenes/snippets, layers | C++ / JUCE | `src/` |

`src/engine/MixerEngine`, `Strip` and `PluginChain` are thin C++ facades over the
Rust engine with the API the rest of the app uses.

* **Shared parameters.** Strip parameters, meters, sends and engine controls are
  `#[repr(C)]` blocks of atomics owned by Rust. C++ sees the same memory as
  `std::atomic<float/bool/int32_t>` structs (identical size and layout to Rust's
  `AtomicU32`/`AtomicBool`/`AtomicI32`). The UI writes them directly with no FFI call.
  Both sides describe the layout. `ld::verifyLayout()` compares the two at startup and in
  the tests, and the app refuses to run if they differ.
* **Plugins.** Plugin instances stay in C++. Each loaded insert slot registers a
  `process` callback with the engine. Rust calls it from the audio or worker thread,
  keeps a dry copy, and auto-bypasses the slot if the callback reports an error (a
  C++ exception is caught in the callback) or returns NaN/inf. A replaced plugin's
  `release` callback runs on the message thread once no audio callback can still
  reach it.
* **Panics.** Every FFI entry point and every worker task runs under `catch_unwind`.
  A panic in the audio path outputs silence for that block and increments
  `engineFaults`. It never unwinds into C++.

## Threads

| Thread | Does |
|---|---|
| Message (UI) thread | Adds/removes strips, loads plugins, writes parameters, collects garbage (`ld_engine_collect`, 10 Hz). |
| Audio thread | `ld_engine_process`. Never allocates, locks or frees. |
| DSP workers | Realtime-priority JUCE threads lent to the Rust pool (`ld_engine_run_worker`). They process strips in parallel with the audio thread. |

### Parameters
The UI writes the shared atomics and the audio thread snapshots them once per block.
Gains are ramped linearly across the block. EQ frequency, gain and Q are smoothed, and
coefficients are recomputed every 32 samples.

### Structure changes
The engine keeps an immutable topology (`Arc<Strip>` lists) behind an `AtomicPtr`.
Adding or removing a strip builds a new topology on the message thread and swaps the
pointer. The old topology, removed strips and released plugins go to an epoch-based
`Garbage` list. It frees them on the message thread once the audio callback has
completed a full cycle, so no running callback can still hold a pointer to them.

### Parallel processing
`WorkerPool::parallel_for` is a fork/join with no allocation. The audio thread
publishes a job (generation, task count and next index packed into one 64-bit atomic),
wakes the workers (spin, then park/unpark) and helps with the work itself. Workers claim
indices with CAS. Each block has two parallel stages:

1. all input strips (read hardware → chain → output stage)
2. all buses (sum sends → chain → output stage)

Summing into the master, building the monitor bus and writing the outputs is cheap
and runs on the audio thread. Idle strips (no input patched, nothing to process) take
a fast path, so thousands of unused channels cost almost nothing.

## Hardware channels
JUCE delivers only the *active* device channels, packed together. The engine maps
hardware channel numbers to active-buffer indices when the device starts. Strips
always refer to hardware channel numbers, so enabling or disabling channels in Audio
Setup doesn't move your patching.

## Shows, scenes and snippets

`src/engine/ConsoleState.*` serialises strips **per section** (Name, Routing, Preamp,
Gate, EQ, Comp, Inserts, Sends, Fader). Every captured strip records which sections
it holds, and `apply()` only touches the intersection of *captured* and *requested*
sections. Strips are referenced by a stable `StripRef` (kind + id). Input ids are
saved and restored, so references survive removing channels and reloading the show.

`src/engine/Show.*`:

```
Show
 ├─ Console     full structure (+ plugin instances), loaded with loadConsole()
 └─ Scene[]     State (all sections) + recall scope + fade time + notes
     └─ Snippet[]   State (chosen strips × chosen sections) + "fire with scene"
```

- Scene recall applies the state within the scene's scope and skips recall-safe strips.
  It then applies the scene's auto-fire snippets.
- Snippets ignore recall safes, which makes them the tool for deliberately changing a
  protected channel.
- Fades collect `FadeTarget`s (fader, pan and send levels) instead of setting them, and
  a 60 Hz message-thread timer interpolates the atomics. A new recall finishes any fade
  still running.
- Inserts are recalled as *plugin state only*, and only when the same plugin is already
  in that slot. Recalls never instantiate plugins.

## Layers

`src/engine/Layers.*` (`LayerSet`, owned by the `Show`):

- `getDefaultLayers()` is computed from the console every time, so default layers can't
  go stale. Inputs, auxes and groups are chunked by the bank size, and the ids are
  stable (`in-2`, `aux-1`, `groups-1`, `all`) so the selected layer survives topology
  changes.
- User layers store ordered `StripRef`s and are saved in the show (`<Layers>`). A
  reference to a removed strip is skipped by `resolve()`.
- Bank size 0 means Auto. The UI reports how many strips fit
  (`setFittedBankSize`), and on a resize it re-selects the bank holding the first
  visible strip (`findDefaultLayerContaining`).
- The mixer view is virtualised: components exist only for strips inside the
  viewport (plus one strip each side), and meters and handles repaint only when their
  displayed value changes. The master strip is pinned on the right.

## Idle channels

An input strip with no hardware input connected and no inserts takes a fast path:
its buffers are zeroed and the chain is skipped, and the bus and master summing skip
it too. Its DSP state is reset when it becomes active again, so it can't click.

## Stability mechanisms

- `PluginChain::process` wraps each plugin in `try/catch` and checks its output with
  `dsp::allFinite`. On failure it keeps the dry signal, then sets `bypass` and `faulted`.
- `Strip::process` checks the post-chain signal. NaN/Inf is cleared, the filters and
  dynamics reset, and `dspFaults` incremented.
- `MixerEngine::protectOutputs` clips every hardware output at the ceiling and zeroes
  any non-finite block.
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
- Out-of-process scanning of AudioUnits on macOS hasn't been verified on real
  hardware yet. The CI integration test covers VST3/LV2 only.
- No plugin delay compensation. Insert latency is shown per strip. Live mixing
  normally avoids high-latency plugins anyway.
- Plugins always run in stereo. A mono strip feeds both plugin inputs and takes the
  left output back.
- Planned: DCAs/mute groups, a matrix, sidechain keys for
  gate/comp, OSC/MIDI remote control, multitrack recording to disk.
