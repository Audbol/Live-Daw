//! Building blocks shared by the reverbs: control-rate smoothing, the feedback
//! delay network (FDN) tank with its frequency-dependent decay, allpass input
//! diffusers, the pre-delay with its build-up taps, discrete taps (early
//! reflections), echoes and the output stage (lo/hi cut, width, level).
//!
//! Controls are smoothed every [`CTL`] samples and the coefficients derived
//! from them (loop gains, shelf corners, tap positions...) are recomputed only
//! while something moves. Delay lengths glide rather than jump (a little
//! every sample, rate-limited), so SIZE, PRE DLY and tap times change without
//! clicks; echo and delay times crossfade between two heads instead. Delays
//! read with Hermite interpolation while they move (or are modulated) and with
//! integer taps once settled, which keeps the settled loops free of
//! interpolation loss.

use crate::wing::fx::blocks::{db_to_gain, DelayLine, Svf};
use std::f32::consts::{LOG2_10, TAU};

/// Samples per control step.
pub(super) const CTL: usize = 32;

/// A device processed sample by sample with a control step every [`CTL`].
pub(super) trait Voice {
    /// Samples left until the next control step.
    fn clock(&mut self) -> &mut usize;
    fn control(&mut self);
    fn tick(&mut self, l: f32, r: f32) -> (f32, f32);
}

/// Runs a [`Voice`] over a block in place.
#[inline]
pub(super) fn run<V: Voice>(v: &mut V, left: &mut [f32], right: &mut [f32]) {
    for (l, r) in left.iter_mut().zip(right.iter_mut()) {
        if *v.clock() == 0 {
            *v.clock() = CTL;
            v.control();
        }
        *v.clock() -= 1;
        (*l, *r) = v.tick(*l, *r);
    }
}

/// Added to recirculating paths so idle tails never decay into denormals
/// (far below anything audible; the output lo cut removes the offset).
pub(super) const TINY: f32 = 1e-18;

/// Hard safety limit for anything written into a feedback path.
pub(super) const LIM: f32 = 24.0;

/// `-3 * log2(10)`: `2^(K60 * t / T60)` is the gain after `t` seconds of a
/// decay that falls 60 dB in `T60`.
pub(super) const K60: f32 = -3.0 * LOG2_10;

/// A control value smoothed at the control rate.
#[derive(Clone, Copy, Default, Debug)]
pub(super) struct Param {
    pub cur: f32,
    pub target: f32,
}

impl Param {
    #[inline]
    pub fn set(&mut self, v: f32) {
        self.target = v;
    }

    pub fn snap(&mut self) {
        self.cur = self.target;
    }

    /// Moves a step `a` (0..1) towards the target; true if the value changed.
    #[inline]
    pub fn step(&mut self, a: f32) -> bool {
        let d = self.target - self.cur;
        if d == 0.0 {
            return false;
        }
        if d.abs() <= 1e-6 * (self.target.abs() + 1.0) {
            self.cur = self.target;
        } else {
            self.cur += a * d;
        }
        true
    }

    /// As [`Self::step`], moving at most `max` per step.
    #[inline]
    pub fn step_limited(&mut self, a: f32, max: f32) -> bool {
        let d = self.target - self.cur;
        if d == 0.0 {
            return false;
        }
        if d.abs() <= 1e-6 * (self.target.abs() + 1.0) {
            self.cur = self.target;
        } else {
            self.cur += (a * d).clamp(-max, max);
        }
        true
    }

    /// Still on its way to the target.
    #[inline]
    pub fn moving(&self) -> bool {
        self.cur != self.target
    }
}

/// Per-control-step smoothing coefficient for a time constant in ms.
pub(super) fn rate(ms: f32, sr: f32) -> f32 {
    1.0 - (-(CTL as f32) / (ms * 0.001 * sr).max(1.0)).exp()
}

/// A console fader level in dB (-60 and below = off) as a gain.
pub(super) fn fader(db: f32) -> f32 {
    if db <= -60.0 {
        0.0
    } else {
        db_to_gain(db)
    }
}

/// Deterministic hash of `i` and `seed` to 0..1.
pub(super) fn hash01(i: u32, seed: u32) -> f32 {
    let mut x = i.wrapping_mul(0x9E37_79B9) ^ seed.wrapping_mul(0x85EB_CA6B);
    x ^= x >> 16;
    x = x.wrapping_mul(0x7FEB_352D);
    x ^= x >> 15;
    x = x.wrapping_mul(0x846C_A68B);
    x ^= x >> 16;
    (x >> 8) as f32 / (1u32 << 24) as f32
}

/// A deterministic sign for `i`.
pub(super) fn sign(i: u32, seed: u32) -> f32 {
    if hash01(i, seed) < 0.5 {
        -1.0
    } else {
        1.0
    }
}

/// One-pole low-pass coefficient for a corner in Hz.
pub(super) fn pole(hz: f32, sr: f32) -> f32 {
    1.0 - (-TAU * hz.clamp(1.0, sr * 0.49) / sr).exp()
}

/// A delay read position: while it glides it moves a little every sample
/// (Hermite reads) to reach each control step's target by the next one; once
/// settled it reads an integer tap.
#[derive(Clone, Copy, Debug)]
pub(super) struct Pos {
    f: f32,
    inc: f32,
    i: usize,
    frac: bool,
}

impl Default for Pos {
    fn default() -> Self {
        Self { f: 2.0, inc: 0.0, i: 2, frac: false }
    }
}

impl Pos {
    /// New target `d` (samples); `frac` while it is gliding.
    #[inline]
    pub fn set(&mut self, d: f32, frac: bool) {
        let d = d.max(1.0);
        if frac {
            self.inc = (d - self.f) / CTL as f32;
        } else {
            self.f = d;
            self.inc = 0.0;
        }
        self.i = d.round() as usize;
        self.frac = frac;
    }

    #[inline]
    pub fn read(&mut self, line: &DelayLine) -> f32 {
        if self.frac {
            self.f += self.inc;
            line.tap_hermite(self.f)
        } else {
            line.tap(self.i)
        }
    }
}

/// `N` relative delay lengths (mean 1) spread geometrically from `lo` to `hi`
/// with a little deterministic jitter, bit-reverse ordered so even (left-fed)
/// and odd (right-fed) lines both get short and long ones.
pub(super) fn spread<const N: usize>(lo: f32, hi: f32, seed: u32) -> [f32; N] {
    let mut sorted = [0.0f32; N];
    for (i, v) in sorted.iter_mut().enumerate() {
        let t = (i as f32 + 0.35 * (hash01(i as u32, seed) - 0.5)) / (N as f32 - 1.0).max(1.0);
        *v = lo * (hi / lo).powf(t.clamp(0.0, 1.0));
    }
    let bits = N.trailing_zeros();
    let mut out = [0.0f32; N];
    for (i, o) in out.iter_mut().enumerate() {
        let j = if bits == 0 { 0 } else { (i as u32).reverse_bits() >> (32 - bits) } as usize;
        *o = sorted[j.min(N - 1)];
    }
    let mean = out.iter().sum::<f32>() / N as f32;
    out.map(|v| v / mean)
}

/// In-place fast Walsh-Hadamard transform (unnormalised: gains are `sqrt(N)`).
#[inline]
fn hadamard<const N: usize>(x: &mut [f32; N]) {
    let mut h = 1;
    while h < N {
        for blk in x.chunks_exact_mut(2 * h) {
            let (a, b) = blk.split_at_mut(h);
            for (p, q) in a.iter_mut().zip(b.iter_mut()) {
                let (s, d) = (*p + *q, *p - *q);
                *p = s;
                *q = d;
            }
        }
        h *= 2;
    }
}

/// `N` delay lines of one power-of-two capacity sharing a write position.
/// Each line mirrors its first samples past its end so a 4-sample window
/// never wraps, and the lines sit a cache line apart beyond their size so one
/// sample's writes don't all land in the same cache set.
struct Lines<const N: usize> {
    buf: Vec<f32>,
    size: usize,
    stride: usize,
    mask: usize,
    pos: usize,
}

/// Mirrored samples past the end of each line.
const GUARD: usize = 4;

impl<const N: usize> Lines<N> {
    fn new(max_delay: usize) -> Self {
        let size = (max_delay + 4).next_power_of_two();
        let stride = size + 16;
        Self { buf: vec![0.0; stride * N], size, stride, mask: size - 1, pos: 0 }
    }

    fn clear(&mut self) {
        self.buf.fill(0.0);
    }

    fn max_delay(&self) -> f32 {
        (self.size - 4) as f32
    }

    /// Line `i`, `d` samples back (0 = the latest write).
    #[inline(always)]
    fn tap(&self, i: usize, d: usize) -> f32 {
        self.buf[i * self.stride + (self.pos.wrapping_sub(d) & self.mask)]
    }

    /// Line `i` at a fractional delay, 4-point Hermite.
    #[inline(always)]
    fn hermite(&self, i: usize, d: f32) -> f32 {
        let d = d.clamp(1.0, self.max_delay());
        let di = d as i32 as usize;
        let f = d - di as f32;
        let s = i * self.stride + (self.pos.wrapping_sub(di + 2) & self.mask);
        let w = &self.buf[s..s + 4];
        let (x2, x1, x0, xm1) = (w[0], w[1], w[2], w[3]);
        let c1 = 0.5 * (x1 - xm1);
        let c2 = xm1 - 2.5 * x0 + 2.0 * x1 - 0.5 * x2;
        let c3 = 0.5 * (x2 - xm1) + 1.5 * (x0 - x1);
        ((c3 * f + c2) * f + c1) * f + x0
    }

    #[inline(always)]
    fn push(&mut self, v: &[f32; N]) {
        self.pos = (self.pos + 1) & self.mask;
        let (p, size, stride) = (self.pos, self.size, self.stride);
        for (i, &x) in v.iter().enumerate() {
            self.buf[i * stride + p] = x;
        }
        if p < GUARD {
            for (i, &x) in v.iter().enumerate() {
                self.buf[i * stride + p + size] = x;
            }
        }
    }
}

/// Random delay modulation for one line (the 480L's MOD SPD / SPIN: random
/// variations of delay time and pitch): a new random target every period,
/// reached along a raised cosine; evaluated per control step and ramped per
/// sample.
#[derive(Clone, Copy, Default)]
struct Wobble {
    phase: f32,
    inc: f32,
    from: f32,
    to: f32,
    rng: u32,
    cur: f32,
    slope: f32,
}

impl Wobble {
    fn control(&mut self) {
        self.phase += self.inc;
        if self.phase >= 1.0 {
            self.phase -= 1.0;
            self.from = self.to;
            self.rng ^= self.rng << 13;
            self.rng ^= self.rng >> 17;
            self.rng ^= self.rng << 5;
            self.to = (self.rng >> 8) as f32 / (1u32 << 23) as f32 - 1.0;
        }
        let k = 0.5 - 0.5 * (std::f32::consts::PI * self.phase.min(1.0)).cos();
        let target = self.from + (self.to - self.from) * k;
        self.slope = (target - self.cur) / CTL as f32;
    }
}

/// Number of probe frequencies used to keep a line's loop gain below 1.
const PROBES: usize = 24;

/// Frequency-dependent decay: `bands` are decay rates (1 / RT60 in s, 0 =
/// infinite) for low, low-mid, high-mid and high, split at `xo` (Hz).
#[derive(Clone, Copy, Debug, PartialEq)]
pub(super) struct Decay {
    pub bands: [f32; 4],
    pub xo: [f32; 3],
}

impl Decay {
    /// The 480L-style decay: RT60 `t60` in the mids, `bass` x as long below
    /// `xover`, and above `damp` a gently shorter decay (the highs end up
    /// decaying twice as fast, as in a real space).
    pub fn lexicon(t60: f32, bass: f32, xover: f32, damp: f32) -> Self {
        let mid = 1.0 / t60.max(0.05);
        Self { bands: [mid / bass.max(0.05), mid, mid, mid * 2.0], xo: [xover, xover.max(damp * 0.5), damp] }
    }

    /// Low / mid / high decay multipliers with fixed crossovers.
    pub fn three_band(t60: f32, low: f32, high: f32, lo_xo: f32, hi_xo: f32) -> Self {
        let mid = 1.0 / t60.max(0.05);
        Self { bands: [mid / low.max(0.05), mid, mid, mid / high.max(0.05)], xo: [lo_xo, (lo_xo * hi_xo).sqrt(), hi_xo] }
    }

    /// Infinite decay (FREEZE).
    pub fn frozen(self) -> Self {
        Self { bands: [0.0; 4], ..self }
    }
}

/// The reverb tank: `N` (8 or 16) delay lines mixed by a Hadamard matrix.
/// Each line has a three-shelf loop filter giving the band decay rates of a
/// [`Decay`] exactly for its length; optionally an allpass in every line
/// (plates, dense rooms) builds density faster and multiplies the modes. The first quarter of the lines are modulated.
/// Each line has its own input (see [`BuildUp`]); the left output is taken
/// from the even lines and the right from the odd.
pub(super) struct Fdn<const N: usize> {
    sr: f32,
    lines: Lines<N>,
    aps: Option<Lines<N>>,
    rel: [f32; N],
    ap_rel: [f32; N],
    /// Current lengths (gliding per sample towards the targets while SIZE
    /// moves), their per-sample steps, targets and integer taps.
    len: [f32; N],
    len_inc: [f32; N],
    target: [f32; N],
    ilen: [usize; N],
    ap_len: [f32; N],
    ap_inc: [f32; N],
    ap_target: [f32; N],
    ap_ilen: [usize; N],
    /// SIZE is gliding: every line reads fractionally.
    moving: bool,
    wobble: [Wobble; N],
    depth: f32,
    ap_gain: f32,
    g: [f32; N],
    rm1: [[f32; N]; 3],
    /// Shelves that do something (a band ratio != 1 on some line).
    shelf_on: [bool; 3],
    z: [[f32; N]; 3],
    a: [f32; 3],
    sgn_in: [f32; N],
    sgn_out: [f32; N],
    probe: [(f32, f32); PROBES],
    max_len: f32,
}

impl<const N: usize> Fdn<N> {
    /// `rel`: relative line lengths (see [`spread`]); `max_mean_ms`: the longest
    /// mean length SIZE can ask for; `loop_ap`: add the in-loop allpasses.
    pub fn new(sr: f32, rel: [f32; N], max_mean_ms: f32, loop_ap: bool, seed: u32) -> Self {
        let max_rel = rel.iter().fold(0.0f32, |a, &b| a.max(b));
        let max_len = max_mean_ms * 0.001 * sr * max_rel;
        // Room for the modulation depth on top.
        let lines = Lines::new((max_len + 0.004 * sr) as usize + 8);
        let mut ap_rel = [0.0; N];
        for (i, a) in ap_rel.iter_mut().enumerate() {
            *a = 0.12 + 0.16 * hash01(i as u32, seed ^ 0xA5);
        }
        let aps = if loop_ap { Some(Lines::new((max_len * 0.3) as usize + 8)) } else { None };
        let mut probe = [(1.0, 0.0); PROBES];
        for (k, p) in probe.iter_mut().enumerate() {
            let hz = 20.0 * (0.47 * sr / 20.0).powf(k as f32 / (PROBES - 1) as f32);
            let w = TAU * hz / sr;
            *p = (w.cos(), w.sin());
        }
        let mut f = Self {
            sr,
            lines,
            aps,
            rel,
            ap_rel,
            len: [100.0; N],
            len_inc: [0.0; N],
            target: [100.0; N],
            ilen: [100; N],
            ap_len: [10.0; N],
            ap_inc: [0.0; N],
            ap_target: [10.0; N],
            ap_ilen: [10; N],
            moving: false,
            wobble: [Wobble::default(); N],
            depth: 0.0,
            ap_gain: 0.0,
            g: [0.0; N],
            rm1: [[0.0; N]; 3],
            shelf_on: [false; 3],
            z: [[0.0; N]; 3],
            a: [1.0; 3],
            sgn_in: [0.0; N],
            sgn_out: [0.0; N],
            probe,
            max_len,
        };
        for i in 0..N {
            f.sgn_in[i] = sign(i as u32, seed ^ 0x51);
            f.sgn_out[i] = sign(i as u32, seed ^ 0x3C);
            let w = &mut f.wobble[i];
            w.phase = hash01(i as u32, seed ^ 0x77);
            w.rng = 0x9E37_79B9 ^ (i as u32 + 1).wrapping_mul(0x85EB_CA6B) ^ seed;
        }
        f
    }

    pub fn clear(&mut self) {
        self.lines.clear();
        if let Some(ap) = &mut self.aps {
            ap.clear();
        }
        self.z = [[0.0; N]; 3];
        for w in &mut self.wobble {
            w.cur = 0.0;
            w.slope = 0.0;
            w.from = 0.0;
            w.to = 0.0;
        }
    }

    /// Mean line length in samples; `moving` while it glides (fractional reads).
    pub fn set_size(&mut self, mean: f32, moving: bool) {
        let mean = mean.clamp(16.0, self.max_len);
        let k = 1.0 / CTL as f32;
        for i in 0..N {
            let l = (self.rel[i] * mean).min(self.max_len);
            let a = (self.ap_rel[i] * l).max(2.0);
            if moving {
                self.len_inc[i] = (l - self.len[i]) * k;
                self.ap_inc[i] = (a - self.ap_len[i]) * k;
            } else {
                self.len[i] = l;
                self.ap_len[i] = a;
                self.len_inc[i] = 0.0;
                self.ap_inc[i] = 0.0;
            }
            self.target[i] = l;
            self.ap_target[i] = a;
            self.ilen[i] = l.round() as usize;
            self.ap_ilen[i] = a.round() as usize;
        }
        self.moving = moving;
        self.depth = self.depth.min(self.min_len() * 0.25);
    }

    fn min_len(&self) -> f32 {
        self.len.iter().fold(f32::MAX, |a, &b| a.min(b))
    }

    /// In-loop allpass gain (if there are any).
    pub fn set_ap_gain(&mut self, g: f32) {
        self.ap_gain = g.clamp(0.0, 0.8);
    }

    /// Modulation rate (Hz, spread +-30 % over the lines) and depth (samples).
    pub fn set_mod(&mut self, hz: f32, depth: f32) {
        for (i, w) in self.wobble.iter_mut().enumerate() {
            let r = hz * (0.7 + 0.6 * hash01(i as u32, 0xD1CE));
            w.inc = (r * CTL as f32 / self.sr).min(0.5);
        }
        self.depth = depth.clamp(0.0, (0.004 * self.sr).min(self.min_len() * 0.25));
    }

    /// Recomputes the loop filters for the current lengths. Call after
    /// [`Self::set_size`] or when the decay changes.
    pub fn set_decay(&mut self, d: &Decay) {
        for (a, &hz) in self.a.iter_mut().zip(&d.xo) {
            *a = pole(hz, self.sr);
        }
        let has_ap = self.aps.is_some();
        // The Hadamard matrix's gain of sqrt(N) is folded into the loop gains.
        let scale = 1.0 / (N as f32).sqrt();
        for i in 0..N {
            // Loop delay in seconds (an allpass delays by its length on average).
            let t = (self.target[i] + 1.0 + if has_ap { self.ap_target[i] + 1.0 } else { 0.0 }) / self.sr;
            let gb = d.bands.map(|r| (K60 * t * r.max(0.0)).exp2());
            let top = gb[3];
            let r = [gb[0] / gb[1], gb[1] / gb[2], gb[2] / top];
            let rm1 = r.map(|x| x - 1.0);
            for (dst, v) in self.rm1.iter_mut().zip(rm1) {
                dst[i] = v;
            }
            let max_g = gb.iter().fold(0.0f32, |a, &b| a.max(b));
            let bound = top * r.iter().fold(1.0f32, |a, &b| a * b.max(1.0));
            let mut g = top;
            if bound > max_g * 1.000_001 {
                // Bands that dip in the middle: the shelves' transitions can
                // overshoot, so measure the peak and keep it at the intended max.
                let peak = self.peak_gain(top, &rm1);
                if peak > max_g {
                    g *= max_g * 0.9995 / peak;
                }
            }
            self.g[i] = g * scale;
        }
        for ((on, rm1), z) in self.shelf_on.iter_mut().zip(&self.rm1).zip(&mut self.z) {
            let was = *on;
            *on = rm1.iter().any(|&r| r != 0.0);
            if *on && !was {
                // Back in use (its ratio just left 1): drop the stale state.
                *z = [0.0; N];
            }
        }
    }

    fn peak_gain(&self, g: f32, rm1: &[f32; 3]) -> f32 {
        let mut peak = 0.0f32;
        for &(c, s) in &self.probe {
            let mut m = g;
            for (&a, &k) in self.a.iter().zip(rm1) {
                // One-pole low-pass response a / (1 - b e^-jw), then 1 + k H.
                let b = 1.0 - a;
                let (dr, di) = (1.0 - b * c, b * s);
                let den = dr * dr + di * di;
                let (hr, hi) = (a * dr / den, -a * di / den);
                let (sr, si) = (1.0 + k * hr, k * hi);
                m *= (sr * sr + si * si).sqrt();
            }
            peak = peak.max(m);
        }
        peak
    }

    /// Advances the modulation (once per control step).
    pub fn control(&mut self) {
        for w in &mut self.wobble[..N / 4] {
            w.control();
        }
    }

    /// One sample: each line's input (scaled by `gain`) into the tank;
    /// returns the left and right outputs.
    #[inline]
    #[allow(clippy::needless_range_loop)] // parallel per-line arrays, indexed so they vectorise
    pub fn tick(&mut self, x: &[f32; N], gain: f32) -> (f32, f32) {
        let mut v = [0.0f32; N];
        for i in 0..N / 4 {
            let w = &mut self.wobble[i];
            w.cur += w.slope;
            self.len[i] += self.len_inc[i];
            v[i] = self.lines.hermite(i, self.len[i] + w.cur * self.depth);
        }
        if self.moving {
            for i in N / 4..N {
                self.len[i] += self.len_inc[i];
                v[i] = self.lines.hermite(i, self.len[i]);
            }
        } else {
            for i in N / 4..N {
                v[i] = self.lines.tap(i, self.ilen[i]);
            }
        }
        // The arithmetic runs on local copies so it vectorises (no aliasing
        // with the state it updates).
        let g = self.g;
        for i in 0..N {
            v[i] *= g[i];
        }
        for k in 0..3 {
            if !self.shelf_on[k] {
                continue;
            }
            let a = self.a[k];
            let rm1 = self.rm1[k];
            let mut z = self.z[k];
            for i in 0..N {
                z[i] += a * (v[i] - z[i]);
                v[i] += rm1[i] * z[i];
            }
            self.z[k] = z;
        }
        // Outputs: even lines left, odd lines right, summed as a tree.
        let sgn_out = self.sgn_out;
        let mut t = [0.0f32; N];
        for i in 0..N {
            t[i] = v[i] * sgn_out[i];
        }
        let mut n = N;
        while n > 2 {
            n /= 2;
            for i in 0..n {
                t[i] += t[i + n];
            }
        }
        hadamard(&mut v);
        let sgn_in = self.sgn_in;
        for i in 0..N {
            v[i] = (v[i] + sgn_in[i] * gain * x[i] + TINY).clamp(-LIM, LIM);
        }
        if let Some(ap) = &mut self.aps {
            let g = self.ap_gain;
            let mut d = [0.0f32; N];
            if self.moving {
                for i in 0..N {
                    self.ap_len[i] += self.ap_inc[i];
                    d[i] = ap.hermite(i, self.ap_len[i]);
                }
            } else {
                for i in 0..N {
                    d[i] = ap.tap(i, self.ap_ilen[i]);
                }
            }
            let mut w = [0.0f32; N];
            for i in 0..N {
                w[i] = v[i] + g * d[i];
                v[i] = d[i] - g * w[i];
            }
            ap.push(&w);
        }
        self.lines.push(&v);
        (t[0], t[1])
    }
}

/// A chain of four Schroeder allpasses smearing transients into a dense
/// wash before the tank (the 480L's input diffusion).
pub(super) struct Diffuser {
    lines: Vec<DelayLine>,
    base: [f32; 4],
    pos: [Pos; 4],
    gain: [f32; 4],
}

impl Diffuser {
    /// Stage delays in ms at scale 1; `max_scale`: the largest scale used.
    pub fn new(sr: f32, ms: [f32; 4], max_scale: f32) -> Self {
        let base = ms.map(|m| m * 0.001 * sr);
        let lines = base.iter().map(|b| DelayLine::new((b * max_scale) as usize + 8)).collect();
        Self { lines, base, pos: [Pos::default(); 4], gain: [0.0; 4] }
    }

    pub fn clear(&mut self) {
        for l in &mut self.lines {
            l.clear();
        }
    }

    /// Delay scale, diffusion 0..1 and whether the scale is gliding.
    pub fn set(&mut self, scale: f32, diffusion: f32, moving: bool) {
        let d = diffusion.clamp(0.0, 1.0);
        self.gain = [0.75 * d, 0.75 * d, 0.625 * d, 0.625 * d];
        for ((p, &b), line) in self.pos.iter_mut().zip(&self.base).zip(&self.lines) {
            p.set((b * scale).clamp(2.0, line.max_delay() as f32 - 3.0), moving);
        }
    }

    #[inline]
    pub fn tick(&mut self, mut x: f32) -> f32 {
        for ((line, p), &g) in self.lines.iter_mut().zip(&mut self.pos).zip(&self.gain) {
            let y = p.read(line);
            let w = x + g * y + TINY;
            line.push(w);
            x = y - g * w;
        }
        x
    }
}

/// Pre-delay plus the build-up contour (the 480L's SHAPE): one tap per tank
/// line, spread irregularly over `span` after the pre-delay, with gains that
/// fall (front-loaded attack), stay flat or rise (slow swell) with `shape`,
/// optionally faded in over the first `ramp` samples (a plate's ATTACK).
/// Each tap feeds its own line (even lines from the left, odd from the
/// right), so the taps never sum into a comb filter.
pub(super) struct BuildUp<const N: usize> {
    /// Left and right pre-delay lines.
    lines: Lines<2>,
    pub pre: Param,
    pub span: Param,
    pub shape: Param,
    pub ramp: Param,
    /// Each line's place in the span (0..1).
    u: [f32; N],
    /// Tap positions (gliding per sample while `frac`), steps, integer taps.
    pos: [f32; N],
    inc: [f32; N],
    ipos: [usize; N],
    frac: bool,
    gain: [f32; N],
}

impl<const N: usize> BuildUp<N> {
    pub fn new(sr: f32, max_ms: f32, seed: u32) -> Self {
        let cap = (max_ms * 0.001 * sr) as usize + 8;
        let half = (N / 2).max(1);
        let mut u = [0.0; N];
        for (i, u) in u.iter_mut().enumerate() {
            let k = i / 2;
            // The first tap of each side sits right at the pre-delay.
            let jit = if k == 0 { 0.0 } else { 0.7 * (hash01(i as u32, seed) - 0.5) };
            *u = ((k as f32 + jit) / (half as f32 - 0.5).max(1.0)).clamp(0.0, 1.0);
        }
        Self {
            lines: Lines::new(cap),
            pre: Param::default(),
            span: Param::default(),
            shape: Param::default(),
            ramp: Param::default(),
            u,
            pos: [1.0; N],
            inc: [0.0; N],
            ipos: [1; N],
            frac: false,
            gain: [0.0; N],
        }
    }

    pub fn clear(&mut self) {
        self.lines.clear();
    }

    pub fn snap(&mut self) {
        self.pre.snap();
        self.span.snap();
        self.shape.snap();
        self.ramp.snap();
    }

    /// Steps the controls (pre-delay, span and ramp in samples, shape 0..1).
    pub fn control(&mut self, a: f32, force: bool) {
        let lim = 0.4 * CTL as f32;
        let moved = self.pre.step_limited(a, lim) | self.span.step_limited(a, lim);
        let shaped = self.shape.step(a) | self.ramp.step(a) | moved;
        if moved || force {
            self.frac = self.pre.moving() || self.span.moving();
            let max = self.lines.max_delay() - 3.0;
            let k = 1.0 / CTL as f32;
            for i in 0..N {
                let t = (self.pre.cur + self.span.cur * self.u[i]).clamp(1.0, max);
                if self.frac {
                    self.inc[i] = (t - self.pos[i]) * k;
                } else {
                    self.pos[i] = t;
                    self.inc[i] = 0.0;
                }
                self.ipos[i] = t.round() as usize;
            }
        }
        if shaped || force {
            let c = (self.shape.cur - 0.5) * 7.0;
            let mut sum = [0.0f32; 2];
            let ramp = self.ramp.cur;
            for (i, (g, &u)) in self.gain.iter_mut().zip(&self.u).enumerate() {
                *g = (c * (u - 0.5)).exp();
                if ramp > 1.0 {
                    *g *= ((self.span.cur * u + 0.1 * ramp) / ramp).min(1.0);
                }
                sum[i % 2] += *g * *g;
            }
            // Each side injects as much energy as one full-level input per line.
            let half = (N / 2) as f32;
            for (i, g) in self.gain.iter_mut().enumerate() {
                *g *= (half / sum[i % 2]).sqrt();
            }
        }
    }

    /// Feeds the (diffused) input; returns each line's input and the summed
    /// taps per side (an early sound, level-normalised).
    #[inline]
    #[allow(clippy::needless_range_loop)] // parallel per-tap arrays
    pub fn tick(&mut self, xl: f32, xr: f32) -> ([f32; N], (f32, f32)) {
        self.lines.push(&[xl, xr]);
        let mut v = [0.0f32; N];
        if self.frac {
            for i in 0..N {
                self.pos[i] += self.inc[i];
                v[i] = self.gain[i] * self.lines.hermite(i & 1, self.pos[i]);
            }
        } else {
            for i in 0..N {
                v[i] = self.gain[i] * self.lines.tap(i & 1, self.ipos[i]);
            }
        }
        let (mut el, mut er) = (0.0, 0.0);
        for pair in v.chunks_exact(2) {
            el += pair[0];
            er += pair[1];
        }
        let n = 1.0 / ((N / 2) as f32).sqrt();
        (v, (el * n, er * n))
    }
}

/// A set of discrete taps per channel (early reflections, reflection
/// clusters). Tap times and gains glide to new settings.
pub(super) struct Taps<const K: usize> {
    lines: Vec<DelayLine>,
    pos: [[Param; K]; 2],
    gain: [[Param; K]; 2],
    read: [[Pos; K]; 2],
    n: usize,
}

impl<const K: usize> Taps<K> {
    /// `n` (<= K) taps per channel, up to `max_ms` long.
    pub fn new(sr: f32, max_ms: f32, n: usize) -> Self {
        let cap = (max_ms * 0.001 * sr) as usize + 8;
        Self {
            lines: vec![DelayLine::new(cap), DelayLine::new(cap)],
            pos: [[Param::default(); K]; 2],
            gain: [[Param::default(); K]; 2],
            read: [[Pos::default(); K]; 2],
            n: n.min(K),
        }
    }

    pub fn clear(&mut self) {
        for l in &mut self.lines {
            l.clear();
        }
    }

    /// Sets tap `k` of channel `ch`: position in samples, gain.
    pub fn set(&mut self, ch: usize, k: usize, pos: f32, gain: f32) {
        let max = self.lines[0].max_delay() as f32 - 3.0;
        self.pos[ch][k].set(pos.clamp(1.0, max));
        self.gain[ch][k].set(gain);
    }

    pub fn snap(&mut self) {
        for p in self.pos.iter_mut().chain(self.gain.iter_mut()).flatten() {
            p.snap();
        }
        // The reads jump too (control() only moves them while a position glides).
        for (rd, pos) in self.read.iter_mut().zip(&self.pos) {
            for (r, p) in rd.iter_mut().zip(pos) {
                r.set(p.cur, false);
            }
        }
    }

    pub fn control(&mut self, a: f32, force: bool) {
        // Positions glide at most half a sample per sample (a bend, not a smear).
        let mut moved = false;
        for p in self.pos.iter_mut().flatten() {
            moved |= p.step_limited(a, 0.5 * CTL as f32);
        }
        for g in self.gain.iter_mut().flatten() {
            g.step(a);
        }
        if moved || force {
            let frac = self.pos.iter().flatten().any(|p| p.moving());
            for (rd, pos) in self.read.iter_mut().zip(&self.pos) {
                for (r, p) in rd.iter_mut().zip(pos) {
                    r.set(p.cur, frac);
                }
            }
        }
    }

    #[inline]
    pub fn tick(&mut self, xl: f32, xr: f32) -> (f32, f32) {
        self.lines[0].push(xl);
        self.lines[1].push(xr);
        let mut out = [0.0f32; 2];
        for (((o, line), rd), gain) in out.iter_mut().zip(&self.lines).zip(&mut self.read).zip(&self.gain) {
            for (r, g) in rd[..self.n].iter_mut().zip(&gain[..self.n]) {
                *o += g.cur * r.read(line);
            }
        }
        (out[0], out[1])
    }
}

/// A delay read whose time jumps with a short crossfade between two heads,
/// like a digital delay's time control: no pitch sweep, no click.
#[derive(Clone, Copy, Debug)]
pub(super) struct XHead {
    pos: [usize; 2],
    active: usize,
    target: usize,
    fade: f32,
    step: f32,
}

impl XHead {
    /// Crossfades over `ms`.
    pub fn new(sr: f32, ms: f32) -> Self {
        Self { pos: [1; 2], active: 0, target: 1, fade: 1.0, step: 1.0 / (ms * 0.001 * sr).max(1.0) }
    }

    /// New delay time in samples.
    pub fn set(&mut self, d: f32) {
        self.target = d.round().max(1.0) as usize;
    }

    /// Goes to the target at once (first parameters).
    pub fn snap(&mut self) {
        self.pos = [self.target; 2];
        self.fade = 1.0;
    }

    /// Starts a crossfade to a new target once the last one has finished.
    pub fn control(&mut self) {
        if self.fade >= 1.0 && self.target != self.pos[self.active] {
            self.pos[1 - self.active] = self.target;
            self.fade = 0.0;
        }
    }

    #[inline]
    pub fn read(&mut self, line: &DelayLine) -> f32 {
        let a = line.tap(self.pos[self.active]);
        if self.fade >= 1.0 {
            return a;
        }
        let b = line.tap(self.pos[1 - self.active]);
        self.fade += self.step;
        if self.fade >= 1.0 {
            self.active = 1 - self.active;
            return b;
        }
        a + (b - a) * self.fade
    }
}

/// The 480L's pre-echoes: a delay per side fed from after the input diffusion
/// and returned before it (FEED; the loop is damped above ~6 kHz), and heard
/// in the output at `level`. Time 0 switches a side off.
pub(super) struct Echo {
    lines: Vec<DelayLine>,
    heads: [XHead; 2],
    pub feed: [Param; 2],
    pub level: [Param; 2],
    z: [f32; 2],
    a: f32,
}

impl Echo {
    pub fn new(sr: f32, max_ms: f32) -> Self {
        let cap = (max_ms * 0.001 * sr) as usize + 8;
        Self {
            lines: vec![DelayLine::new(cap), DelayLine::new(cap)],
            heads: [XHead::new(sr, 30.0); 2],
            feed: [Param::default(); 2],
            level: [Param::default(); 2],
            z: [0.0; 2],
            a: pole(6000.0, sr),
        }
    }

    pub fn clear(&mut self) {
        for l in &mut self.lines {
            l.clear();
        }
        self.z = [0.0; 2];
    }

    /// Side `ch`: time (samples, < 1 = off), feedback (-1..1), output level.
    pub fn set(&mut self, ch: usize, time: f32, feed: f32, level: f32) {
        let max = self.lines[0].max_delay() as f32 - 3.0;
        let on = time >= 1.0;
        if on {
            self.heads[ch].set(time.clamp(1.0, max));
        }
        self.feed[ch].set(if on { feed.clamp(-0.95, 0.95) } else { 0.0 });
        self.level[ch].set(if on { level } else { 0.0 });
    }

    pub fn snap(&mut self) {
        for p in self.feed.iter_mut().chain(&mut self.level) {
            p.snap();
        }
        for h in &mut self.heads {
            h.snap();
        }
    }

    pub fn control(&mut self, a: f32) {
        for h in &mut self.heads {
            h.control();
        }
        for p in self.feed.iter_mut().chain(&mut self.level) {
            p.step(a);
        }
    }

    /// The (damped) echoes due now.
    #[inline]
    pub fn read(&mut self) -> [f32; 2] {
        for ((z, line), head) in self.z.iter_mut().zip(&self.lines).zip(&mut self.heads) {
            *z += self.a * (head.read(line) - *z);
        }
        self.z
    }

    /// Feeds the diffused signal.
    #[inline]
    pub fn write(&mut self, l: f32, r: f32) {
        self.lines[0].push((l + TINY).clamp(-LIM, LIM));
        self.lines[1].push((r + TINY).clamp(-LIM, LIM));
    }
}

/// The wet output: lo cut and hi cut (12 dB/oct), stereo width (mid/side;
/// 0 = mono, 1 = as generated, more is wider), balance and level.
pub(super) struct Output {
    sr: f32,
    hp: [Svf; 2],
    lp: [Svf; 2],
    pub lc: Param,
    pub hc: Param,
    pub width: Param,
    pub gain: Param,
    pub bal: Param,
}

impl Output {
    pub fn new(sr: f32) -> Self {
        Self {
            sr,
            hp: [Svf::default(); 2],
            lp: [Svf::default(); 2],
            lc: Param { cur: 20.0, target: 20.0 },
            hc: Param { cur: 20000.0, target: 20000.0 },
            width: Param { cur: 1.0, target: 1.0 },
            gain: Param { cur: 1.0, target: 1.0 },
            bal: Param::default(),
        }
    }

    pub fn clear(&mut self) {
        for f in self.hp.iter_mut().chain(&mut self.lp) {
            f.reset();
        }
    }

    pub fn snap(&mut self) {
        for p in [&mut self.lc, &mut self.hc, &mut self.width, &mut self.gain, &mut self.bal] {
            p.snap();
        }
    }

    pub fn control(&mut self, a: f32, force: bool) {
        if self.lc.step(a) || force {
            for f in &mut self.hp {
                f.set(self.lc.cur, 0.707, self.sr);
            }
        }
        if self.hc.step(a) || force {
            for f in &mut self.lp {
                f.set(self.hc.cur, 0.707, self.sr);
            }
        }
        self.width.step(a);
        self.gain.step(a);
        self.bal.step(a);
    }

    #[inline]
    pub fn tick(&mut self, l: f32, r: f32) -> (f32, f32) {
        let l = self.lp[0].tick(self.hp[0].tick(l).hp).lp;
        let r = self.lp[1].tick(self.hp[1].tick(r).hp).lp;
        let m = 0.5 * (l + r);
        let s = 0.5 * (l - r) * self.width.cur;
        let b = self.bal.cur;
        let g = self.gain.cur;
        ((m + s) * g * (1.0 - b).min(1.0), (m - s) * g * (1.0 + b).min(1.0))
    }
}
