# WING vs Sidecar: comparing the sound

A kit for recording a Behringer WING's channel processing (EQ, filters, gate,
compressor and the other dynamics models, FX) with test signals, rendering
Sidecar's processing of the same signals with the same settings, and reporting
where they differ. At the console you only record the WING: Sidecar's side is
rendered offline by its own engine (`render`, an example of `livedaw-wing`).

| File | What it does |
|---|---|
| `test_plan.md` | The tests in order of importance: signal file, settings, where each control is on the console, time estimate. |
| `test_plan.json` | The same for the scripts (made by `plan.py` from the definitions in it). |
| `make_signals.py` | Writes the test signals (48 kHz, 24-bit mono WAV with sync chirps and an ID code). |
| `wing_osc.py` | Optional: sets the WING up for a test over OSC and reads the values back. |
| `analyze.py` | Compares one WING recording with Sidecar's render; markdown report with PASS / FAIL. |
| `run_all.py` | Renders Sidecar for a folder of recordings, analyzes them all, writes one summary. |
| `tests/` | Self-tests (`python3 -m unittest -v`). |

## Install

- Python 3.8 or newer with **numpy** and **scipy**: `python3 -m pip install numpy scipy`.
  Optional: **matplotlib** (PNG plots in the reports) and **soundfile** (faster WAV
  reading; without it the kit reads 16/24/32-bit PCM and 32-bit float WAV itself).
- For `run_all.py` (Sidecar's side): the Rust toolchain that builds this repository. It
  builds the render tool on first use (`cargo build --release -p livedaw-wing --example render`
  in `rust/`). Only needed where the recordings are analyzed, not at the console.

## Tomorrow at the console

1. **Before**, on the computer: `python3 make_signals.py` (writes `signals/`, about 250
   MB for all tests; `--priority 1` for the most important 28 only). Copy
   `signals/tests/` to a USB stick for the WING. Optional check of the kit:
   `python3 run_all.py --dry-run --priority 1` (should report every test as PASS).
2. **At the WING**: save your show, then SETUP > INITIALIZE CONSOLE (Main 1 must hear
   channel 1 only). Plug in the stick.
3. **Patch once** (test_plan.md, "Before the first test"): channel 1's input = USB
   PLAYER 1; USB recorder inputs 1-2 = MAIN 1 L / R, 3 = USB PLAYER 1; recorder 4-CH, 24
   bit, WAV. With the computer on the WING's network, `python3 wing_osc.py WING_IP setup`
   does all of it and the common settings, and prints what the console reports.
4. **For each test**, in the order of test_plan.md (stop whenever you like):
   - `python3 wing_osc.py WING_IP apply TEST_NAME` (or dial the settings in by hand from
     test_plan.md and put them back afterwards);
   - press REC on the USB recorder, play the test's file `NN_TEST_NAME.wav` on the USB
     player, press STOP when the file has ended.
   Priority 1 takes about 25 minutes with `wing_osc.py`, 80 minutes by hand; everything
   about 1 hour / 3 hours.
5. **After**: copy the recordings from the stick (`R_....wav`, any names: every recording
   carries its test's ID code) into a folder `wing_recordings/`, together with the
   `wing_settings/` folder `wing_osc.py` wrote.

Don't touch the trim, fader or Main fader during the session; the signals peak at -6 dBFS
(the dynamics level steps reach 0 dBFS), so nothing clips.

## Sending the recordings back

Zip `wing_recordings/` (WAV files and `wing_settings/`) and share it, with the WING's
firmware version and anything unusual. A test recorded twice is fine: the newest
recording (the last by file name, which for the recorder's `R_date-time` names is the
latest) is used. If you can, run `python3 run_all.py wing_recordings` first and include the `results-...`
folder's `summary.md`. Do not commit the WAV files (they are ignored by git).

## How the analysis works

- **Alignment.** Every file starts with a rising and ends with a falling chirp. Their
  positions in the recording (matched filter, sub-sample) give the latency and the clock
  drift between player and recorder; the recording is resampled onto the file's
  timeline. Recorder input 3 (the unprocessed player signal) is the reference: it
  calibrates the level (player / recorder offsets cancel) and is what the transfer
  functions are measured against. Without it the processed output is used (a warning in
  the report: levels then include any capture offset). The 12-tone ID code after the
  first chirp tells which test a recording is.
- **lin** (EQ, filters, baseline): transfer function from a 20 Hz - 20 kHz log sweep
  (Farina deconvolution divided by the reference's), magnitude and phase smoothed to
  1/12 octave, compared within 20 dB of the peak (±0.25 dB, phase ±10° after removing
  a pure delay) and 20-40 dB below it (±1 dB); harmonics from the sweep's harmonic
  responses; 1 kHz level and L/R balance (pan law).
- **dyn** (gate, compressor, other dynamics): output level for 1 kHz steps from -60 to 0
  dBFS (±0.5 dB per step; ±3 dB where a gate is closed; nothing below the noise floor),
  and the 10 / 63 / 90 % times of the gain change after 40 dB level jumps (±20 % or 2
  ms): attack on the way up, hold and release on the way down.
- **key**: gain per 1/3-octave tone at -10 dBFS (key filters, crossover, de-esser).
- **reverb**: impulse response from the sweep: per octave EDT, RT60 (T30, else T20),
  C80 and level; arrival time; decay of three clicks.
- **delay**: echo times (±0.5 ms) and levels (±0.5 dB), first echo's response.
- **mod**: LFO rate (±5 %) and depth (±20 %) from a 1 kHz tone, pink-noise spectrum.
- **drive**: magnitude at -20 and -6 dBFS, harmonics, output level and THD per 1 kHz step.
- **music**: level and 1/3-octave spectrum of a synthetic band (no recorded audio);
  every report also gives the null depth (WING minus Sidecar).

Tolerances are in `plan.py` (`TOLERANCES`).

```
python3 analyze.py wing_recordings/R_0001.wav results/sidecar/eq_peq_600_p6_q1.wav \
    --test eq_peq_600_p6_q1 --report eq.md --plots plots
python3 run_all.py wing_recordings --out results
```

`run_all.py` renders Sidecar with the values the WING reported (`wing_settings/`)
where they exist, else with the plan's. The render tool on its own:

```
cd rust
cargo run --release -p livedaw-wing --example render -- \
    --settings settings.json --in signal.wav --out sidecar.wav [--set /ch/1/eq/2g=7] [--print]
cargo run -p livedaw-wing --example render -- --list /ch/1/eq    # paths, values, ranges
```

It builds a factory-initialized console, plays the file's first channel into channel 1
and writes Main 1 L/R plus the input (channel 3); the settings are WING paths applied
through Sidecar's WING remote mapping, so a value lands on the console's step as it
does on the WING.

## Other ways to record

- **USB audio** (a DAW on a computer connected to the WING's USB port): play the file to
  USB 1 and patch channel 1's input to USB 1; send MAIN 1 L / R and USB 1 back on USB
  outputs 1-3 and record the three at 48 kHz. Any clock difference is corrected.
- **Sidecar live**: Sidecar's USB player and recorder work like the WING's, with the
  same patch, if you want to record Sidecar instead of rendering it.

## Self-test

`python3 -m unittest -v` (here) checks the WAV reader, the signals and their ID codes,
the plan files, and the alignment against simulated capture artefacts (latency of
0.69 s, 40 ppm drift, -0.4 dB level, noise at -110 dBFS). The end-to-end tests render
Sidecar as both sides (they pass), then change one setting on the "WING" side (EQ band
gain +1 dB, compressor ratio 4:1 to 5:1, gate release, delay time) and check that the
report flags exactly that. They need the render tool (`SIDECAR_RENDER=path`, or cargo
to build it).

All signals are computed here; no Behringer software, firmware or recorded audio is
used.
