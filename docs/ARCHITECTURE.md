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

## Known limitations / roadmap
- No plugin delay compensation. Insert latency is shown per strip. Live mixing
  normally avoids high-latency plugins anyway.
- Plugins always run in stereo. A mono strip feeds both plugin inputs and takes the
  left output back.
- Planned: DCAs/mute groups, scenes/snapshots, a matrix, sidechain keys for
  gate/comp, OSC/MIDI remote control, multitrack recording to disk.
