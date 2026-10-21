"""Simulated capture artefacts, for checking the kit without a WING.

A recording made at the console differs from the file it played: it starts
somewhere (latency), the recorder's clock may run slightly off the player's
(drift), the level may be off a little and there is noise. `capture()`
applies all of that to a clean render, with resampling methods independent
of the ones analyze.py uses to undo them.
"""

import numpy as np
from scipy import signal as sps


def capture(data, sr=48000, latency_samples=0.0, gain_db=0.0, drift_ppm=0.0, noise_db=None, lead_s=0.0,
            tail_s=0.0, seed=3):
    """`data` (frames, channels) as a recorder would capture it: `lead_s` of
    silence first, then the signal `latency_samples` (fractional) later,
    stretched by `drift_ppm`, scaled by `gain_db`, with white noise at
    `noise_db` dBFS RMS."""
    x = np.asarray(data, dtype=np.float64)
    if x.ndim == 1:
        x = x[:, None]
    n = len(x)
    if drift_ppm:
        m = int(round(n * (1.0 + drift_ppm * 1e-6)))
        x = sps.resample(x, m, axis=0)
    lead = int(lead_s * sr)
    total = lead + len(x) + int(tail_s * sr) + int(np.ceil(abs(latency_samples))) + 64
    y = np.zeros((total, x.shape[1]))
    whole = int(np.floor(latency_samples))
    frac = latency_samples - whole
    y[lead + whole:lead + whole + len(x)] = x
    if frac:
        k = np.fft.rfftfreq(total)
        y = np.fft.irfft(np.fft.rfft(y, axis=0) * np.exp(-2j * np.pi * k * frac)[:, None], total, axis=0)
    y *= 10.0 ** (gain_db / 20.0)
    if noise_db is not None:
        rng = np.random.default_rng(seed)
        y += rng.standard_normal(y.shape) * 10.0 ** (noise_db / 20.0)
    return y
