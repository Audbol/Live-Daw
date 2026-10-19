# Architecture

Sidecar emulates a Behringer WING: the console topology, the processing models and
the control surface with its main display. The audio engine, DSP and sessions are
Rust; audio devices, plugin hosting and the UI are C++ with JUCE.

## Signal flow

```
 hardware inputs   recorder playback channels 1-64 (PLAY n = input 0x10000 + n-1),
       │           USB PLAYER 1-4 (input 0x20000 + n-1)
       │                 │
       ▼                 ▼   sources: gain · mute · Ø · MONO / STEREO / MID-SIDE pairs
       │                 │
       ▼                 ▼   (each input / aux channel: MAIN or ALT source, INPUT SELECT)
┌──────────── input channels and aux (parallel stage 1) ─────────────────────────────────┐
│ channel: INPUT trim · balance · Ø ─ IN ─ FILTER ─ FILT                                 │
│          G / E / D / INS 1 in one of 24 process orders ─ taps 3, 4, 5                  │
│          ─ PFL ─ fader (+ DCAs; mute, mute groups, DCA mutes) ─ AFL                    │
│          ─ INS 2 (FX slot or automix X/Y) ─ DELAY (POST) ─ POST ─ width / pan ─► out   │
│ aux:     INPUT ─ delay ─ DYN ─ EQ ─ INS 1 ─ PFL (= tap) ─ fader ─ POST ─ pan ─► out    │
└───┬──────────────────────────────────┬──────────────────────────────┬──────────────────┘
    │ bus sends (TAP/PRE at the tap     │ main sends (pre: tap, else out) │ matrix sends, direct outs
    │ point, POST, GROUP)               │                                │
    ▼                                   │                                │
 buses 9+  (parallel stage 2) ──► sends │                                │
    ▼                                   │                                │
 buses 1-8 (parallel stage 3)           │                                │
    ▼                                   ▼                                │
 mains 1-4 (parallel stage 4: inputs + buses)                            │
    ▼                                                                    ▼
 matrices 1-8 (parallel stage 5: inputs, buses, mains + a direct input from any channel)
    │       (talkback: the talk channel's PFL into the buses / matrices / mains of TALK A, B)
 solo (PFL/AFL per strip type, routed by SOLO MODE) ─► monitor paths SPEAKERS and PHONES:
    solo bus + SOURCE (Main 1 by default; SRC MIX while soloing) + DIRECT IN ─► EQ
    ─ PAN / WIDTH ─ INVERT ─ DELAY ─► level (+ MON DCAs) · DIM · mute · talk MON DIM
    · MONO / SWAP (on the selected path) ─► LIMITER ─► its outputs
 every strip ─► its hardware outputs ─► output protection (safety clip)
 RECORDER 1-4 (the USB recorder) tap strip outputs or sources after stage 6

 bus / main / matrix strip:
   summed sends ─ INPUT trim · balance ─ DYN ─ INS 1 ─ PFL (= tap) ─ fader (+ DCAs,
   mutes; Mains 2-4 + Main 1's level with MAIN LINK) ─ EQ (post-fader) ─ INS 2 ─ delay
   ─ POST ─ width / pan ─► out
```

- Input and aux channels take their signal from a source: a hardware input or one of
  the recorder's 64 playback channels (`PLAYBACK_SOURCE_BASE` + n, valid wherever a
  hardware input index is: strip inputs, ALT inputs, sidechain keys, record ports).
  Each has a MAIN (`input_left`/`input_right`) and an ALT source (`alt_input_left`/
  `alt_input_right`, -1 = none). It uses ALT when it has one and
  `input_select_global ? global_alt : alt_selected` (WING INPUT SELECT, manual
  p. 44-45, 68-69; here one console-wide INDIVIDUAL / GLOBAL choice). A switch
  between MAIN and ALT is instant (no crossfade).
- The order follows the WING Operation Manual (chapter 6). Input channels choose their
  process order and tap point (`TAP_POINTS`: INPUT, FILTER, TAP 3-5, PRE FDR, POST FDR,
  POST PROC); aux, buses, mains and matrices tap after INS 1 (`tap::PFL`), whatever
  `StripParams::tap` says. Aux channels have no filter, gate or INS 2 and delay before the
  fader; everything else delays after INS 2.
- Buses, mains and matrices use the 8-band bus EQ table. `ld_strip_response` draws the
  EQ alone, so its graph doesn't depend on where the EQ sits.
- PFL solo, direct outs, the matrix direct input (`direct_pre`) and the pre-fader meter
  (`StripMeters::pfl_peak`) read the PFL copy; AFL solo and post-fader sends read the
  output. `trim_peak` measures after trim, `input_peak` before it.
- Send modes: TAP/PRE reads the tap point and is cut while the source is muted (own
  mute, mute groups, DCA mutes when DCA MUTEGROUPS is on), unless an input or aux
  channel's send has IGNORE CHANNEL MUTE (`SendParams::ignore_mute`,
  `MatrixSendParams::ignore_mute`); pre-fader main sends always follow the mute. POST
  reads after INS 2 and the delay, panned with the send pan (linked, linked reversed or
  its own). GROUP reads the panned output at unity: it has no send level.
- Bus sends are stored in a paged `SendBank`, so buses can be added beyond the WING's
  16. Buses 9 and up can feed buses 1-8 (`BUS_TO_BUS_TARGETS`).
- A matrix's direct input takes any channel's PFL or output; inverted, it builds a
  mix-minus.
- INS 2 of input channels can run an FX slot or join automix group X or Y (each with its
  ENABLE switch in `EngineControls::automix_enable`). The audio thread computes the
  gain sharing between stages 1 and 2 (`engine.rs`, `automix`) from the post-fader level,
  so the channel fader shapes it; channels leaving automix glide back to unity.
- **Sources** (`sources.rs`): every hardware input port (the first 576) and playback
  channel has a source with gain, mute, polarity, +48 V (stored only), colour, icon,
  name and a pairing with its neighbour (ports 1-2, 3-4, ...: STEREO, or MID/SIDE
  decoded to M + S / M - S; a pair runs on its first source's settings) and mute
  groups (SOURCES > TAGS: muted while one is engaged). Before stage
  1 the audio thread processes the sources that change their signal into buffers of
  their own and hands those to the channels and the recorder's input streams; sources
  at their defaults pass their port through. Changes are ramped (gains per source,
  the mid-side matrix per pair).
- **SOURCE SOLO** (`monitor.rs`, `MonitorControls`): with SOURCE SOLO on CH39 or
  AUX7, a soloed source (`source_solo_port`; ROUTING > SOURCES' SRC SOLO, the
  WING's `$srcsolo`, or the host's own SOLO) is patched to that channel for the
  block (`ProcessContext::source_solo`, read in `read_hardware_inputs`; a pair plays
  in stereo) and the channel solos (`solo::console_pass`), so SOLO MODE routes it.
  The channel's own patch is untouched.
- **Solo and mute** (`solo.rs`): AFL (POST) solo reads the strip's output, after
  width and pan, so it follows the pan for inputs, aux and DCA members (WING 3.0.6;
  `post_solo_follows_the_pan`). Also before stage 1 the engine works out every strip's
  effective solo (own, a DCA's or a TALK SOLO tag) and the console's mute decision,
  which the strip's fader stage applies over its own mute (own, mute groups, DCA
  mutes) through `StripConsole::apply_mute`: SIP (SOLO MODE) mutes the input and aux
  channels that are neither soloed nor SOLO SAFE while an input or aux is soloed;
  MUTEGROUP/SIP OVERRIDE lets MUTE unmute a strip muted by a group, DCA or SIP until
  that cause changes; TALK ON tags unmute while their talk key is on. Strips report
  their MUTE state (`ld_strip_mute_state`; group mutes flash on the surface).
- **Monitor paths**: SPEAKERS (the `monitor_*` fields plus `speakers`) and PHONES
  (`phones`). Each has a SOURCE (MAIN.1-4, MTX.1-8, BUS.1-16, AUX.1-8, cross-
  faded when changed), SRC LEVEL, SRC MIX (the source's level while a solo reaches
  the path), PFL DIM, level, DIM amount and outputs. LIVE solos each strip to its
  `monitor_bus` (A = PHONES, B = SPEAKERS, A+B, as the WING's PH / SPK / PH+SPK) unless a
  path is picked while soloing
  (`solo_bus_override`); STUDIO and SIP solo to `solo_bus`, the selected path, which
  DIM, MUTE, MONO, SWAP and the talk MON DIM act on. DCAs named MON A / MON B / MON
  LVL add their level (and mute) to the PHONES, the SPEAKERS or both. The solo bus feed goes through
  `solo_tap` in `engine.rs`, the hook for solo-tap overrides (band, key, xover solo);
  LISTEN plays on the first selected path, a BAND SOLO boosted by that path's BAND
  SOLO.
- **Monitor path processing** (`monitor.rs`, `EngineControls::monitor`, path 0 =
  SPEAKERS, 1 = PHONES): DIRECT IN adds any input, aux, bus, main or matrix output
  at its own level (crossfaded when it changes); the EQ is the bus WING EQ (model
  STD: low shelf, 6 PEQ, high shelf; `WingEq`, so it glides like the strips');
  PAN, WIDTH and INVERT are one 2x2 matrix crossfaded over a block; DELAY (0.1-100
  m) crossfades between the old and new tap of a line that is always written; the
  LIMITER (-40..0 dB) is a stereo-linked peak limiter with instant attack and an 80
  ms release that meters its gain reduction (`limiter_gr_db`). A path that turns
  non-finite is reset and silenced for the block.
- **Talkback** (`talk.rs`, `EngineControls::talk`): the talkback channel (any input or
  aux channel; CH40 / AUX8 on SETUP) feeds its PFL copy to the buses (1-16), matrices
  and mains each talk path selects while its key is on, at TALK LEVEL or (USE SEND
  LEVELS) at the channel's own send levels, added to the destination's sum before its
  processing; the buses talked to are dimmed by BUS DIM and the selected monitor path
  by MON DIM. The application applies the keys' AUTO / PUSH / LATCH
  (`MixerEngine::talkKey`). Talk ramps belong to the destination tasks.
- Console-wide switches in `EngineControls`: PFL/AFL per strip type (`solo_afl` for
  input and aux channels, `bus_solo_afl`, `main_solo_afl`, `matrix_solo_afl`), the
  monitor keys (`monitor_dim` with `monitor_dim_db`, `monitor_mute`, `monitor_mono`,
  `monitor_swap`, ramped so they don't click), `dca_mute_groups`, `main_link` and
  `startup_main_mute` (applied by the application through
  `ld_engine_apply_startup_main_mute`).

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
| DSP (filters, EQ models, dynamics models, the FX rack's devices, delay) | Rust | `rust/crates/livedaw-dsp` |
| Realtime engine (WING topology, strips, sends, DCAs, mute groups, FX slots, automix, solo, monitor, output protection, worker pool, deferred frees) | Rust | `rust/crates/livedaw-engine` |
| Recorder (SPSC rings, WAV / RF64, AIFF, FLAC and MP3 codecs, transport, disk reader and writer threads, playlist rendering) and the USB recorder / player (`usb`) | Rust | `rust/crates/livedaw-recorder` |
| Sessions (console state, Show > Scene > Snippet, fades, fader banks, show files, legacy import) | Rust | `rust/crates/livedaw-session` |
| WING interoperability (parameter tree, mapping, discovery / OSC / native servers, .snap / .show files) | Rust | `rust/crates/livedaw-wing` |
| C ABI | Rust + C++ header | `rust/crates/livedaw-ffi`, `include/livedaw_engine.h` |
| Audio devices (ASIO, CoreAudio, ALSA, JACK, WASAPI), plugin hosting (VST3/AU/LV2), UI | C++ / JUCE | `src/` |

`src/engine/MixerEngine`, `Strip`, `FxSlots`, `Models`, `Show`, `Recorder`, `UsbRecorder` and `LayerSet` are thin
C++ facades over the Rust code. `Strip` and `FxSlots` are stateless views: names,
colours and slot state live in the engine, and `MixerEngine::syncStrips()` rebuilds the
views after a show load changes the console from Rust.

- **Shared parameters.** Strip parameters, meters, sends, DCAs, FX slot controls and
  engine controls are `#[repr(C)]` blocks of atomics owned by Rust. C++ sees the same
  memory as `std::atomic<float/bool/int32_t>` structs, and the UI writes them directly
  with no FFI call. Both sides describe the layout; `ld::verifyLayout()` compares the
  two at startup and in the tests (ABI version 8), and the app refuses to run if they
  differ.
- **FX slots.** The 16 slots are the WING's FX rack. Each holds one device from the
  console's FX table (`FX_MODELS`, generated like the processing models): the 26
  premium effects (reverbs, chorus/flanger/phaser, delays, pitch; slots 1-8 only), the
  32 standard effects (EQs, dynamics, enhancers, amps, utilities) and the 5 channel
  strips, or **EXT**, a VST3/AU/LV2 plugin.
  - Built-in devices live in `livedaw-dsp/src/wing/fx` (one module per family, shared
    building blocks in `blocks.rs`). A device is an `FxAlgo`: it is created on the
    message thread (allocating its buffers for the sample rate and block size), then
    swapped into the slot (`AtomicPtr`) and runs on the audio thread without
    allocating or locking. `FxDevice` feeds it the slot's parameter atomics
    (`FxParams::values`) when they change. A replaced device is freed through the
    epoch garbage list. Setting the model through the parameters (a scene recall, the
    UI) is picked up by `ld_engine_collect`, which builds the device.
  - Plugin instances stay in C++ (`FxSlots`). Each loaded slot registers `process`
    and `release` callbacks with the engine (`ld_fx_set`, which also sets the model to
    EXT). A replaced plugin's `release` runs on the message thread once no audio
    callback can still reach it.
  - Rust runs the slot from whichever thread runs the strip whose PRE or POST insert
    uses it, keeps a dry copy for the slot's FX MIX, and auto-bypasses the slot if a
    plugin reports an error (a C++ exception is caught in the callback) or a device or
    plugin returns NaN/inf. A slot runs for one strip per block: a second insert on
    the same slot passes its signal through and the slot is flagged as double
    assigned. The engine measures each slot's load.
  - Devices are stereo. A mono strip feeds both inputs and takes the average of the
    two outputs back.
- **FX in sessions.** A slot saves its device and parameter values. Plugins are
  reached through a *plugin bridge* implemented by `Show`: capture a slot (description
  XML + base64 state), restore one (state only, or by creating the plugin on show load)
  and clear one.
- **Panics.** Every FFI entry point and every worker task runs under `catch_unwind`.
  A panic in the audio path outputs silence for that block and increments
  `engineFaults`. It never unwinds into C++.

## Threads

| Thread | Does |
|---|---|
| Message (UI) thread | Adds/removes strips, loads plugins, writes parameters, collects garbage (`ld_engine_collect`, 10 Hz), runs scene fades (`ld_show_tick`, 60 Hz). |
| Audio thread | `ld_engine_process`. Never allocates, locks or frees. |
| DSP workers | Realtime-priority JUCE threads lent to the Rust pool (`ld_engine_run_worker`). They process strips in parallel with the audio thread. |
| Disk reader | Rust thread (started on the recorder's first use). Renders the playlist ahead of the play position into the playback rings. |
| Disk writer | Rust thread. Writes the record rings to WAV files, computes the recording peaks, finalizes the takes. |
| USB disk | Rust thread (the USB recorder / player, started on its first use). Primes and tops up the player's ring, writes the USB recordings. |

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

## Recorder

`rust/crates/livedaw-recorder` (no dependencies), owned by the engine; C++ facade
`src/engine/Recorder.*` over `ld_rec_*`. The UI owns the project (tracks, clips,
undo, files); the recorder knows the playlist it was last given and the takes it
recorded.

- **Transport** (`audio.rs`, audio thread): Stopped / Starting / Playing / Recording,
  the position (samples at the engine rate) and a loop range. The message thread sends
  commands through an SPSC queue; getters already show a command the audio thread
  hasn't taken (play reads Starting at once, a locate its target).
- **Per chunk** (`engine.rs` `process_inner`): `begin_chunk` takes the commands, starts
  rolling once the reader has primed, shortens the chunk at the loop end and at a
  pending ring switch (every chunk is contiguous on the timeline) and fills the 64
  playback buffers (silence unless rolling, and while Starting); the strips run
  (playback channels are sources); hardware inputs are recorded at the start of the
  chunk, strip taps after stage 6 (`StripPreFader` = the PFL tap, `StripPostFader` =
  after fader and pan; strips by kind and number as shown, CH 3 = 3, BUS 12 = slot 11;
  a missing strip records silence); `end_chunk` commits the recorded frames and
  advances the position. Sample accuracy: playback sample i of a chunk at position p
  is p + i; recorded sample k of a take is its start + k.
- **Playback rings** (`reader.rs`): two planar rings ("slots") of 2 s, one lane per
  playback channel the playlist uses. Play or locate puts the transport in Starting
  with a new epoch: the audio thread uses no ring; the reader primes slot 0 from the
  position (0.25 s), publishes it, rolling begins with a 256-sample fade-in. A locate
  or stop while rolling fades the chunk out first (256 samples). A playlist or loop
  change while rolling: the reader renders the new content into the other slot from a
  transport frame S a little ahead (50 ms, doubled after a miss), publishes a switch
  request (seqlock); the audio thread splits its chunk to land on S and crossfades
  over 512 samples, or reports a miss and keeps playing the old ring. The audio thread
  never waits. An underrun plays silence, counts a dropout and skips the missing
  frames later, so playback stays aligned.
- **Rendering** (`playlist.rs`): clips (file offset, start, length, gain, fade in /
  out with Linear, EqualPower or SCurve curves; a linear crossfade sums to 1),
  overlapping clips summed, track gain and mute; mono files feed both sides of a
  stereo track, stereo files are averaged on a mono one. Recording never loops.
- **Recording** (`writer.rs`): `record` creates the files in the record format
  (`set_record_format`: 32-bit float WAV by default, `codec::create_writer`; or fails),
  one record ring (4 s, one lane per mono stream) per recording. The writer drains it,
  computes min/max peaks per 256 samples (`getRecordingPeaks`), rewrites the headers
  every 2 s and, once the audio thread has let go (stop, locate, device restart,
  shutdown), finalizes the files; only then does a take appear in
  `takeFinishedTakes`. The take ends with the chunk that saw the stop. An overrun
  becomes silence in the file (a gap record), so the take stays aligned. Record meters
  (`getRecordLevel`) are peaks the audio thread stores.
- **WAV** (`wav.rs`): the writer reserves a `JUNK` chunk the size of `ds64`; past
  4 GB the header is rewritten in place as RF64. A file whose header is behind its
  data (crash) reads to its end. The reader takes 16/24/32-bit integer and 32-bit
  float, plain, `WAVE_FORMAT_EXTENSIBLE` or RF64.
- **Codecs** (`codec/`, no dependencies, written from the format specifications):
  `AudioWriter` / `AudioReader` traits, `create_writer` (by `EncodeFormat`) and
  `open_reader` (recognizes the format by its contents), used by both recorders'
  writer threads and by the playlist renderer and the USB player's reader. AIFF
  (`aiff.rs`: big-endian 16/24-bit, AIFF-C `fl32`; headers rewritten every 2 s like
  WAV), FLAC (`flac.rs`: 4096-sample blocks, constant / fixed 0-4 / LPC up to order 12
  with Rice or Rice2 residuals in partitions, stereo decorrelation, verbatim fallback;
  STREAMINFO says "unknown length" until `finalize` patches the total and the MD5; the
  decoder checks CRC-16, seeks by SEEKTABLE or by bisecting on frame headers, and
  measures a stream cut short from its last frames), MP3 (`mp3/`: MPEG-1 Layer III,
  CBR 128-320 kbit/s at 32 / 44.1 / 48 kHz; polyphase analysis, MDCT, alias
  butterflies, a spreading masking model, the two nested loops, Huffman coding with
  the standard tables, mid/side; no bit reservoir; an `Info` frame in the LAME layout
  carries frames, encoder delay and padding, patched on finalize; the decoder handles
  any MPEG-1 Layer III stream (reservoir, short / mixed blocks, M/S and intensity),
  skips the delays (gapless, sample-aligned with what was recorded), indexes frames
  lazily and restarts 10 frames before a seek target). Tests cross-check with ffmpeg
  when it is installed (FLAC bit-exact, every Huffman code, LAME / shine files).
  C ABI `ld_afw_*` / `ld_afr_*` for the C++ side (`src/engine/AudioFileFormat.*`:
  `AudioFileWriter`, `AudioFileReader`, and `EngineMp3AudioFormat` registered first in
  the project's `AudioFormatManager`). MP3 encoding costs about 1/50 of a core per
  mono track.
- **Health**: `getDropouts` counts ring under/overruns and write errors;
  `getDiskLoad` is the fullest record ring / emptiest playback ring. Offline drivers
  (tests, rendering faster than real time) call `waitForDisk` between blocks.
- **Virtual device** (`src/engine/VirtualAudioDevice.*`): the device type
  "Virtual (no hardware)" runs the callback from a clocked thread with 64 silent
  inputs and 64 outputs. `Main.cpp` registers it after the platform types, so it is
  only used when chosen (SETUP > AUDIO or the saved device settings).

## USB recorder / player

`rust/crates/livedaw-recorder/src/usb` (`UsbDeck`, owned by the engine next to the
multitrack recorder; C ABI `ld_usb_*`, scalars and strings only; C++ facade
`src/engine/UsbRecorder.*`; screen `src/ui/UsbRecorderPage.*`). The WING's USB
recorder (`/rec`) and player (`/play`), on the multitrack recorder's rings, codecs
and disk-thread design.

- **Engine hooks** (`engine.rs`, "USB deck hooks"): `begin_chunk` / `end_chunk` run
  next to the multitrack recorder's in each chunk. The four player buffers are sources
  after the playback channels (`SourceDsp`, table indices `USB_PLAYER_SOURCE_INDEX`..,
  input `USB_PLAYER_SOURCE_BASE + n`, resolved by `source_ptr`), so channels patch and
  process them like any input. After stage 6, `record_usb` writes the four RECORDER
  ports: each holds a `RecTap` (an `AtomicI32` code: off, a source's input index, or a
  strip by kind and number and its channel), read as the strip's hardware output
  carries it (input / aux channels their PFL direct out, the others after fader and
  pan). The ports are metered while not recording. ROUTING shows them as the output
  group RECORDER; a tap leaves the strip's own outputs alone.
- **Recorder** (audio thread `usb/audio.rs`, disk thread `usb/disk.rs`): REC creates
  the first file on the message thread (an error is reported at once) and hands a
  `RecSession` (a 2- or 4-lane ring of 4 s, an event queue, the files to come) to the
  disk thread and, through an `AtomicPtr`, to the audio thread. PAUSE stops writing
  (the file stays open); NEW FILE creates the next file first, then the audio thread
  queues a `NewFile` event at the ring frame it reached, where the disk thread switches
  files: no sample is lost or doubled. Overruns become gap events (silence). STOP ends
  the session at the frames committed; the disk thread drains it and finalizes the
  file (`RecFormat`: WAV / FLAC / AIFF 16/24-bit or 2-channel MP3 through
  `codec::create_writer`; a file that got no sample is deleted).
- **Player:** one ring of `PLAY_RING_FRAMES` (4 lanes). As in the multitrack reader,
  each start, cue, seek or stop is a new epoch: the audio thread stops reading, the
  disk thread resets the ring, opens the clip (the queue resolves its id; a missing or
  unreadable file reports an error), primes 0.2 s and publishes `ready(epoch)`. The disk
  thread keeps the ring full; at the end of a clip it waits until 0.5 s is left, asks
  the queue what follows (`Queue::after`: the next clip not skipped, REPEAT wrapping,
  STOP after a clip, one file at a time for a folder) and pushes a `Boundary` with the
  ring frame where it starts (or where playback stops, with the clip to cue), so clips
  follow without a gap and late playlist edits count. Positions and lengths are in
  engine frames; a file at another rate is resampled (`usb/resample.rs`: 64-tap
  windowed sinc with interpolated phases, positions exact in integers) and flagged.
  Play, pause, stop and seeks fade in or out over 256 samples; an underrun plays
  silence and waits for the disk.
- **Queue** (`usb/queue.rs`, behind a mutex shared with the disk thread): a folder's
  audio files (WAV, FLAC, AIFF, MP3) or a playlist of entries with stable ids, SKIP and STOP flags and REPEAT;
  playlists are extended M3U files (`#LIVEDAW:` lines for the flags).
- **Facade and screen:** `UsbRecorder` adds the storage folder (default
  `Documents/Sidecar/USB`), the browser listing, FOLDER / RENAME / DELETE, file names
  `R_YYYYMMDD-HHMMSS.<ext>` in the REC PATH and the settings (app settings `usb.*`,
  including the RECORDER taps, USB HOST SPEED and the FORMAT `usb.recFormat` /
  `usb.recKbps`). The page draws figs. 78 / 79 itself (keys, seven-segment times,
  reels, meters; FORMAT and BIT RATE open WING lists) and is opened from the status bar's USB
  icon or by holding the monitoring VIEW key; the CC section's 2-TRACK TRANSP layer and
  the USB PLAY/REC custom-control functions drive the same facade.
- **Tests:** `livedaw-recorder/tests/usb.rs` (bit-exact 2/4-channel 16/24-bit
  recordings in WAV, FLAC and AIFF and MP3 recordings, each played back; pause and NEW
  FILE in every format, dropping the deck mid-recording, playlists with SKIP / STOP /
  REPEAT, seeks, rate conversion, missing files; a counting allocator checks that the audio-thread calls
  never allocate), `livedaw-engine/tests/usb.rs` (RECORDER taps and USB PLAYER sources
  end to end), `tests/UsbRecorderTests.cpp` (the facade; JUCE reads the files back).

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

`livedaw-session/src/console.rs` serialises strips **per section** (Name, Routing (CONN:
input patch, ALT inputs, outputs; with the console's INPUT SELECT switches),
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
 ├─ GlobalSafe  DCAs, mute groups, FX slots, sections, console settings
 ├─ PartialScope  the scope PARTIAL LOAD / UPDATE last used
 └─ Scene[]     State (all sections) + recall scope + fade time + notes
     └─ Snippet[]   State (each strip with its own sections) + scope + "fire with scene"
```

- `livedaw-session/src/scope.rs`: a `Scope` is sections + strips (or all) + DCA,
  mute group and FX slot masks + the console-wide settings. Scoped recall filters the
  captured `<State>` (strips outside the scope, items outside the masks or GLOBAL
  safe, console parts) and hands the rest to `console::apply`, so DCAs, mute groups and
  FX slots still apply with their section (FADER, GROUPS, INSERTS). PARTIAL UPDATE
  merges a scoped capture into the stored state (a strip's captured children replace
  the stored ones). Snippet REC captures the console once per section at REC and
  compares at STOP: the changed (strip, section) pairs, DCAs, mute groups and FX slots
  become the snippet (each strip element keeps its own `sections`), and the number of
  changed values is its parameter count. Plugins are left out of the comparison.
- `livedaw-session/src/presets.rs`: FX presets (`.lfx`, one `<Slot>` through the same
  per-slot capture / apply as shows) and channel presets (`.lchn`, one `<Strip>` with a
  scope, applied to another strip of the same family by rewriting its `ref`).

- The console also keeps the console-level state of `livedaw-session/src/globals.rs`:
  SOLO MODE, the solo bus, MUTEGROUP/SIP OVERRIDE and SOURCE SOLO, talkback, the
  strips' talk tags and the sources (only those that differ from the defaults, with
  their mute groups). The monitor paths (sources, PHONES, each path's processing as
  `<Dsp>`) are part of the monitor setup, which scenes recall with ROUTING.
- Scene recall applies the state within the scene's scope and skips recall-safe strips.
  It then applies the scene's auto-fire snippets.
- Snippets ignore recall safes, which makes them the tool for deliberately changing a
  protected channel.
- Fades collect fade targets (fader, pan and send levels) instead of setting them, and
  `ld_show_tick` interpolates the atomics. A fade target holds its strip (`Arc`), so
  removing a strip mid-fade is safe. A new recall finishes any fade still running.
- FX slots recall their built-in device and its parameters. A plugin's state is
  recalled only when the same plugin is already in that slot; recalls never
  instantiate or remove plugins, and a premium device is refused in slots 9-16.
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
- A bank holds `FaderRef`s: a strip, a DCA, a send level (`send:ch:3>bus:0`) or an
  FX parameter (`fxp:2.5`). User banks are saved with the show; the first two are on
  the USER 1 / USER 2 keys, further ones get keys of their own under the shift keys. A
  reference to a removed strip is skipped.
- **Spill** (`SpillKind`): a computed layer (id `spill`) shown by a section until it
  ends: a DCA's members, the sources sending to a bus, an FX device's continuous
  parameters, an automix group, a strip's sends. `spill_target` sends a spill from the
  right section to the center, the rest to the left. The layer set keeps each
  section's scroll offset, so ending a spill restores the layer and the offset; a
  layer key ends it without going back.

## The UI

`src/ui/` draws the WING surface: three fader sections (`FaderSection.*`,
`FaderStrip.*`) with VIEW, the layer and shift keys, colour bars and scribble strips,
SELECT / SOLO / MUTE, LED meters and scaled faders; the custom controls section
between the center and right sections (mute groups, show control); and the top row
laid out as on the console: the WING panel with USB AUDIO/DATA and MONITORING/TALKBACK
(left), the main display with the screen keys, the channel strip section with its LED
panel and screen (`SurfaceSections.*`), and the 4-Channel section
(`FourChannelSection.*`, the right fader section's four strips) at the right. Windows
too narrow for that keep the channel strip at the left and the monitoring panel over
the 4-Channel section (or the custom controls' lower part) at the right. SETUP
shows CUSTOM CONTROLS (`CustomControlsPage.*`, also the assignments' model and what
each function does) and MONITORS (`MonitorsPage.*`) in place of its tabs. The
preferences the surface, the status bar and the pages share are `uiprefs::` keys in
`UiContext.h`. Surface keys
(`SurfaceKey` in `WingWidgets.h`) know press, hold (500 ms or right-click) and
blinking LEDs.

- **Look.** `ConsoleLookAndFeel` (`LookAndFeel.*`) holds the WING palette (black
  screen, grey panels with header bars, amber channel tab, yellow ON keys, lime knob
  rings, cyan selections; the surface follows the dark WING's panel: frosted light
  key caps in black bezels lit in their LED colour, indigo scribble LCDs under thin
  glowing light bars, silver fader caps, plain encoder caps) and uses Barlow Condensed (SIL OFL, `resources/fonts/`,
  embedded with `juce_add_binary_data`). `Icons.*` draws the screen icons as vectors;
  `WingWidgets.h` has the screen keys, ON chips and segmented keys.
- **Main display** (`MainDisplay.*`): the screen keys, the 16:10 screen and the column
  right of it (CLR SOLO, main meter, fine-adjust knob, blank key); the status bar
  (channel tab in the strip's colour, status tiles, the page's `getStatusKeys()`,
  clock), the page, and the parameter bar with six controls that follow the page
  (`DisplayPage::getKnobs`, styled by `getKnobStyles`), plus CHANNEL SELECT.
- **Screen only** (`MainComponent::setScreenMaximized`, `UiContext::setScreenMaximized`;
  F10, the display's maximize key, a double-click on the status bar): the other
  sections are hidden but stay alive and laid out; the display takes the window
  (`MainDisplay::setMaximized` shrinks its frame around the largest 16:10 screen and
  centres it on black). Windows taller than 1100 px lay it out at 1100 px and scale it
  up with a component transform, so the screens keep their proportions. F11 is full
  screen (kiosk mode). Settings `ui.screenMaximized`, `ui.fullScreen`.
- **Start-up notice** (`StartupNotice.*`, `popups::notice`): what the project is and
  how it differs from a WING, shown at start (after safe-mode recovery) unless turned
  off for the running version (`ui.noticeDismissedFor`); SETUP > GENERAL > ABOUT and
  the LOG tab's ABOUT show it again. Keep its list in step with *Known limitations*.
- **Screens:** HOME (`HomePage.*`: the toolbar, OVERVIEW, ICON/COLOR, NAME, TAGS and
  the INIT / COPY / SETTINGS / FEED overlays; `HomeScreens.*`: INPUT, FILTER SETUP,
  GATE / COMP, EQ, INS 1 / INS 2, MAIN SENDS, BUS / MATRIX SENDS and the overlays;
  `HomeWidgets.h`: their shared widgets),
  EFFECTS, METERS, ROUTING, SETUP, LIBRARY (`ShowPanel.*`: the scene / snippet list and the INT / USB file list;
  `ScopeWindow.*`: the SCOPE and GLOBAL window) and UTILITY, and the VIEW screens
  (`ViewPages.*`: OVERVIEW, FADERS, SENDS, CONFIG, MUTE GROUPS, for the section in
  `UiContext::viewSection`). `ScreenWidgets.h` holds the darker widgets of the SETUP,
  LIBRARY and VIEW screens (tab row, choice keys, ON key, dropdown fields, settings
  panels, overlays) and the console preferences kept in the app settings
  (`prefs::`, e.g. CONFIRM LIBRARY LOAD, TOUCH FADER RES, the meter taps).
- **Faceplates** (`Faceplate.*`): every FX device and dynamics/EQ model is drawn as the
  unit it recreates. Drawn designs (`FaceplateDesign.h`, `FaceplateDraw.cpp`,
  `FaceplateDesigns.cpp`) lay a unit out in the screen's faceplate frame (1156 x 479
  px: scratched backdrop, inset plate), measured off the console's screen: knob looks,
  scales, toggles, keys with LEDs, VU / LED meters showing the gain reduction, 7-segment
  and dot-matrix displays, an EQ response graph, and tabbed pages for units whose keys
  page their controls; bitmaps are rendered at the shown pixel scale. Models without a
  design get a generic panel styled per model token (panel colour, knob style, meter). EFFECTS opens a
  slot's faceplate in its editor; the GATE, EQ and DYNAMICS screens show the model's.
  The device menu groups the table like the console (PREMIUM, STANDARD, CHANNEL
  STRIPS, PLUGIN).
- **Dynamics graphs** (`DynamicsGraph.*`): the WING's own gate, ducker, compressor and
  expander show the transfer graph and the ENVELOPE panel instead of a faceplate. The
  curve is the DSP's static curve (`ld_dyn_static_curve`, the same configuration the
  audio thread runs); the level dot, GR meter and output ticks read the slot's meters
  (`StripMeters` gate/dyn key level, reduction and output peak). The model list is the
  3.1 tile grid (`ModelSelector::chooseDynamics`).
- Views poll the shared atomics on timers (4-25 Hz); meters read `MeterBank`, the one
  reader of the engine's "peak since last read" meters (input, post-trim, pre-fader
  and output).

### Multitrack projects, the editor and the recorder screen

- **Project model** (`src/daw/`, no UI, unit tested in `tests/DawTests.cpp`).
  `daw::Project` holds a project in a `juce::ValueTree` (tracks of clips, markers,
  loop range, tempo / time signature for the grid, sample rate) and every edit goes
  through its `UndoManager` (`beginEdit()` starts an action; arming and track heights
  aren't undoable). A project is a folder `<name>/<name>.ldproj` (XML, written
  atomically with a `.bak`) plus `Audio/` (takes, imports), `Peaks/` (cached
  waveforms) and `Export/`; clip paths inside the folder are relative.
  `clipmath` does the edit arithmetic (split, trims, moves, range clearing,
  crossfades, fade curves), `TimeGrid` the snapping and time formats,
  `AudioFiles` the import (any JUCE format incl. FLAC and MP3, resampled with a
  windowed sinc to the project's rate, 32-bit float WAV) and the track export (any
  `AudioFileFormat`: WAV, AIFF, FLAC, MP3, through `AudioFileWriter`). The session's
  record format (`daw.recordFormat`, 32-bit float WAV by default; the RECORDER
  header's format key and the editor's FILE menu) names the takes' extension.
- **`daw::Session`** ties the project to the engine's `Recorder`: it rebuilds the
  playlist whenever the project changes (throttled during drags; muted, solo-excluded
  tracks muted, muted clips and missing files left out), follows the loop, records the
  armed tracks into `Audio/`, collects the takes into clips (one undoable "Record"
  action, replacing what they cover) and saves the project, polls record levels and
  the peaks of the takes in progress for the views, runs imports and exports on a
  worker thread, autosaves to `<name>.ldproj.autosave` every 30 s (offered when the
  project is opened next) and remembers recent projects. `MainComponent` owns it and
  reopens the last project at start-up; `UiContext::recording` hands it to the views.
- **Editor** (`src/ui/daw/`): `DawEditor` replaces the console view (Ctrl+E, EDIT,
  holding the surface's DAW key; Esc / CONSOLE return). `DawEditorState` holds the view
  (zoom, scroll, snap), the selection and a copy of the tracks and clips refreshed
  once per message-loop turn after project changes, plus the edit operations.
  `DawTrackList` / `DawTrackHeader` draw the headers (hit areas, no child buttons),
  `DawRuler` the ruler, loop and markers, `DawLanes` the clips (waveforms from
  `juce::AudioThumbnail` with a cache that keeps finished thumbnails in `Peaks/`;
  takes in progress from `Recorder::getRecordingPeaks`). The lanes and ruler are
  buffered to images, so the playhead (its own component, moved on a
  `VBlankAttachment`) only blits; a growing take repaints only its new pixels.
- **Recorder screen** (`RecorderPage`, `Screen::Recorder`): the WING-LIVE layout
  (PLAYER, SESSIONS, MARKERS, the tracks to arm, RECORD ALL CHANNELS and VIRTUAL
  SOUNDCHECK), opened from the status bar's LIVE card tile or MULTI TRANSP's VIEW.
  VIRTUAL SOUNDCHECK sets each channel's ALT input to the playback channel of the
  track recording it (`Session::virtualSoundcheckRoutes`) and switches INPUT SELECT
  to GLOBAL ALT. ROUTING (MAIN / ALT keys, PLAY source groups), HOME > INPUT (ALT
  tile, MAIN / ALT, INDIVIDUAL / GLOBAL) and SETUP > AUDIO (INPUT SELECT grid, GLOBAL
  INPUT SELECT OVERRIDE) edit the ALT inputs.

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

## WING remote and WING files

`rust/crates/livedaw-wing` makes Sidecar look like a WING to remote software. It is
clean-room: built from Patrick-Gilles Maillot's *WING Remote Protocols* document,
libwing (Apache-2.0; its `propmap.jsonl` is the source of the tree) and the Bitfocus
Companion module. No Behringer firmware or WING Edit code is used. It has no
dependencies beyond the other Sidecar crates.

- **Tree** (`schema.rs`). `tools/wing/gen_tree.py` compiles libwing's parameter map
  (WING firmware 3.1) into `data/wing_tree.bin`, embedded in the binary: rows (node
  id, name, parent, definition), shared definitions and enums, and the processing
  model families. A model's parameters are addressed by position in their slot
  (`Node::Param { slot, index }`), as on the console, where the ids depend on the slot
  and position and the active model decides names and types. Paths, ids, children
  and definitions are resolved against the live models.
- **Mapping** (`map.rs`). `bind()` gives every row a `Binding` (a strip field, a DCA
  or mute group field, an FX slot, a console control, an output patch...) and
  `Console` gets and sets values in WING units (dB, Hz, enums by name, fader law with
  -144 for -oo), converting to the engine's parameters, model tables and port
  numbers. Ports: AES50 A/B/C 1-48 are hardware ports 1-144 and the WING-LIVE card
  (CRD) is the recorder's playback channels. Nodes without an engine equivalent live
  in a **shadow store** (`WingState`) so clients and snapshots read back what they
  wrote; they do nothing. `map::coverage()` counts what maps (82.8 % of the leaves).
  The console level maps too: TAP WIDTH / TAP EQ (`/ch/N/tapwid`, `peq`), `/cfg/solo`
  (mode, the solo bus, mute, SOURCE SOLO), `/$ctl/cfg/muteovr`, `/cfg/talk`, both
  monitor paths (`/cfg/mon/1` PHONES, `/2` SPEAKERS), the RTA settings and the
  sources of the A/B/C and CRD groups (`/io/in/G/N`: name, colour, icon, gain, mute,
  polarity, +48 V, mode, `#M` tags); the MONITOR outputs are MON 1/2 (PHONES L/R)
  and 3/4 (SPEAKERS L/R).
- **Servers** (`server.rs`, `osc.rs`, `native.rs`, `meters.rs`). Plain `std::net`
  threads, started and stopped from the app: discovery (UDP), OSC (UDP), the native
  protocol (TCP, one thread per client, up to 16; meters as UDP datagrams) and a
  poller. They read and write the engine's shared atomics like the UI does, never
  touching the audio thread, and every thread runs under `catch_unwind`. A client's
  change is applied and pushed to the other clients at once; the poller diffs the
  mapped leaves every 100 ms to push changes made elsewhere (UI, scene recalls) and
  sends meters every 50 ms. It takes its snapshot without blocking client sets and
  drops a snapshot that raced one (generation counter).
- **Files** (`snap.rs`, `json.rs`). Snapshot import sets each value through the same
  mapping (models first, input group before number), keeps unmapped values in the
  shadow store and reports what was not applied. Export walks the tree and writes the
  console's `snapshot.11` layout. A `.show` becomes scenes: each snapshot is applied
  to a scratch engine copied from the live console and captured there.
- **C ABI / C++.** `livedaw-ffi/src/wing.rs` (`ld_wing_*`) owns the server and state;
  `src/engine/WingRemote` wraps it, owned by `MixerEngine`, which destroys it before
  the engine. SETUP > REMOTE starts it from the app settings (`wing.*` keys and the
  console name); LIBRARY > SHOW imports and exports files.
- **Tests.** `tests/remote.rs` drives the servers over sockets, `tests/snapshot.rs`
  imports an authored fixture and round-trips exports, `tests/libwing.rs` runs
  libwing's own clients when `LIBWING_TOOLS` is set (`tools/wing/libwing_tools.sh`),
  and `tools/wing/check_companion.py` checks the Companion module's paths.

Limits: WING Edit and WING Copilot are untested. One output patch per strip (the WING
can send a source to many outputs; extra ones are kept in the shadow store). Input
groups other than AES50 A/B/C and CRD collapse onto the port with the same number
(their source settings stay in the shadow store). A talkback channel other than
CH40 / AUX8 reads as OFF, and DIRECT IN's level has no node.
Meters are approximate (RTA, source and output meters stay silent). Getting a model
parameter position the active model doesn't have gets no answer (libwing's
`wingdrive --probe-echo` relies on the console answering it). Export is unvalidated
on a console.

## MIDI remote control and DAW remote control

`src/engine/MidiRemote.*` (MIDI, owned by `MainComponent`, `UiContext::midi`) and
`src/engine/DawRemote.*` (the MCU / HUI protocols); `src/ui/MidiRemoteUi.*` connects
them to the views, `src/ui/DawSurface.*` is the fader sections' DAW mode. Sources: the
WING MIDI and DAW documentation (firmware 1.07), the DAW translation table, Patrick-
Gilles Maillot's *WING Remote Protocols* (MIDI chapter, the MCU command lists), the
public Mackie Control and reverse-engineered HUI protocol descriptions.

- **Ports.** Five logical ports, as on the WING: DIN, USB (MIDI Control), MIDI DAW
  1-3. Each has an input and an output device (JUCE `MidiInput` / `MidiOutput`,
  chosen by name) or the app's virtual port. A port opens only while a function or
  the DAW uses it; `MidiDeviceListConnection` and a 2 s retry reopen devices that come
  back. Settings: `midi.*` and `daw.*` in the app settings (`MidiRemote::Settings`).
- **Threads.** Device callbacks only queue the message (a locked vector) and trigger
  an `AsyncUpdater`; `handleMessage()` runs on the message thread, where it writes the
  engine's shared atomics (as the UI and the WING remote do), recalls scenes and calls
  the UI's hooks (custom controls received over MIDI). Tests call `handleMessage()`
  and read `onSend`; `useDevices = false` keeps them off the MIDI system.
- **MIDI REMOTE CONTROL.** Each function (`/$ctl/midi`) is OFF / DIN / USB and handles
  only its port's messages. The CC maps are static (`stripForCc`, `fxControl`,
  `customEncoder`); fader values use the WING fader law (0..127 = position), pans
  64 = centre, mutes >= 64 = on. Feedback: a 25 Hz timer diffs the mapped values
  against what was last sent or received (no echo of received values; the first pass
  after a route change only learns) and sends the changes; CC 9 sends everything.
  SYSEX CONTROL execute / dump / describe go through `ld_wing_sysex`
  (`livedaw-wing/src/sysex.rs`, the snapshot node-string grammar on the WING tree).
  SCENE MIDI TX sends a program change when the current scene changes on the console
  (not for recalls MIDI asked for).
- **DAW remote.** `DawRemote` emulates up to three units (MCU + 2 extenders, or 3
  HUI) of eight strips; `output (unit, message)` goes to MIDI DAW 1-3 (DIN: unit 0).
  Surface -> DAW: faders (MCU pitch bend 14-bit / HUI zone faders) with touch, V-pots
  (relative), strip keys, the DAW BUTTONS commands (code, MCU note, HUI zone / port;
  greyed in the other protocol), jog, and the special keys (FADER TOUCH [MUTE], V-POT
  CTRL, RECRDY / AUTO / V-SEL / INSERT [SEL]). DAW -> surface: motor faders, LEDs
  (on / blink), V-pot rings, meters (decaying 300 ms per segment), the LCD (2 x 56
  per unit, 7 characters per strip; HUI 4-character names and the 2 x 40 display),
  timecode and the assignment display; the MCU handshake (device query -> serial and
  challenge, connection reply -> confirmation, version) and the HUI ping are answered.
  DAW mode (`setOn`, the DAW key) only decides whether the surface drives the DAW; the
  state stays current either way. The views poll `getRevision()`.
- **Surface.** DAW mode: `DawFaderSection` lies over each fader section (shown while
  DAW mode is on and the CONFIGURATION gives faders); the custom controls section
  switches its lower layer keys to the DAW layers (custom control layers 20-23,
  `livedaw-session/custom.rs`) and, with USE UPPER CC FOR DAW, its upper part to four
  V-pots (blocks of four). Custom control functions DAW BUTTONS, DAW V-POTS, MIDI CC and
  MIDI CC/NOTE/PGM call `midiui` (keys get press and release).

Limits: virtual ports need ALSA (Linux) or CoreMIDI (macOS); the WING's per-scene SEND
MIDI strings, the HUI's extra encoder section (upper VIEW), the 4-Channel section in
DAW mode and DAW meters on the right section's 4-Channel strips are not done; the DAW
presets are Sidecar's own (built from the translation table), not the WING's files.

## Known limitations / roadmap
- Plugins run inside the mixer process. Bad output and exceptions are contained, but
  a plugin that segfaults takes the app down (safe mode then recovers the show).
  Hosting each plugin in a sandbox process is the next step for full isolation.
- The FX devices recreate each unit's controls and characteristic sound, not the
  console's exact algorithms.
- No plugin delay compensation.
- Plugins always run in stereo. A mono strip feeds both plugin inputs and takes the
  left output back (built-in devices return the average of both).
- Custom controls (`CustomControlsPage.*`; stored with the show by
  `livedaw-session/custom.rs`) cover the phase-1 functions: faders, pan, DCAs, bus
  sends, FX parameters, monitoring, mutes / solos, mute groups, sends on fader, pages,
  show control, scene recall, single actions, the USB PLAY/REC functions, DAW buttons /
  V-pots and MIDI CC / note / program change; spill, SD and GPIO functions are listed
  disabled (the spill layers themselves exist: DCA SPILL, the SENDS screens' SPILL and
  FX SPILL use them).
- LIBRARY: scenes and snippets live in the show (no stand-alone `.snap` / `.snip`
  files, so ADD ITEM TO SHOW is disabled); 10 content sections instead of the
  console's 17 (CONTENTS keys sharing a section light together); the SCOPE window's
  SFC, PREFS and L / C / R tiles (surface and preferences in scenes) are disabled; no
  CLIP player or SEND MIDI.
- Not yet: remote preamp control (HA REMOTE, RCV CUST; drawn disabled) and the RTA on
  MONITORS EQ (the analyser follows strips).
- Recorder: no transport without a running audio device (play stays in Starting);
  a loop change while rolling plays the old content for ~50 ms before the
  crossfade; take files are flushed every 2 s but only synced to disk when
  finalized; no latency compensation of takes (the UI doesn't shift them yet).
- Projects: one tempo and time signature for the whole timeline (for the grid only);
  Save As keeps referring to the old folder's audio instead of copying it; playback
  assumes the project's sample rate is the console's (a mismatch is flagged).
- Shown on the screens but disabled until the engine supports them (the WING manual
  describes them): MIDI CC faders in custom layers.
