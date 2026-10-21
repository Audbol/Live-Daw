#!/usr/bin/env python3
"""Compares a recording of the WING with Sidecar's render of the same test.

  python3 analyze.py WING.wav SIDECAR.wav --test NAME [--plan test_plan.json]
                     [--report out.md] [--plots DIR] [--json out.json]

Both files are captures of the test's signal (make_signals.py): channels
1-2 are Main 1 L/R; channel 3, when present, is the unprocessed signal (the
WING's recorder input 3 = USB PLAYER 1; Sidecar's render writes its input
there). Each capture is put on the signal's own timeline first:

  * the rising and falling sync chirps are found in the reference channel
    (channel 3, else channel 1) by matched filtering with sub-sample
    precision; their distance against the signal's gives the clock drift,
    the start gives the latency;
  * the capture is resampled onto the timeline (band-limited), and with a
    reference channel its level is calibrated against the known signal, so
    player / recorder offsets cancel;
  * the test ID code is read and checked against --test.

Then, by the test's analysis type:

  lin     transfer function from the log sweep (Farina deconvolution, divided
          by the reference's): magnitude and phase, smoothed to 1/12 octave,
          differences per octave band, max / RMS error, harmonic distortion
          per harmonic, 1 kHz level and L/R balance, latency
  dyn     static curve from the level steps (output vs input per step, gain
          reduction), attack / hold / release from the level jumps (10 %,
          63 % and 90 % times of the gain change)
  key     gain per frequency from the tone steps (key filter, crossover)
  reverb  per octave band EDT, T20, T30, C80, level; arrival time
  delay   echo times and levels, feedback per repeat, first echo's response
  mod     LFO rate and depth from a 1 kHz tone, pink-noise spectrum
  drive   magnitude at two levels, harmonics, THD and level vs input level
  music   1/3-octave spectrum and level, null depth

The report lists each measure for the WING and Sidecar, the difference, the
tolerance (test_plan.json) and PASS / FAIL. Plots need matplotlib (optional).
"""

import argparse
import json
import math
import os
import sys

import numpy as np
from scipy import fft as sfft
from scipy import signal as sps

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import make_signals as ms  # noqa: E402
from wavio import read_wav  # noqa: E402

SR = ms.SR
OCTAVES = [31.5, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000]
RT_BANDS = [125, 250, 500, 1000, 2000, 4000, 8000]
CHECK_MAX_HZ = 19000.0
EPS = 1e-20


def db20(x):
    return 20.0 * np.log10(np.maximum(np.abs(x), EPS))


def db10(x):
    return 10.0 * np.log10(np.maximum(x, EPS))


def rms_db(x):
    """RMS as the peak level of a sine with that RMS (dBFS)."""
    return db10(np.mean(np.square(x)) * 2.0) if len(x) else -400.0


# ---- capture alignment ------------------------------------------------------------

def correlate(x, tpl):
    """Cross-correlation r[i] = sum x[i + k] tpl[k] for every start i."""
    return sps.fftconvolve(x, tpl[::-1], mode="valid")


def fine_peak(x, tpl, coarse, span=1.5):
    """Sub-sample position of `tpl` in `x` near integer `coarse`, from the
    band-limited cross-correlation (evaluated on a coarse, then a fine grid)."""
    pad = 64
    lo = coarse - pad
    seg = np.zeros(len(tpl) + 2 * pad)
    a, b = max(lo, 0), min(lo + len(seg), len(x))
    if b <= a:
        return float(coarse)
    seg[a - lo:b - lo] = x[a:b]
    n = sfft.next_fast_len(2 * len(seg))
    c = sfft.rfft(seg, n) * np.conj(sfft.rfft(tpl, n))
    keep = np.abs(c) > 1e-6 * np.max(np.abs(c))
    c, k = c[keep], np.nonzero(keep)[0]

    def best(taus):
        vals = np.array([np.real(np.sum(c * np.exp(2j * np.pi * k * t / n))) for t in taus])
        return int(np.argmax(vals)), vals

    taus = pad + np.arange(-span, span + 1e-9, 0.1)
    i, _ = best(taus)
    step = 0.005
    taus = taus[i] + np.arange(-0.1, 0.1 + 1e-9, step)
    i, vals = best(taus)
    t = taus[i]
    if 0 < i < len(vals) - 1:
        y0, y1, y2 = vals[i - 1], vals[i], vals[i + 1]
        den = y0 - 2 * y1 + y2
        if den < 0:
            t += step * 0.5 * (y0 - y2) / den
    return lo + t


def find_chirp(x, tpl, lo=0, hi=None):
    """(sub-sample start, gain) of the chirp `tpl` in x[lo:hi]."""
    hi = len(x) if hi is None else min(hi, len(x))
    lo = max(0, lo)
    if hi - lo < len(tpl):
        return None, 0.0
    r = correlate(x[lo:hi], tpl)
    i = int(np.argmax(r))
    gain = r[i] / np.dot(tpl, tpl)
    return fine_peak(x, tpl, lo + i), float(gain)


def resample_onto(x, a, b, n):
    """y[k] = x(a + b k) for k < n, band-limited (x is zero outside)."""
    if abs(b - 1.0) * n < 0.01:
        i0 = int(math.floor(a))
        frac = a - i0
        margin = 256
        seg = np.zeros(n + 2 * margin)
        s, e = i0 - margin, i0 + n + margin
        cs, ce = max(s, 0), min(e, len(x))
        if ce > cs:
            seg[cs - s:ce - s] = x[cs:ce]
        if abs(frac) > 1e-6:
            m = sfft.next_fast_len(len(seg))
            spec = sfft.rfft(seg, m)
            f = np.arange(len(spec)) / m
            seg = sfft.irfft(spec * np.exp(2j * np.pi * f * frac), m)[:len(seg)]
        return seg[margin:margin + n]
    return sinc_interp(x, a + b * np.arange(n))


_SINC = None


def _sinc_table(half=48, res=2048, cutoff=0.975, beta=10.0):
    """Kaiser-windowed sinc taps for fractions q/res: table[tap, q]."""
    global _SINC
    if _SINC is None:
        taps = np.arange(-half + 1, half + 1)
        u = taps[:, None] - np.arange(res + 1)[None, :] / res
        kaiser = np.i0(beta * np.sqrt(np.clip(1.0 - (u / half) ** 2, 0.0, 1.0))) / np.i0(beta)
        h = cutoff * np.sinc(cutoff * u) * kaiser
        _SINC = (h, half, res)
    return _SINC


def sinc_interp(x, t):
    """x at the (sorted, fractional) positions t, band-limited; zero outside x."""
    h, half, res = _sinc_table()
    xp = np.concatenate([np.zeros(half + 1), x, np.zeros(half + 1)])
    y = np.zeros(len(t))
    inside = np.nonzero((t > -half) & (t < len(x) + half - 1))[0]
    step = 1 << 18
    for c0 in range(0, len(inside), step):
        sel = inside[c0:c0 + step]
        tc = t[sel]
        i0 = np.floor(tc).astype(np.int64)
        q = np.round((tc - i0) * res).astype(np.int64)
        base = i0 + half + 1
        acc = np.zeros(len(tc))
        for j, k in enumerate(range(-half + 1, half + 1)):
            acc += xp[base + k] * h[j][q]
        y[sel] = acc
    return y


class Capture:
    """A recording put on the signal's timeline: `out` (n, 2) Main L/R and
    `ref` (n,) the signal that went in (calibrated reference channel, or the
    known signal when the capture has none)."""

    def __init__(self, path, signal, timeline, expect_code=None):
        data, sr = read_wav(path)
        if sr != SR:
            raise ValueError(f"{path}: {sr} Hz; the kit works at {SR} Hz")
        self.path = path
        self.info = {"file": os.path.basename(path), "channels": data.shape[1]}
        self.warnings = []
        n = len(signal)
        chans = [data[:, c] for c in range(data.shape[1])]
        has_ref = len(chans) >= 3 and np.sqrt(np.mean(chans[2] ** 2)) > 1e-5
        refsrc = chans[2] if has_ref else chans[0]
        self.info["reference"] = "channel 3 (unprocessed)" if has_ref else "channel 1 (processed: no reference channel)"
        if not has_ref:
            self.warnings.append("no reference channel: latency and level include the processing; "
                                 "record 4 channels with input 3 = USB PLAYER 1")
        start, g1 = find_chirp(refsrc, ms.sync_chirp(True))
        if start is None:
            raise ValueError(f"{path}: too short")
        dist = timeline["end_chirp"] - timeline["start_chirp"]
        win = int(0.003 * dist + 0.05 * SR)
        exp_end = int(round(start)) + dist
        end, g2 = find_chirp(refsrc, ms.sync_chirp(False), exp_end - win, exp_end + win + SR)
        if end is None or g2 < 0.1 * max(g1, 1e-9):
            self.warnings.append("end chirp not found (recording stopped early?): assuming no clock drift")
            b = 1.0
        else:
            # A chirp's timing leans on its high frequencies (the end of the rising
            # one, the start of the falling one), so with drift both are found
            # again with templates stretched like the recording.
            b = (end - start) / dist
            for _ in range(2):
                ws = int(round(start)) - 64
                start, _ = find_chirp(refsrc, ms.sync_chirp(True, stretch=b), ws, ws + 128 + SR)
                we = int(round(end)) - 64
                end, _ = find_chirp(refsrc, ms.sync_chirp(False, stretch=b), we, we + 128 + SR)
                b = (end - start) / dist
        a = start - b * timeline["start_chirp"]
        self.a, self.b = a, b
        self.info.update({"offset_s": a / SR, "drift_ppm": (b - 1.0) * 1e6, "chirp_gain_db": float(db20(g1))})
        code = ms.decode_id(refsrc, a + b * timeline["id_at"])
        self.info["code"] = code
        if expect_code is not None and code is not None and code != expect_code:
            self.warnings.append(f"the recording's ID is test {code}, not test {expect_code}")
        if code is None:
            self.warnings.append("test ID code not readable")
        out = [resample_onto(chans[c], a, b, n) for c in range(min(2, len(chans)))]
        if len(out) == 1:
            out.append(out[0].copy())
        self.out = np.stack(out, axis=1)
        quiet = slice(int(0.05 * SR), timeline["start_chirp"] - int(0.05 * SR))
        if has_ref:
            ref = resample_onto(chans[2], a, b, n)
            content = slice(timeline["start_chirp"], timeline["length"])
            g = np.dot(ref[content], signal[content]) / max(np.dot(signal[content], signal[content]), EPS)
            if g <= 0:
                raise ValueError(f"{path}: the reference channel does not carry the test signal")
            ref /= g
            self.out /= g
            resid = ref[content] - signal[content]
            self.info["gain_cal_db"] = float(db20(g))
            self.info["ref_null_db"] = float(db10(np.sum(resid ** 2) / max(np.sum(signal[content] ** 2), EPS)))
            if self.info["ref_null_db"] > -40:
                self.warnings.append(f"the reference channel differs from the test file "
                                     f"({self.info['ref_null_db']:.0f} dB): wrong file, or the player resamples?")
            self.ref = ref
        else:
            self.info["gain_cal_db"] = 0.0
            self.ref = signal.copy()
        self.info["noise_dbfs"] = float(rms_db(self.out[quiet, 0]))
        clip = float(np.max(np.abs(data[:, :2]))) if data.shape[1] else 0.0
        self.info["peak_dbfs"] = float(db20(clip))
        if clip >= 0.999:
            self.warnings.append("the recording clips (0 dBFS)")


# ---- measurement helpers -----------------------------------------------------------

def log_grid(f1=20.0, f2=20000.0, per_oct=48):
    n = int(round(np.log2(f2 / f1) * per_oct)) + 1
    return f1 * 2.0 ** (np.arange(n) / per_oct)


def smooth_power(freqs, p, grid, frac=12):
    """Average of power `p` (on linear `freqs`) over 1/frac octave around each grid point."""
    c = np.concatenate([[0.0], np.cumsum(p)])
    lo = np.searchsorted(freqs, grid * 2 ** (-0.5 / frac))
    hi = np.searchsorted(freqs, grid * 2 ** (0.5 / frac), side="right")
    hi = np.maximum(hi, lo + 1)
    return (c[np.minimum(hi, len(p))] - c[np.minimum(lo, len(p) - 1)]) / (hi - lo)


def smooth_complex(freqs, h, grid, frac=12):
    return smooth_power(freqs, h.real, grid, frac) + 1j * smooth_power(freqs, h.imag, grid, frac)


def deconvolve(y, seconds=ms.SWEEP_S):
    """Farina deconvolution: the linear response starts at index len(sweep) - 1."""
    inv = ms.inverse_sweep(seconds)
    return sps.fftconvolve(y, inv), len(inv) - 1


def window(ir, z, pre, post, fade_in=0.002, fade_out=None):
    pre_n, post_n = int(pre * SR), int(post * SR)
    a = z - pre_n
    seg = np.zeros(pre_n + post_n)
    s, e = max(a, 0), min(z + post_n, len(ir))
    if e > s:
        seg[s - a:e - a] = ir[s:e]
    fi = max(1, int(fade_in * SR))
    seg[:fi] *= 0.5 - 0.5 * np.cos(np.pi * np.arange(fi) / fi)
    fo = max(1, int((fade_out if fade_out is not None else post * 0.1) * SR))
    seg[-fo:] *= 0.5 + 0.5 * np.cos(np.pi * np.arange(fo) / fo)
    return seg


def sweep_responses(cap, seg, post=0.5, pre=0.02):
    """Per output channel: (freqs, H relative to the reference, H1 spectrum,
    deconvolved IR and its zero index) from a sweep segment."""
    s0, n = seg["start"], seg["length"] + seg["after"]
    ref = cap.ref[s0:s0 + n]
    ir_ref, z = deconvolve(ref)
    wr = window(ir_ref, z, pre, post)
    nfft = sfft.next_fast_len(max(1 << 17, 2 * len(wr)))
    R = sfft.rfft(wr, nfft)
    freqs = np.arange(len(R)) * SR / nfft
    res = []
    for c in range(2):
        ir, _ = deconvolve(cap.out[s0:s0 + n, c])
        w = window(ir, z, pre, post)
        H1 = sfft.rfft(w, nfft)
        res.append({"H": H1 / np.where(np.abs(R) > EPS, R, EPS), "H1": H1, "ir": ir, "z": z})
    return freqs, res, pre


def band_limited_ir(freqs, H, nfft_len, f_lo=15.0, f_hi=21000.0):
    m = np.clip((freqs - f_lo * 0.5) / (f_lo * 0.5), 0, 1) * np.clip((22500.0 - freqs) / (22500.0 - f_hi), 0, 1)
    return sfft.irfft(H * m, nfft_len)


def harmonics(freqs, r, n_harm=5, at=(100.0, 300.0, 1000.0, 3000.0)):
    """Harmonic levels (dB re the fundamental) from the Farina harmonic responses,
    with the fundamental's level re the response's peak: {name: (harmonic, fundamental)}."""
    ir, z = r["ir"], r["z"]
    nfft = 2 * (len(freqs) - 1)
    lin_p = np.abs(r["H1"]) ** 2
    peak = np.max(smooth_power(freqs, lin_p, log_grid(), 6))
    out = {}
    for k in range(2, n_harm + 1):
        zk = z - int(round(ms.sweep_harmonic_offset(k) * SR))
        w = window(ir, zk, 0.005, 0.2)
        Hk = np.abs(sfft.rfft(w, nfft)) ** 2
        for f in at:
            if k * f > 20000.0:
                continue
            num = smooth_power(freqs, Hk, np.array([k * f]), 6)[0]
            den = smooth_power(freqs, lin_p, np.array([f]), 6)[0]
            out[f"H{k} @ {f:g} Hz"] = (float(db10(num / max(den, EPS))), float(db10(den / max(peak, EPS))))
    return out


def compare_harmonics(res, hw, hs, section):
    """Harmonics above -80 dB where the fundamental is within 20 dB of the response's peak."""
    for k, (lw, fw) in hw.items():
        ls, fs = hs.get(k, (-400.0, -400.0))
        if min(fw, fs) > -20 and max(lw, ls) > -80:
            res.check(k, lw, ls, res.tol["thd_db"], section=section)


def tone_analysis(x, f0=1000.0, n_harm=9):
    """(fundamental level dBFS peak, harmonic levels dB re fundamental, THD dB)."""
    w = sps.get_window("blackmanharris", len(x))
    spec = np.abs(np.fft.rfft(x * w)) / (np.sum(w) / 2)

    def amp(f):
        i = int(round(f * len(x) / SR))
        lo, hi = max(i - 4, 0), min(i + 5, len(spec))
        return np.sqrt(np.sum(spec[lo:hi] ** 2) / 2.0) if lo < hi else 0.0

    a1 = amp(f0)
    hs = {}
    total = 0.0
    for k in range(2, n_harm + 1):
        if k * f0 >= SR / 2:
            break
        ak = amp(k * f0)
        hs[k] = float(db20(ak / max(a1, EPS)))
        total += ak * ak
    return float(db20(a1)), hs, float(db10(total / max(a1 * a1, EPS)))


def band_levels(x, ref, centres=ms.THIRD_OCTAVES, frac=3):
    """Gain of x against ref per fractional-octave band (dB), from Welch spectra."""
    f, px = sps.welch(x, SR, nperseg=8192)
    _, pr = sps.welch(ref, SR, nperseg=8192)
    out = {}
    for fc in centres:
        m = (f >= fc * 2 ** (-0.5 / frac)) & (f < fc * 2 ** (0.5 / frac))
        if m.any():
            out[fc] = float(db10(np.sum(px[m]) / max(np.sum(pr[m]), EPS)))
    return out


def moving_rms(x, n=48):
    k = np.ones(n) / n
    return np.sqrt(np.maximum(np.convolve(x * x, k, mode="same"), EPS))


def fractional_delay(x, y):
    """Delay (samples, sub-sample) of y against x from the cross-spectrum."""
    n = sfft.next_fast_len(len(x) + len(y))
    r = sfft.irfft(sfft.rfft(y, n) * np.conj(sfft.rfft(x, n)), n)
    i = int(np.argmax(r))
    lag = i if i < n // 2 else i - n
    y0, y1, y2 = r[(i - 1) % n], r[i], r[(i + 1) % n]
    den = y0 - 2 * y1 + y2
    return lag + (0.5 * (y0 - y2) / den if den < 0 else 0.0)


def null_depth(w, s):
    """Residual of WING minus Sidecar after aligning them (dB re the WING)."""
    d = fractional_delay(w, s)
    if abs(d) > SR * 0.05:
        d = 0.0
    s2 = resample_onto(s, d, 1.0, len(s))
    return float(db10(np.sum((w - s2) ** 2) / max(np.sum(w ** 2), EPS))), float(d)


# ---- results ---------------------------------------------------------------------

class Result:
    def __init__(self, test, tol):
        self.test = test
        self.tol = tol
        self.rows = []
        self.tables = []
        self.series = {}
        self.notes = []

    def check(self, metric, w, s, tol, unit="dB", rel=False, section=""):
        """A compared measure (tol None: information only)."""
        if w is None or s is None or not np.isfinite(w) or not np.isfinite(s):
            self.rows.append({"section": section, "metric": metric, "wing": w, "sidecar": s, "diff": None,
                              "tol": tol, "unit": unit, "ok": None})
            return None
        diff = float(s - w)
        if tol is None:
            ok = None
        elif rel:
            ok = bool(abs(diff) <= tol * max(abs(w), EPS))
        else:
            ok = bool(abs(diff) <= tol + 1e-9)
        self.rows.append({"section": section, "metric": metric, "wing": float(w), "sidecar": float(s), "diff": diff,
                          "tol": tol, "unit": unit, "rel": rel, "ok": ok})
        return ok

    def check_diff(self, metric, diff, tol, unit="dB", section=""):
        """A measure that is itself a difference (largest error, residual delay)."""
        ok = None if tol is None else bool(abs(float(diff)) <= tol + 1e-9)
        self.rows.append({"section": section, "metric": metric, "wing": None, "sidecar": None, "diff": float(diff),
                          "tol": tol, "unit": unit, "ok": ok})
        return ok

    def counts(self):
        checked = [r for r in self.rows if r["ok"] is not None]
        failed = [r for r in checked if not r["ok"]]
        return len(checked), len(failed)

    def worst(self):
        failed = [r for r in self.rows if r["ok"] is False]
        if not failed:
            return ""

        def excess(r):
            t = r["tol"] * max(abs(r["wing"]), EPS) if r.get("rel") else r["tol"]
            return abs(r["diff"]) / max(t, EPS)

        r = max(failed, key=excess)
        return f"{r['section']} {r['metric']}: {r['diff']:+.3g} {r['unit']}".strip()


# ---- analyses ----------------------------------------------------------------------

def analyze_sweep(res, W, S, seg, label, mag_tol, mag_skirt_tol, phase_tol, post=0.5, with_phase=True):
    grid = log_grid()
    fw, rw, _ = sweep_responses(W, seg, post)
    fs, rs, _ = sweep_responses(S, seg, post)
    out = {}
    for c, name in ((0, "L"), (1, "R")):
        Hw, Hs = rw[c]["H"], rs[c]["H"]
        mw = db10(smooth_power(fw, np.abs(Hw) ** 2, grid))
        mss = db10(smooth_power(fs, np.abs(Hs) ** 2, grid))
        out[name] = (mw, mss)
    mw, mss = out["L"]
    diff = mss - mw
    pw, pss = mw - mw.max(), mss - mss.max()
    # Checked up to 19 kHz: the sweep ends at 20 kHz.
    audible = grid <= CHECK_MAX_HZ
    main = (pw > -20) & (pss > -20) & audible
    skirt = ~main & (pw > -40) & (pss > -40) & audible
    res.series[f"{label}: magnitude"] = {"x": grid.tolist(), "wing": mw.tolist(), "sidecar": mss.tolist(),
                                         "xlabel": "Hz", "ylabel": "dB", "logx": True}
    sec = f"{label} magnitude"
    if main.any():
        res.check_diff("largest difference (within 20 dB of peak)", diff[main][np.argmax(np.abs(diff[main]))], mag_tol,
                       section=sec)
        res.check_diff("RMS difference (within 20 dB of peak)", np.sqrt(np.mean(diff[main] ** 2)), mag_tol,
                       section=sec)
    if skirt.any():
        res.check_diff("largest difference (20-40 dB below peak)", diff[skirt][np.argmax(np.abs(diff[skirt]))],
                       mag_skirt_tol, section=sec)
    i_max = int(np.argmax(np.where(main, np.abs(diff), -1)))
    res.notes.append(f"{label}: largest magnitude difference {diff[i_max]:+.2f} dB at {grid[i_max]:.0f} Hz.")
    rows = []
    for fc in OCTAVES:
        m = (grid >= fc / np.sqrt(2)) & (grid < fc * np.sqrt(2))
        if m.any():
            rows.append([f"{fc:g}", f"{np.mean(mw[m]):+.2f}", f"{np.mean(mss[m]):+.2f}", f"{np.mean(diff[m]):+.2f}",
                         f"{np.max(np.abs(diff[m])):.2f}"])
    res.tables.append({"title": f"{label}: magnitude per octave band (L)",
                       "head": ["Octave (Hz)", "WING dB", "Sidecar dB", "Sidecar - WING", "largest diff"], "rows": rows})
    lw, rwd = out["L"][0], out["R"][0]
    ls, rsd = out["L"][1], out["R"][1]
    res.check("L/R balance (mean R - L, within 20 dB of peak)", float(np.mean((rwd - lw)[main])) if main.any() else 0,
              float(np.mean((rsd - ls)[main])) if main.any() else 0, res.tol["level_db"], section=sec)
    if with_phase:
        Hw, Hs = rw[0]["H"], rs[0]["H"]
        ratio = Hs * np.conj(Hw)
        ratio /= np.maximum(np.abs(ratio), EPS)
        dph = np.angle(smooth_complex(fw, ratio, grid))
        band = main & (grid >= 100) & (grid <= 10000)
        un = np.unwrap(dph)
        un -= np.round(un[np.argmin(np.abs(grid - 1000.0))] / (2 * np.pi)) * 2 * np.pi
        tau = -np.sum(un[band] * grid[band]) / (2 * np.pi * np.sum(grid[band] ** 2)) if band.sum() > 4 else 0.0
        resid = np.degrees(np.angle(np.exp(1j * (un + 2 * np.pi * grid * tau))))
        res.series[f"{label}: phase difference"] = {"x": grid.tolist(), "sidecar - wing": resid.tolist(),
                                                    "xlabel": "Hz", "ylabel": "degrees", "logx": True}
        res.check_diff("relative delay", tau * 1e6, None, unit="us", section=f"{label} phase")
        if main.any():
            res.check_diff("largest phase difference after the delay (within 20 dB of peak)",
                           resid[main][np.argmax(np.abs(resid[main]))], phase_tol, unit="deg", section=f"{label} phase")
        for cap, r, who in ((W, rw[0], "WING"), (S, rs[0], "Sidecar")):
            nfft = 2 * (len(fw) - 1)
            ir = band_limited_ir(fw, r["H"], nfft)
            ir = np.concatenate([ir[-int(0.02 * SR):], ir[:int(0.5 * SR)]])
            i = int(np.argmax(np.abs(ir)))
            res.series.setdefault("latency_samples", {})[who] = i - int(0.02 * SR)
    return rw, rs, fw


def analyze_lin(res, W, S, tl):
    t = res.tol
    seg = ms.segment(tl, "sweep")
    rw, rs, fw = analyze_sweep(res, W, S, seg, "sweep", t["mag_db"], t["mag_db_skirt"], t["phase_deg"])
    lat = res.series.pop("latency_samples", {})
    res.check("latency, output vs reference", lat.get("WING", 0) / SR * 1e3, lat.get("Sidecar", 0) / SR * 1e3, None,
              unit="ms", section="timing")
    compare_harmonics(res, harmonics(fw, rw[0]), harmonics(fw, rs[0]), "harmonics (sweep)")
    tone_seg = ms.segment(tl, "tone")
    a, n = tone_seg["start"] + int(0.25 * SR), tone_seg["length"] - int(0.5 * SR)
    for c, name in ((0, "L"), (1, "R")):
        lw, hw1, thw = tone_analysis(W.out[a:a + n, c])
        ls, hs1, ths = tone_analysis(S.out[a:a + n, c])
        lin, _, _ = tone_analysis(W.ref[a:a + n])
        res.check(f"1 kHz gain {name}", lw - lin, ls - lin, t["level_db"], section="1 kHz tone")
        if max(thw, ths) > -80:
            res.check(f"THD {name}", thw, ths, t["thd_db"], section="1 kHz tone")
    pink = ms.segment(tl, "pink")
    p0, pn = pink["start"], pink["length"]
    bw = band_levels(W.out[p0:p0 + pn, 0], W.ref[p0:p0 + pn])
    bs = band_levels(S.out[p0:p0 + pn, 0], S.ref[p0:p0 + pn])
    res.tables.append({"title": "pink noise: gain per 1/3 octave (L, information)",
                       "head": ["Band (Hz)", "WING dB", "Sidecar dB", "Sidecar - WING"],
                       "rows": [[f"{f:g}", f"{bw[f]:+.2f}", f"{bs[f]:+.2f}", f"{bs[f] - bw[f]:+.2f}"] for f in bw if f in bs]})


def step_levels(cap, start, levels, step_n, ch=0, lo=0.6, hi=0.95):
    out, inp = [], []
    for i in range(len(levels)):
        a = start + i * step_n + int(lo * step_n)
        b = start + i * step_n + int(hi * step_n)
        out.append(rms_db(cap.out[a:b, ch]))
        inp.append(rms_db(cap.ref[a:b]))
    return np.array(inp), np.array(out)


def analyze_static(res, W, S, seg, section="static curve"):
    t = res.tol
    levels = seg["params"]["levels"]
    step_n = int(seg["params"]["step_s"] * SR)
    iw, ow = step_levels(W, seg["start"], levels, step_n)
    is_, os_ = step_levels(S, seg["start"], levels, step_n)
    floor = res.floor_db
    rows = []
    for k, lev in enumerate(levels):
        gw, gs = ow[k] - iw[k], os_[k] - is_[k]
        if max(ow[k], os_[k]) < floor:
            ok, mark = None, "noise"
        else:
            closed = gw < -40 or gs < -40
            tol = t["static_db_closed"] if closed else t["static_db"]
            ok = res.check(f"in {lev:+d} dBFS: output", max(ow[k], floor), max(os_[k], floor), tol, section=section)
            mark = "ok" if ok else "FAIL"
        rows.append([f"{lev:+d}", f"{iw[k]:.2f}", f"{ow[k]:.2f}", f"{os_[k]:.2f}", f"{gw:+.2f}", f"{gs:+.2f}",
                     f"{os_[k] - ow[k]:+.2f}", mark])
    res.tables.append({"title": f"{section}: output level per input step (L)",
                       "head": ["Step dBFS", "In (meas.)", "WING out", "Sidecar out", "WING gain", "Sidecar gain",
                                "diff", ""], "rows": rows})
    res.series[f"{section}"] = {"x": iw.tolist(), "wing": ow.tolist(), "sidecar": os_.tolist(),
                                "xlabel": "input dBFS", "ylabel": "output dBFS", "logx": False}


def gain_trajectory(cap, a, b, floor_db):
    """Gain (dB) over time of a 1 kHz signal (1 ms RMS); the output no lower than `floor_db`."""
    env_o = np.maximum(moving_rms(cap.out[a:b, 0]), 10.0 ** (floor_db / 20.0) / np.sqrt(2.0))
    env_i = moving_rms(cap.ref[a:b])
    return db20(env_o) - db20(env_i)


def edge_times(g, t_event, n_after, guard=0.1):
    """10/63/90 % times (ms) of the gain change after an event at index t_event."""
    pre = g[max(0, t_event - int(guard * SR)):t_event - int(0.005 * SR)]
    post = g[t_event:t_event + n_after]
    if len(pre) == 0 or len(post) == 0:
        return None
    g0 = float(np.median(pre))
    g1 = float(np.median(post[int(0.8 * len(post)):]))
    delta = g1 - g0
    if abs(delta) < 1.0:
        return {"g0": g0, "g1": g1, "delta": delta}
    p = (post - g0) / delta
    out = {"g0": g0, "g1": g1, "delta": delta}
    for name, lvl in (("t10", 0.1), ("t63", 0.632), ("t90", 0.9)):
        idx = np.nonzero(p >= lvl)[0]
        out[name] = float(idx[0] / SR * 1e3) if len(idx) else float("nan")
    return out


def analyze_envelope(res, W, S, seg):
    t = res.tol
    p = seg["params"]
    lo_n, hi_n, af_n = int(p["low_s"] * SR), int(p["high_s"] * SR), int(p["after_s"] * SR)
    a, n = seg["start"], seg["length"]
    gw, gs = gain_trajectory(W, a, a + n, res.floor_db), gain_trajectory(S, a, a + n, res.floor_db)
    events = {"rise (level up 40 dB)": [], "fall (level down 40 dB)": []}
    pos = 0
    for _ in range(p["repeats"]):
        events["rise (level up 40 dB)"].append((pos + lo_n, hi_n))
        events["fall (level down 40 dB)"].append((pos + lo_n + hi_n, af_n))
        pos += lo_n + hi_n + af_n
    rows = []
    for kind, evs in events.items():
        tw = [edge_times(gw, e, d) for e, d in evs]
        ts = [edge_times(gs, e, d) for e, d in evs]
        tw = [x for x in tw if x]
        ts = [x for x in ts if x]
        if not tw or not ts:
            continue

        def mean(lst, key):
            v = [x[key] for x in lst if key in x and np.isfinite(x[key])]
            return float(np.mean(v)) if v else float("nan")

        sec = f"timing: {kind}"
        res.check("gain change", mean(tw, "delta"), mean(ts, "delta"), t["static_db"], section=sec)
        if abs(mean(tw, "delta")) >= 1.0 or abs(mean(ts, "delta")) >= 1.0:
            for key in ("t10", "t63", "t90"):
                w, s = mean(tw, key), mean(ts, key)
                tol = max(t["time_abs_ms"], t["time_rel"] * abs(w)) if np.isfinite(w) else t["time_abs_ms"]
                res.check(f"{key} (time to {key[1:]} % of the change)", w, s, tol, unit="ms", section=sec)
                rows.append([kind, key, f"{w:.1f}", f"{s:.1f}"])
    res.tables.append({"title": "attack / hold / release (mean of 3 jumps)", "head": ["Event", "Time", "WING ms",
                                                                                      "Sidecar ms"], "rows": rows})
    span = int(1.5 * SR)
    e0 = events["rise (level up 40 dB)"][0][0]
    x = np.arange(span) / SR * 1e3
    res.series["gain after the first level jump"] = {"x": x[::24].tolist(), "wing": gw[e0:e0 + span][::24].tolist(),
                                                     "sidecar": gs[e0:e0 + span][::24].tolist(), "xlabel": "ms",
                                                     "ylabel": "gain dB", "logx": False}


def analyze_dyn(res, W, S, tl):
    analyze_static(res, W, S, ms.segment(tl, "levels"))
    analyze_envelope(res, W, S, ms.segment(tl, "envelope"))


def analyze_key(res, W, S, tl):
    seg = ms.segment(tl, "stepfreq")
    freqs = seg["params"]["freqs"]
    step_n = int(seg["params"]["step_s"] * SR)
    iw, ow = step_levels(W, seg["start"], freqs, step_n)
    is_, os_ = step_levels(S, seg["start"], freqs, step_n)
    rows = []
    for k, f in enumerate(freqs):
        gw, gs = ow[k] - iw[k], os_[k] - is_[k]
        fl = res.floor_db - iw[k]
        ok = res.check(f"{f:g} Hz: gain", max(gw, fl), max(gs, fl), res.tol["key_db"],
                       section="gain per frequency (-10 dBFS)")
        rows.append([f"{f:g}", f"{gw:+.2f}", f"{gs:+.2f}", f"{gs - gw:+.2f}", "ok" if ok else "FAIL"])
    res.tables.append({"title": "gain per frequency at -10 dBFS", "head": ["Hz", "WING dB", "Sidecar dB", "diff", ""],
                       "rows": rows})
    res.series["gain per frequency"] = {"x": list(freqs), "wing": (ow - iw).tolist(), "sidecar": (os_ - is_).tolist(),
                                        "xlabel": "Hz", "ylabel": "gain dB", "logx": True}


def impulse_response(cap, seg, post):
    """The IR of each output channel against the reference (band-limited)."""
    freqs, rs, pre = sweep_responses(cap, seg, post=post, pre=0.01)
    nfft = 2 * (len(freqs) - 1)
    irs = []
    for r in rs:
        ir = band_limited_ir(freqs, r["H"], nfft)
        irs.append(np.concatenate([ir[-int(pre * SR):], ir[:int(post * SR)]]))
    return np.stack(irs, axis=1), int(pre * SR)


def schroeder(h2):
    e = np.cumsum(h2[::-1])[::-1]
    return db10(e / max(e[0], EPS))


def decay_time(curve, top, bottom):
    idx = np.nonzero((curve <= top) & (curve >= bottom))[0]
    if len(idx) < 10 or curve.min() > bottom:
        return float("nan")
    tt = idx / SR
    slope, _ = np.polyfit(tt, curve[idx], 1)
    return float(-60.0 / slope) if slope < 0 else float("nan")


def reverb_measures(ir, z):
    e_all = np.sum(ir ** 2, axis=1)
    env = db10(e_all / max(e_all.max(), EPS))
    onset = int(np.nonzero(env > -20)[0][0]) if (env > -20).any() else z
    out = {"arrival_ms": (onset - z) / SR * 1e3}
    for fc in RT_BANDS:
        sos = sps.butter(3, [fc / np.sqrt(2), min(fc * np.sqrt(2), SR / 2 * 0.95)], btype="band", fs=SR, output="sos")
        hb = sps.sosfiltfilt(sos, ir, axis=0)
        h2 = np.sum(hb ** 2, axis=1)[onset:]
        c = schroeder(h2)
        n80 = int(0.08 * SR)
        out[fc] = {
            "level": float(db10(np.sum(h2))),
            "edt": decay_time(c, 0.0, -10.0),
            "t20": decay_time(c, -5.0, -25.0),
            "t30": decay_time(c, -5.0, -35.0),
            "c80": float(db10(np.sum(h2[:n80]) / max(np.sum(h2[n80:]), EPS))),
        }
    return out


def analyze_reverb(res, W, S, tl):
    t = res.tol
    seg = ms.segment(tl, "sweep")
    post = min(4.5, (seg["after"] / SR) - 0.2)
    irw, z = impulse_response(W, seg, post)
    irs, _ = impulse_response(S, seg, post)
    mw, mss = reverb_measures(irw, z), reverb_measures(irs, z)
    res.check("arrival (pre-delay)", mw["arrival_ms"], mss["arrival_ms"], 1.0, unit="ms", section="reverb")
    rows = []
    for fc in RT_BANDS:
        a, b = mw[fc], mss[fc]
        sec = f"reverb {fc} Hz"
        res.check("level", a["level"], b["level"], t["band_db"], section=sec)
        res.check("EDT", a["edt"], b["edt"], t["rt_rel"], unit="s", rel=True, section=sec)
        rt_w = a["t30"] if np.isfinite(a["t30"]) else a["t20"]
        rt_s = b["t30"] if np.isfinite(b["t30"]) else b["t20"]
        res.check("RT60 (T30, else T20)", rt_w, rt_s, t["rt_rel"], unit="s", rel=True, section=sec)
        res.check("C80", a["c80"], b["c80"], t["c80_db"], section=sec)
        rows.append([f"{fc}", f"{a['edt']:.2f} / {b['edt']:.2f}", f"{rt_w:.2f} / {rt_s:.2f}",
                     f"{a['c80']:+.1f} / {b['c80']:+.1f}", f"{a['level']:+.1f} / {b['level']:+.1f}"])
    res.tables.append({"title": "reverb per octave band (WING / Sidecar)",
                       "head": ["Band Hz", "EDT s", "RT60 s", "C80 dB", "level dB"], "rows": rows})
    for name, ir in (("wing", irw), ("sidecar", irs)):
        c = schroeder(np.sum(ir[z:] ** 2, axis=1))
        res.series.setdefault("energy decay (broadband)", {"xlabel": "s", "ylabel": "dB", "logx": False,
                                                           "x": (np.arange(len(c)) / SR)[::240].tolist()})[name] = \
            c[::240].tolist()
    lr = []
    for ir in (irw, irs):
        a, b = ir[z:, 0], ir[z:, 1]
        cc = sps.correlate(a, b, mode="full", method="fft")
        mid = len(b) - 1
        w = int(0.001 * SR)
        lr.append(float(np.max(np.abs(cc[mid - w:mid + w + 1])) / max(np.sqrt(np.sum(a * a) * np.sum(b * b)), EPS)))
    res.check("L/R correlation (IACC-like)", lr[0], lr[1], None, unit="", section="reverb")
    analyze_clicks(res, W, S, tl)


def analyze_clicks(res, W, S, tl):
    seg = ms.segment(tl, "clicks")
    p = seg["params"]
    spacing = int(p["spacing_s"] * SR)
    out = []
    for cap in (W, S):
        e = np.zeros(spacing)
        for i in range(p["count"]):
            a = seg["start"] + i * spacing
            x = cap.out[a:a + spacing]
            e[:len(x)] += np.sum(x ** 2, axis=1)
        c = schroeder(e)
        out.append((decay_time(c, 0.0, -10.0), decay_time(c, -5.0, -25.0)))
    res.check("EDT (clicks, broadband)", out[0][0], out[1][0], res.tol["rt_rel"], unit="s", rel=True,
              section="clicks")
    res.check("T20 (clicks, broadband)", out[0][1], out[1][1], res.tol["rt_rel"], unit="s", rel=True,
              section="clicks")


def find_echoes(ir, z, max_n=8, floor_db=-40.0, min_gap_ms=2.0):
    """(time ms, level dB) of the strongest peaks of the IR's energy, in time order."""
    e = np.sum(ir ** 2, axis=1)
    env = db10(e / max(e.max(), EPS))
    gap = int(min_gap_ms * 1e-3 * SR)
    order = np.argsort(e)[::-1]
    peaks = []
    for i in order:
        if env[i] < floor_db or len(peaks) >= max_n:
            break
        if all(abs(i - q) > gap for q in peaks):
            peaks.append(int(i))
    peaks.sort()
    out = []
    for i in peaks:
        y0, y1, y2 = e[max(i - 1, 0)], e[i], e[min(i + 1, len(e) - 1)]
        den = y0 - 2 * y1 + y2
        frac = 0.5 * (y0 - y2) / den if den < 0 else 0.0
        w = int(0.001 * SR)
        out.append(((i + frac - z) / SR * 1e3, float(db10(np.sum(e[max(i - w, 0):i + w + 1])))))
    return out


def analyze_delay(res, W, S, tl):
    t = res.tol
    seg = ms.segment(tl, "sweep")
    post = min(4.5, (seg["after"] / SR) - 0.2)
    irw, z = impulse_response(W, seg, post)
    irs, _ = impulse_response(S, seg, post)
    ew, es = find_echoes(irw, z), find_echoes(irs, z)
    res.check("echoes within 40 dB of the strongest", len(ew), len(es), None, unit="", section="delay")
    rows = []
    for k in range(min(len(ew), len(es))):
        res.check(f"echo {k + 1} time", ew[k][0], es[k][0], t["delay_ms"], unit="ms", section="delay")
        res.check(f"echo {k + 1} level", ew[k][1], es[k][1], t["echo_db"], section="delay")
        rows.append([str(k + 1), f"{ew[k][0]:.2f}", f"{es[k][0]:.2f}", f"{ew[k][1]:+.2f}", f"{es[k][1]:+.2f}"])
    for name, e in (("WING", ew), ("Sidecar", es)):
        if len(e) >= 3:
            res.notes.append(f"{name}: feedback {np.mean(np.diff([x[1] for x in e[1:]])):+.2f} dB per repeat.")
    res.tables.append({"title": "echoes (both channels)", "head": ["#", "WING ms", "Sidecar ms", "WING dB",
                                                                   "Sidecar dB"], "rows": rows})
    if ew and es:
        nfft = 1 << 15
        grid = log_grid(50, 16000, 12)
        f = np.arange(nfft // 2 + 1) * SR / nfft
        mags = []
        for ir, e in ((irw, ew), (irs, es)):
            c = z + int(round(e[0][0] * 1e-3 * SR))
            seg_ = ir[max(c - int(0.002 * SR), 0):c + int(0.02 * SR), 0]
            mags.append(db10(smooth_power(f, np.abs(np.fft.rfft(seg_, nfft)) ** 2, grid, 3)))
        d = mags[1] - mags[0]
        res.check_diff("first echo: largest response difference, 50 Hz - 16 kHz", d[np.argmax(np.abs(d))], t["band_db"],
                  section="delay")


def inst_freq(x, lo=300.0, hi=3000.0):
    sos = sps.butter(4, [lo, hi], btype="band", fs=SR, output="sos")
    a = sps.hilbert(sps.sosfiltfilt(sos, x))
    f = np.diff(np.unwrap(np.angle(a))) * SR / (2 * np.pi)
    k = int(0.005 * SR)
    return np.convolve(f, np.ones(k) / k, mode="valid")


def lfo(fi):
    """(rate Hz, depth Hz) of a frequency modulation from the instantaneous frequency."""
    x = (fi - np.mean(fi))[::16]
    rate_sr = SR / 16
    n = 1 << int(np.ceil(np.log2(len(x) * 16)))
    spec = np.log(np.abs(np.fft.rfft(x * np.hanning(len(x)), n)) + EPS)
    f = np.fft.rfftfreq(n, 1 / rate_sr)
    m = np.nonzero((f >= 0.05) & (f <= 20))[0]
    rate = float("nan")
    if len(m):
        i = m[int(np.argmax(spec[m]))]
        y0, y1, y2 = spec[i - 1], spec[i], spec[i + 1]
        den = y0 - 2 * y1 + y2
        rate = float(f[i] + (0.5 * (y0 - y2) / den if den < 0 else 0.0) * (f[1] - f[0]))
    depth = float((np.percentile(fi, 95) - np.percentile(fi, 5)) / 2)
    return rate, depth


def analyze_mod(res, W, S, tl):
    t = res.tol
    seg = ms.segment(tl, "tone")
    a, n = seg["start"] + int(0.5 * SR), seg["length"] - int(1.0 * SR)
    for c, name in ((0, "L"), (1, "R")):
        rw_, dw = lfo(inst_freq(W.out[a:a + n, c]))
        rs_, ds = lfo(inst_freq(S.out[a:a + n, c]))
        res.check(f"LFO rate {name}", rw_, rs_, t["lfo_rel"], unit="Hz", rel=True, section="modulation (1 kHz tone)")
        res.check(f"depth {name} (frequency swing)", dw, ds, t["depth_rel"], unit="Hz", rel=True,
                  section="modulation (1 kHz tone)")
        res.check(f"tone level {name}", rms_db(W.out[a:a + n, c]), rms_db(S.out[a:a + n, c]), t["band_db"],
                  section="modulation (1 kHz tone)")
    spectrum_check(res, W, S, ms.segment(tl, "pink"), t["band_db"], "pink noise")


def spectrum_check(res, W, S, seg, tol, label):
    p0, pn = seg["start"], seg["length"]
    rows = []
    for c, name in ((0, "L"), (1, "R")):
        bw = band_levels(W.out[p0:p0 + pn, c], W.ref[p0:p0 + pn])
        bs = band_levels(S.out[p0:p0 + pn, c], S.ref[p0:p0 + pn])
        for f in bw:
            if f in bs and f >= 40:
                ok = res.check(f"{f:g} Hz {name}", bw[f], bs[f], tol, section=f"{label}: gain per 1/3 octave")
                if c == 0:
                    rows.append([f"{f:g}", f"{bw[f]:+.2f}", f"{bs[f]:+.2f}", f"{bs[f] - bw[f]:+.2f}",
                                 "ok" if ok else "FAIL"])
    res.tables.append({"title": f"{label}: gain per 1/3 octave (L)", "head": ["Hz", "WING dB", "Sidecar dB", "diff",
                                                                             ""], "rows": rows})


def analyze_drive(res, W, S, tl):
    t = res.tol
    for name in ("sweep", "sweep_hot"):
        seg = ms.segment(tl, name)
        rw, rs, fw = analyze_sweep(res, W, S, seg, name, t["band_db"], 2 * t["band_db"], 4 * t["phase_deg"],
                                   with_phase=False)
        compare_harmonics(res, harmonics(fw, rw[0]), harmonics(fw, rs[0]), f"{name}: harmonics")
    seg = ms.segment(tl, "levels")
    levels = seg["params"]["levels"]
    step_n = int(seg["params"]["step_s"] * SR)
    rows = []
    for k, lev in enumerate(levels):
        a = seg["start"] + k * step_n + int(0.3 * step_n)
        n = int(0.6 * step_n)
        lw, _, thw = tone_analysis(W.out[a:a + n, 0])
        ls, _, ths = tone_analysis(S.out[a:a + n, 0])
        res.check(f"in {lev:+d} dBFS: output level", max(lw, res.floor_db), max(ls, res.floor_db), t["band_db"],
                  section="1 kHz steps")
        if max(thw, ths) > -80:
            res.check(f"in {lev:+d} dBFS: THD", thw, ths, t["thd_db"], section="1 kHz steps")
        rows.append([f"{lev:+d}", f"{lw:.2f}", f"{ls:.2f}", f"{thw:.1f}", f"{ths:.1f}"])
    res.tables.append({"title": "1 kHz steps", "head": ["In dBFS", "WING out", "Sidecar out", "WING THD dB",
                                                        "Sidecar THD dB"], "rows": rows})
    spectrum_check(res, W, S, ms.segment(tl, "pink"), t["band_db"], "pink noise")


def analyze_music(res, W, S, tl):
    seg = ms.segment(tl, "music")
    a, n = seg["start"], seg["length"]
    for c, name in ((0, "L"), (1, "R")):
        res.check(f"level {name}", rms_db(W.out[a:a + n, c]), rms_db(S.out[a:a + n, c]), res.tol["level_db"],
                  section="music")
    spectrum_check(res, W, S, seg, res.tol["spectrum_db"], "music")


ANALYSES = {"lin": analyze_lin, "dyn": analyze_dyn, "key": analyze_key, "reverb": analyze_reverb,
            "delay": analyze_delay, "mod": analyze_mod, "drive": analyze_drive, "music": analyze_music}


def find_test(plan, name):
    for t in plan["tests"]:
        if t["name"] == name:
            return t
    raise SystemExit(f"no test {name!r} in the plan")


def analyze(wing_path, sidecar_path, test, plan):
    """Runs the test's analysis; returns a Result with the captures' info."""
    signal, tl = ms.test_signal(test)
    res = Result(test, plan["tolerances"])
    W = Capture(wing_path, signal, tl, test["code"])
    S = Capture(sidecar_path, signal, tl, test["code"])
    res.captures = {"WING": W.info, "Sidecar": S.info}
    # Levels below the louder noise floor + 10 dB aren't compared.
    res.floor_db = max(W.info["noise_dbfs"], S.info["noise_dbfs"], -200.0) + 10.0
    res.warnings = [f"WING: {w}" for w in W.warnings] + [f"Sidecar: {w}" for w in S.warnings]
    ANALYSES[test["analysis"]](res, W, S, tl)
    content = slice(tl["segments"][0]["start"], tl["end_chirp"])
    depth, d = null_depth(W.out[content, 0], S.out[content, 0])
    res.check_diff("null depth (residual of WING - Sidecar, L)", depth, None, section="overall")
    res.notes.append(f"Null test: Sidecar aligned to the WING by {d:+.2f} samples; residual {depth:.1f} dB "
                     "re the WING's output (below -40 dB: hard to hear; information only).")
    return res


# ---- report ------------------------------------------------------------------------

def fmt_num(v, unit=""):
    if v is None:
        return "-"
    if isinstance(v, float) and not np.isfinite(v):
        return "n/a"
    return f"{v:.3g}" if abs(v) < 1e4 else f"{v:.0f}"


def report_markdown(res, plots=None):
    t = res.test
    checked, failed = res.counts()
    verdict = "PASS" if failed == 0 else f"FAIL ({failed} of {checked})"
    out = [f"# {t['name']}: {verdict}", "", t["title"], ""]
    if t["settings"]:
        out += ["Settings (on top of the common ones): " + ", ".join(f"`{k}` = {v}" for k, v in t["settings"].items()),
                ""]
    out += ["## Captures", "", "| | WING | Sidecar |", "|---|---|---|"]
    keys = ["file", "channels", "reference", "code", "offset_s", "drift_ppm", "gain_cal_db", "ref_null_db",
            "chirp_gain_db", "peak_dbfs", "noise_dbfs"]
    for k in keys:
        a, b = res.captures["WING"].get(k), res.captures["Sidecar"].get(k)
        fa = f"{a:.4g}" if isinstance(a, float) else str(a)
        fb = f"{b:.4g}" if isinstance(b, float) else str(b)
        out.append(f"| {k} | {fa} | {fb} |")
    out.append("")
    for w in res.warnings:
        out.append(f"- **Warning:** {w}")
    if res.warnings:
        out.append("")
    out += ["## Measures", "", "| Section | Measure | WING | Sidecar | Sidecar - WING | Tolerance | |",
            "|---|---|---|---|---|---|---|"]
    for r in res.rows:
        tol = "-" if r["tol"] is None else (f"±{r['tol'] * 100:.3g} %" if r.get("rel") else f"±{r['tol']:.3g} {r['unit']}")
        mark = "" if r["ok"] is None else ("ok" if r["ok"] else "**FAIL**")
        diff = "-" if r["diff"] is None else f"{r['diff']:+.3g} {r['unit']}"
        out.append(f"| {r['section']} | {r['metric']} | {fmt_num(r['wing'])} | {fmt_num(r['sidecar'])} | {diff} | "
                   f"{tol} | {mark} |")
    out.append("")
    for n in res.notes:
        out.append(f"- {n}")
    out.append("")
    for tb in res.tables:
        if not tb["rows"]:
            continue
        out += [f"### {tb['title']}", "", "| " + " | ".join(tb["head"]) + " |",
                "|" + "---|" * len(tb["head"])]
        out += ["| " + " | ".join(r) + " |" for r in tb["rows"]]
        out.append("")
    if plots:
        out += ["## Plots", ""] + [f"![{os.path.basename(p)}]({p})" for p in plots] + [""]
    return "\n".join(out)


def write_plots(res, folder, rel_to=None):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except Exception:
        return []
    os.makedirs(folder, exist_ok=True)
    paths = []
    for i, (title, s) in enumerate(res.series.items()):
        fig, ax = plt.subplots(figsize=(8, 4))
        x = s["x"]
        for key, val in s.items():
            if key in ("x", "xlabel", "ylabel", "logx"):
                continue
            ax.plot(x[:len(val)], val, label=key)
        if s.get("logx"):
            ax.set_xscale("log")
        ax.set_xlabel(s.get("xlabel", ""))
        ax.set_ylabel(s.get("ylabel", ""))
        ax.set_title(f"{res.test['name']}: {title}")
        ax.grid(True, which="both", alpha=0.3)
        ax.legend()
        name = os.path.join(folder, f"{res.test['name']}_{i}.png")
        fig.tight_layout()
        fig.savefig(name, dpi=90)
        plt.close(fig)
        paths.append(os.path.relpath(name, rel_to) if rel_to else name)
    return paths


def result_json(res):
    checked, failed = res.counts()
    return {"test": res.test["name"], "title": res.test["title"], "priority": res.test["priority"],
            "checked": checked, "failed": failed, "worst": res.worst(), "captures": res.captures,
            "warnings": res.warnings, "rows": res.rows, "notes": res.notes}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("wing")
    ap.add_argument("sidecar")
    ap.add_argument("--test", required=True)
    ap.add_argument("--plan", default=os.path.join(HERE, "test_plan.json"))
    ap.add_argument("--report", help="markdown report (default: print)")
    ap.add_argument("--plots", help="folder for PNG plots (needs matplotlib)")
    ap.add_argument("--json", help="results as JSON")
    a = ap.parse_args(argv)
    plan = ms.load_plan(a.plan)
    test = find_test(plan, a.test)
    res = analyze(a.wing, a.sidecar, test, plan)
    plots = []
    if a.plots:
        rel = os.path.dirname(os.path.abspath(a.report)) if a.report else None
        plots = write_plots(res, a.plots, rel)
        if not plots:
            print("(no plots: matplotlib is not installed)", file=sys.stderr)
    text = report_markdown(res, plots)
    if a.report:
        with open(a.report, "w") as f:
            f.write(text)
    else:
        print(text)
    if a.json:
        with open(a.json, "w") as f:
            json.dump(result_json(res), f, indent=1)
    checked, failed = res.counts()
    print(f"{test['name']}: {'PASS' if failed == 0 else 'FAIL'} ({checked - failed}/{checked} within tolerance)",
          file=sys.stderr)
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
