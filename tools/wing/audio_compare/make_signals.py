#!/usr/bin/env python3
"""Writes the test signals for comparing a WING's processing with Sidecar's.

Every file is 48 kHz, 24-bit, mono:

    [0.5 s silence] [sync chirp, rising 0.5 s] [test ID code] [content...]
    [sync chirp, falling 0.5 s] [1 s silence]

The two chirps let analyze.py find the file in a recording (latency) and
measure the recorder's clock against the player's (drift); the ID code (12
short tones) tells which test a recording is, so recordings can be sent back
in any order and with any name.

The content depends on the test's signal kind (see KINDS):

  lin    log sweep 20 Hz - 20 kHz (Farina) at -20 dBFS, pink noise, 1 kHz tone
  dyn    2 s of silence, 1 kHz level steps -60 .. 0 dBFS (2 dB, 0.5 s each), then level
         jumps -50 / -10 dBFS for attack, hold and release
  key    1/3-octave tone steps 31.5 Hz - 16 kHz at -10 dBFS (key filter, crossover)
  fx     clicks, a log sweep with a 5 s tail, pink noise, 1 kHz tone
  mod    a 10 s 1 kHz tone (modulation rate and depth), pink noise
  drive  sweeps at -20 and -6 dBFS, 1 kHz steps -40 .. -4 dBFS, pink noise
  music  a synthetic band (drums, bass, chords), made here: no recorded audio

All signals are computed (seeded noise), so analyze.py rebuilds them exactly
instead of reading these files.

  python3 make_signals.py [--out signals] [--plan test_plan.json] [--only NAME ...] [--priority N]

writes signals/tests/NN_<test>.wav (one per test of the plan, the file to
play for that test), signals/kinds/<kind>.wav (each kind once) and
signals/everything.wav (every kind in a row) with its timeline printed and
saved as signals/everything.json.
"""

import argparse
import json
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from wavio import write_wav  # noqa: E402

SR = 48000
LEAD_S = 0.5
CHIRP_S = 0.5
CHIRP_F = (200.0, 12000.0)
CHIRP_DB = -12.0
ID_BITS = 12
ID_TONE_S = 0.04
ID_GAP_S = 0.01
ID_FREQS = (1000.0, 2000.0)
ID_AT_S = LEAD_S + CHIRP_S + 0.3
CONTENT_AT_S = ID_AT_S + ID_BITS * (ID_TONE_S + ID_GAP_S) + 0.5
END_GAP_S = 0.5
TAIL_S = 1.0

SWEEP_F = (20.0, 20000.0)
SWEEP_S = 10.0
THIRD_OCTAVES = [31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630, 800, 1000, 1250, 1600,
                 2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000]


def db(x):
    return 10.0 ** (x / 20.0)


def fade(x, ms=5.0, sr=SR):
    n = min(int(ms * 1e-3 * sr), len(x) // 2)
    if n > 0:
        w = 0.5 - 0.5 * np.cos(np.pi * np.arange(n) / n)
        x[:n] *= w
        x[-n:] *= w[::-1]
    return x


def sync_chirp(rising=True, sr=SR, stretch=1.0):
    """Exponential chirp between CHIRP_F (rising for the start, falling for the end);
    `stretch` > 1 plays it slower (as a recorder with a fast clock captures it)."""
    n = int(round(CHIRP_S * sr * stretch))
    t = np.arange(n) / sr / stretch
    f1, f2 = CHIRP_F if rising else CHIRP_F[::-1]
    k = np.log(f2 / f1) / CHIRP_S
    x = np.sin(2 * np.pi * f1 * (np.exp(k * t) - 1.0) / k)
    return fade(x * db(CHIRP_DB), 5.0, sr)


def id_check(code):
    return (code * 7 + 3) % 16


def id_bits(code):
    word = (code & 0xFF) << 4 | id_check(code & 0xFF)
    return [(word >> (ID_BITS - 1 - i)) & 1 for i in range(ID_BITS)]


def id_code(code, sr=SR):
    """The test ID as 12 tones (8 bits and a 4-bit check, MSB first)."""
    out = []
    n = int(ID_TONE_S * sr)
    t = np.arange(n) / sr
    for b in id_bits(code):
        out.append(fade(np.sin(2 * np.pi * ID_FREQS[b] * t) * db(CHIRP_DB), 3.0, sr))
        out.append(np.zeros(int(ID_GAP_S * sr)))
    return np.concatenate(out)


def decode_id(x, at, sr=SR):
    """Reads the ID code from samples `x` whose code starts at sample `at`
    (None if the check fails)."""
    n = int(ID_TONE_S * sr)
    step = int((ID_TONE_S + ID_GAP_S) * sr)
    word = 0
    for i in range(ID_BITS):
        a = int(round(at)) + i * step + n // 8
        seg = x[a:a + n * 3 // 4]
        if len(seg) < n // 2:
            return None
        t = np.arange(len(seg)) / sr
        e = [abs(np.dot(seg, np.exp(-2j * np.pi * f * t))) for f in ID_FREQS]
        if max(e) < 1e-6 * len(seg) or max(e) < 4.0 * min(e):
            return None  # no clear tone: not a code
        word = word << 1 | (1 if e[1] > e[0] else 0)
    code, check = word >> 4, word & 0xF
    return code if id_check(code) == check else None


# ---- content -------------------------------------------------------------------

def log_sweep(level_db=-20.0, seconds=SWEEP_S, f=SWEEP_F, sr=SR):
    """Farina exponential sweep (constant amplitude) with 20 ms fades."""
    n = int(seconds * sr)
    t = np.arange(n) / sr
    L = seconds / np.log(f[1] / f[0])
    x = np.sin(2 * np.pi * f[0] * L * (np.exp(t / L) - 1.0))
    return fade(x, 20.0, sr) * db(level_db)


def inverse_sweep(seconds=SWEEP_S, f=SWEEP_F, sr=SR):
    """Inverse filter of log_sweep (time-reversed, rising 6 dB/oct), not normalised."""
    n = int(seconds * sr)
    t = np.arange(n) / sr
    L = seconds / np.log(f[1] / f[0])
    x = fade(np.sin(2 * np.pi * f[0] * L * (np.exp(t / L) - 1.0)), 20.0, sr)
    # Reversed, the sweep starts at the top: +6 dB/oct against the sweep's pink spectrum.
    return x[::-1] * np.exp(-t / L)


def sweep_harmonic_offset(k, seconds=SWEEP_S, f=SWEEP_F):
    """How much earlier than the linear response harmonic k appears (s)."""
    return seconds / np.log(f[1] / f[0]) * np.log(k)


def pink_noise(seconds, level_db=-20.0, seed=1, sr=SR):
    """Pink noise (1/f power, 20 Hz - 20 kHz), `level_db` RMS, peaks clipped at 4x RMS."""
    n = int(seconds * sr)
    rng = np.random.default_rng(seed)
    spec = np.fft.rfft(rng.standard_normal(n))
    fr = np.fft.rfftfreq(n, 1.0 / sr)
    shape = np.zeros_like(fr)
    band = (fr >= 20.0) & (fr <= 20000.0)
    shape[band] = 1.0 / np.sqrt(fr[band])
    x = np.fft.irfft(spec * shape, n)
    x *= db(level_db) / np.sqrt(np.mean(x * x))
    lim = 4.0 * db(level_db)
    return fade(np.clip(x, -lim, lim), 20.0, sr)


def tone(freq, seconds, level_db, sr=SR, ramp_ms=5.0):
    n = int(seconds * sr)
    return fade(np.sin(2 * np.pi * freq * np.arange(n) / sr) * db(level_db), ramp_ms, sr)


def level_steps(levels, step_s=0.5, freq=1000.0, sr=SR):
    """A continuous sine (whole cycles per step) whose level jumps between steps."""
    n = int(step_s * sr)
    t = np.arange(n * len(levels)) / sr
    amp = np.repeat([db(v) for v in levels], n)
    return fade(np.sin(2 * np.pi * freq * t) * amp, 2.0, sr)


def freq_steps(freqs, level_db, step_s=0.4, sr=SR):
    return np.concatenate([tone(f, step_s, level_db, sr, ramp_ms=3.0) for f in freqs])


def clicks(count=3, spacing_s=4.0, level_db=-6.0, sr=SR):
    """Single-sample impulses `spacing_s` apart (the first at 0)."""
    x = np.zeros(int(count * spacing_s * sr))
    x[(np.arange(count) * spacing_s * sr).astype(int)] = db(level_db)
    return x


def music(seconds=12.0, seed=7, sr=SR):
    """A deterministic synthetic band at 120 BPM: kick, snare, hi-hat, bass, chords."""
    rng = np.random.default_rng(seed)
    n = int(seconds * sr)
    out = np.zeros(n)
    beat = int(0.5 * sr)

    def add(at, sig):
        end = min(n, at + len(sig))
        if at < n:
            out[at:end] += sig[:end - at]

    def env(seconds_, decay):
        t = np.arange(int(seconds_ * sr)) / sr
        return t, np.exp(-t / decay)

    t, e = env(0.4, 0.09)
    kick = np.sin(2 * np.pi * (45 * t + (120 - 45) * 0.03 * (1 - np.exp(-t / 0.03)))) * e
    t, e = env(0.25, 0.05)
    snare = (rng.standard_normal(len(t)) * 0.6 + np.sin(2 * np.pi * 185 * t)) * e
    t, e = env(0.06, 0.012)
    hat = np.diff(rng.standard_normal(len(t) + 1)) * 0.5 * e
    for b in range(int(seconds / 0.5)):
        add(b * beat, 0.9 * kick if b % 2 == 0 else 0.5 * snare)
        add(b * beat, 0.25 * hat)
        add(b * beat + beat // 2, 0.15 * hat)
    roots = [55.0, 43.65, 65.41, 49.0]  # A1 F1 C2 G1
    chords = [[220.0, 261.63, 329.63], [174.61, 220.0, 261.63], [261.63, 329.63, 392.0], [196.0, 246.94, 293.66]]
    bar = 4 * beat
    for i in range(int(seconds / 2.0)):
        r, ch = roots[i % 4], chords[i % 4]
        for q in range(8):
            t, e = env(0.24, 0.12)
            note = sum(np.sin(2 * np.pi * r * (1 + (q % 4 == 3)) * h * t) / h for h in range(1, 12) if r * h < 4000)
            add(i * bar + q * beat // 2, 0.35 * note * e)
        t = np.arange(bar) / sr
        pad_env = np.minimum(1.0, t / 0.3) * np.minimum(1.0, (bar / sr - t) / 0.2)
        pad = sum(np.sin(2 * np.pi * f * t) + np.sin(2 * np.pi * f * 1.003 * t) for f in ch)
        add(i * bar, 0.05 * pad * pad_env)
    out *= db(-6.0) / np.max(np.abs(out))
    return fade(out, 10.0, sr)


# ---- kinds ---------------------------------------------------------------------
# Each kind is a list of segments: (name, generator, parameters, silence after (s)).

LEVELS_DYN = list(range(-60, 1, 2))
LEVELS_THD = list(range(-40, -3, 4))
ENVELOPE = {"low": -50.0, "high": -10.0, "low_s": 1.0, "high_s": 1.0, "after_s": 2.0, "repeats": 3}


def envelope(low=-50.0, high=-10.0, low_s=1.0, high_s=1.0, after_s=2.0, repeats=3, freq=1000.0, sr=SR):
    """1 kHz jumping between `low` and `high` dBFS: [low][high][low after] x repeats."""
    levels, durs = [], []
    for _ in range(repeats):
        levels += [low, high, low]
        durs += [low_s, high_s, after_s]
    amp = np.concatenate([np.full(int(d * sr), db(v)) for v, d in zip(levels, durs)])
    t = np.arange(len(amp)) / sr
    return fade(np.sin(2 * np.pi * freq * t) * amp, 2.0, sr)


GENERATORS = {
    "silence": lambda seconds=1.0: np.zeros(int(seconds * SR)),
    "sweep": lambda level_db=-20.0: log_sweep(level_db),
    "pink": lambda seconds=4.0, level_db=-20.0, seed=1: pink_noise(seconds, level_db, seed),
    "tone": lambda freq=1000.0, seconds=2.0, level_db=-20.0: tone(freq, seconds, level_db),
    "levels": lambda levels=LEVELS_DYN, step_s=0.5: level_steps(levels, step_s),
    "envelope": lambda **kw: envelope(**kw),
    "stepfreq": lambda freqs=THIRD_OCTAVES, level_db=-10.0, step_s=0.4: freq_steps(freqs, level_db, step_s),
    "clicks": lambda count=3, spacing_s=4.0, level_db=-6.0: clicks(count, spacing_s, level_db),
    "music": lambda seconds=12.0: music(seconds),
}

KINDS = {
    "lin": [("sweep", "sweep", {"level_db": -20.0}, 2.0),
            ("pink", "pink", {"seconds": 4.0, "level_db": -20.0}, 0.5),
            ("tone", "tone", {"freq": 1000.0, "seconds": 2.0, "level_db": -20.0}, 0.5)],
    # 2 s of silence first: a gate or compressor settles (closes, releases) before the steps.
    "dyn": [("settle", "silence", {"seconds": 2.0}, 0.0),
            ("levels", "levels", {"levels": LEVELS_DYN, "step_s": 0.5}, 1.0),
            ("envelope", "envelope", dict(ENVELOPE), 0.5)],
    "key": [("stepfreq", "stepfreq", {"freqs": THIRD_OCTAVES, "level_db": -10.0, "step_s": 0.4}, 1.0)],
    "fx": [("clicks", "clicks", {"count": 3, "spacing_s": 4.0, "level_db": -6.0}, 0.0),
           ("sweep", "sweep", {"level_db": -20.0}, 5.0),
           ("pink", "pink", {"seconds": 4.0, "level_db": -20.0}, 3.0),
           ("tone", "tone", {"freq": 1000.0, "seconds": 4.0, "level_db": -20.0}, 3.0)],
    "drive": [("sweep", "sweep", {"level_db": -20.0}, 2.0),
              ("sweep_hot", "sweep", {"level_db": -6.0}, 2.0),
              ("levels", "levels", {"levels": LEVELS_THD, "step_s": 0.5}, 1.0),
              ("pink", "pink", {"seconds": 4.0, "level_db": -20.0}, 0.5)],
    "mod": [("tone", "tone", {"freq": 1000.0, "seconds": 10.0, "level_db": -20.0}, 1.0),
            ("pink", "pink", {"seconds": 4.0, "level_db": -20.0}, 1.0)],
    "music": [("music", "music", {"seconds": 12.0}, 1.0)],
}

KIND_CODES = {k: 200 + i for i, k in enumerate(KINDS)}


def build(kind, code, sr=SR):
    """The signal of a kind with an ID code, and its timeline (sample positions)."""
    if sr != SR:
        raise ValueError("the kit works at 48 kHz")
    parts = [np.zeros(int(LEAD_S * sr)), sync_chirp(True, sr)]
    pos = sum(len(p) for p in parts)
    gap = int(ID_AT_S * sr) - pos
    parts.append(np.zeros(gap))
    parts.append(id_code(code, sr))
    pos = sum(len(p) for p in parts)
    parts.append(np.zeros(int(CONTENT_AT_S * sr) - pos))
    pos = int(CONTENT_AT_S * sr)
    segments = []
    for name, gen, params, after in KINDS[kind]:
        x = GENERATORS[gen](**params)
        segments.append({"name": name, "generator": gen, "params": params, "start": pos, "length": len(x),
                         "after": int(after * sr)})
        parts += [x, np.zeros(int(after * sr))]
        pos += len(x) + int(after * sr)
    parts.append(np.zeros(int(END_GAP_S * sr)))
    end_chirp = pos + int(END_GAP_S * sr)
    parts += [sync_chirp(False, sr), np.zeros(int(TAIL_S * sr))]
    x = np.concatenate(parts)
    timeline = {
        "kind": kind, "code": code, "sample_rate": sr, "length": len(x),
        "start_chirp": int(LEAD_S * sr), "end_chirp": end_chirp, "chirp_length": int(CHIRP_S * sr),
        "id_at": int(ID_AT_S * sr), "segments": segments,
    }
    return x, timeline


def segment(timeline, name):
    for s in timeline["segments"]:
        if s["name"] == name:
            return s
    raise KeyError(name)


def load_plan(path=None):
    with open(path or os.path.join(HERE, "test_plan.json")) as f:
        return json.load(f)


def test_signal(test):
    """The signal and timeline a test of the plan plays."""
    return build(test["signal"], test["code"])


def file_name(test):
    return f"{test['code']:02d}_{test['name']}.wav"


def fmt_time(samples, sr=SR):
    s = samples / sr
    return f"{int(s // 60)}:{s % 60:06.3f}"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default="signals", help="output folder (default: signals)")
    ap.add_argument("--plan", default=os.path.join(HERE, "test_plan.json"))
    ap.add_argument("--only", nargs="*", help="only these tests")
    ap.add_argument("--priority", type=int, help="only tests up to this priority (1 = most important)")
    ap.add_argument("--no-everything", action="store_true", help="skip everything.wav")
    ap.add_argument("--stereo", action="store_true", help="stereo files (the signal on both sides)")
    a = ap.parse_args(argv)
    plan = load_plan(a.plan)
    os.makedirs(os.path.join(a.out, "tests"), exist_ok=True)
    os.makedirs(os.path.join(a.out, "kinds"), exist_ok=True)
    tests = [t for t in plan["tests"] if (not a.only or t["name"] in a.only)
             and (a.priority is None or t["priority"] <= a.priority)]
    total = 0
    for t in tests:
        x, tl = test_signal(t)
        path = os.path.join(a.out, "tests", file_name(t))
        write_wav(path, np.stack([x, x], axis=1) if a.stereo else x, SR, 24)
        total += len(x)
    print(f"{len(tests)} test files in {os.path.join(a.out, 'tests')} ({fmt_time(total)} in all)")
    for kind in KINDS:
        x, _ = build(kind, KIND_CODES[kind])
        write_wav(os.path.join(a.out, "kinds", f"{kind}.wav"), x, SR, 24)
    if not a.no_everything:
        parts, timeline, pos = [], [], 0
        for kind in KINDS:
            x, tl = build(kind, KIND_CODES[kind])
            for s in tl["segments"]:
                timeline.append({"kind": kind, "segment": s["name"], "start_s": round((pos + s["start"]) / SR, 4),
                                 "length_s": round(s["length"] / SR, 4), "params": s["params"]})
            parts.append(x)
            pos += len(x)
        write_wav(os.path.join(a.out, "everything.wav"), np.concatenate(parts), SR, 24)
        with open(os.path.join(a.out, "everything.json"), "w") as f:
            json.dump(timeline, f, indent=1)
        print(f"everything.wav ({fmt_time(pos)}):")
        for e in timeline:
            print(f"  {fmt_time(e['start_s'] * SR)}  {e['kind']:6} {e['segment']:10} {e['length_s']:7.2f} s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
