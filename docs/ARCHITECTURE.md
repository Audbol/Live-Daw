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

Every strip type uses the same `Strip` class (`src/engine/Strip.h`). The engine decides
what feeds a strip and where its output goes.

## Threads

| Thread | Does |
|---|---|
| Message (UI) thread | Owns all `Strip` objects. Adds/removes strips, loads plugins, writes parameters. |
| Audio thread | `MixerEngine::processBlock`. Never allocates, locks or frees. |
| DSP workers | Realtime-priority threads in `WorkerPool`. They process strips in parallel with the audio thread. |

### Parameters
All parameters are `std::atomic`s in `StripParams` (`src/engine/Parameters.h`). The UI
writes them and the audio thread snapshots them once per block. Gains are ramped
linearly across the block. EQ frequency, gain and Q are smoothed, and coefficients
are recomputed every 32 samples.

### Structure changes
`MixerEngine` keeps an immutable `Topology` (a list of strip pointers) behind a
`std::atomic<Topology*>`. Adding or removing a strip builds a new topology on the
message thread and swaps the pointer. The old topology and any removed strips go to
`RtGarbage`. It destroys them on the message thread once the audio callback has
completed a full cycle, so no running callback can still hold a pointer to them.

Plugin inserts use the same scheme per slot (`PluginChain`). A plugin is created,
set to stereo and prepared on the message thread, then published atomically.

### Parallel processing
`WorkerPool::parallelFor` is a fork/join with no allocation. The audio thread
publishes a job (generation, task count and next index packed into one 64-bit atomic),
wakes the workers (`std::atomic::wait/notify`) and helps with the work itself.
Workers claim indices with CAS. Each block has two parallel stages:

1. all input strips (read hardware → chain → output stage)
2. all buses (sum sends → chain → output stage)

Summing into the master, building the monitor bus and writing the outputs is cheap
and runs on the audio thread.

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
- The UI builds components only for the current layer's strips. The master strip is
  pinned on the right.

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
- No plugin delay compensation. Insert latency is shown per strip. Live mixing
  normally avoids high-latency plugins anyway.
- Plugins always run in stereo. A mono strip feeds both plugin inputs and takes the
  left output back.
- Planned: DCAs/mute groups, a matrix, sidechain keys for
  gate/comp, OSC/MIDI remote control, multitrack recording to disk.
