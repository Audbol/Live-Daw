//! A splicing delay-line pitch shifter.
//!
//! A read head moves through a delay line at `ratio` times the write speed, so
//! its delay grows or shrinks by `1 - ratio` per sample. When it reaches the
//! end of its range a second head is started one jump (about a window) away and
//! the two are crossfaded. The jump is not fixed: within a search span the
//! splice point whose recent past correlates best with the running head is
//! chosen, so on harmonic material the heads line up period for period and the
//! crossfade neither flanges nor cancels. Poorly correlated material (noise,
//! silence) uses the nominal jump and an equal-power crossfade.
//!
//! Stereo instances share their splice decisions (made on L + R), so the image
//! stays put. An extra delay (`offset`) moves both heads, giving the devices
//! their DELAY controls and modulated timing for free.

use super::super::blocks::DelayLine;
use std::f32::consts::FRAC_PI_2;

/// Pitch ratios outside this range are clamped (two octaves either way).
pub const MIN_RATIO: f32 = 0.25;
pub const MAX_RATIO: f32 = 4.0;

/// Fixed settings of a [`Shifter`] (all in ms).
#[derive(Debug, Clone, Copy)]
pub struct ShifterSpec {
    /// Longest `offset` the caller will use.
    pub max_offset_ms: f32,
    /// Smallest read delay (latency floor).
    pub min_ms: f32,
    /// Nominal splice jump, and the span a head travels between splices.
    pub window_ms: f32,
    /// Range of jumps searched for the best splice (covers the longest period
    /// of interest; at most `window_ms`).
    pub search_ms: f32,
    /// Correlation window.
    pub corr_ms: f32,
    /// Crossfade length.
    pub fade_ms: f32,
}

/// How the current head and the read positions of both heads mix this sample.
#[derive(Clone, Copy)]
struct Heads {
    da: f32,
    db: f32,
    ga: f32,
    gb: f32,
}

/// Splicing pitch shifter for one or two channels.
pub struct Shifter {
    lines: [DelayLine; 2],
    stereo: bool,
    /// Delay of each head (without `offset`), samples.
    d: [f32; 2],
    active: usize,
    /// Crossfade progress 0..1 (1 = no splice running).
    fade: f32,
    fade_step: f32,
    fade_len: f32,
    /// Equal-power (uncorrelated) instead of equal-gain crossfade.
    power: bool,
    lo: f32,
    window: f32,
    search: usize,
    corr: usize,
    d0: f32,
    scratch: Vec<f32>,
    energy: Vec<f64>,
}

impl Shifter {
    /// Message thread (allocates).
    pub fn new(spec: ShifterSpec, sr: f32, stereo: bool) -> Self {
        let ms = |v: f32| (v.max(0.0) * 0.001 * sr).round();
        let lo = ms(spec.min_ms).max(2.0);
        let window = ms(spec.window_ms).max(16.0);
        let search = (ms(spec.search_ms) as usize).clamp(1, window as usize - 4);
        let corr = (ms(spec.corr_ms) as usize).max(8);
        let fade_len = ms(spec.fade_ms).max(8.0);
        // Pitch down: the old head runs on during the fade (|rate| < 1); pitch
        // up: the correlation looks back a window plus the correlation length.
        let capacity = (ms(spec.max_offset_ms) + lo + 2.0 * window + 2.0 * fade_len) as usize + corr + 16;
        let line = |n: usize| DelayLine::new(n);
        let mut s = Self {
            lines: [line(capacity), line(if stereo { capacity } else { 4 })],
            stereo,
            d: [0.0; 2],
            active: 0,
            fade: 1.0,
            fade_step: 1.0 / fade_len,
            fade_len,
            power: false,
            lo,
            window,
            search,
            corr,
            d0: lo + (0.25 * window).round(),
            scratch: vec![0.0; window as usize + corr + 2],
            energy: vec![0.0; window as usize + corr + 3],
        };
        s.clear();
        s
    }

    pub fn clear(&mut self) {
        for l in &mut self.lines {
            l.clear();
        }
        self.d = [self.d0; 2];
        self.active = 0;
        self.fade = 1.0;
        self.power = false;
    }

    /// Base latency in samples at ratio 1 (without `offset`).
    #[cfg(test)]
    pub fn latency(&self) -> f32 {
        self.d0
    }

    /// One stereo sample. `offset` is an extra delay in samples (>= 0, at most
    /// the spec's `max_offset_ms`).
    #[inline]
    pub fn tick_stereo(&mut self, l: f32, r: f32, ratio: f32, offset: f32) -> (f32, f32) {
        self.lines[0].push(l);
        self.lines[1].push(r);
        let h = self.step(ratio, offset);
        (read(&self.lines[0], h), read(&self.lines[1], h))
    }

    /// One mono sample (instances made with `stereo = false`).
    #[inline]
    pub fn tick_mono(&mut self, x: f32, ratio: f32, offset: f32) -> f32 {
        self.lines[0].push(x);
        let h = self.step(ratio, offset);
        read(&self.lines[0], h)
    }

    #[inline]
    fn step(&mut self, ratio: f32, offset: f32) -> Heads {
        let rate = 1.0 - ratio.clamp(MIN_RATIO, MAX_RATIO);
        let offset = offset.max(0.0);
        let a = self.active;
        self.d[a] += rate;
        if self.fade < 1.0 {
            let b = 1 - a;
            self.d[b] += rate;
            self.fade = (self.fade + self.fade_step).min(1.0);
            let f = self.fade;
            let (ga, gb) = if self.power {
                let (s, c) = (f * FRAC_PI_2).sin_cos();
                (c, s)
            } else {
                let g = f * f * (3.0 - 2.0 * f);
                (1.0 - g, g)
            };
            if f >= 1.0 {
                self.active = b;
            }
            return Heads { da: self.d[a] + offset, db: self.d[b] + offset, ga, gb };
        }
        if rate < 0.0 && self.d[a] <= self.lo - rate * self.fade_len {
            self.splice(true, offset);
        } else if rate > 0.0 && self.d[a] >= self.lo + self.window {
            self.splice(false, offset);
        } else if self.d[a] < self.lo - 1.0 || self.d[a] > self.lo + self.window + 2.0 * self.fade_len {
            // Out of range after a large ratio change mid-fade: re-centre.
            self.d[a] = self.d[a].clamp(self.lo, self.lo + self.window);
        }
        Heads { da: self.d[a] + offset, db: 0.0, ga: 1.0, gb: 0.0 }
    }

    /// The analysis signal (mono, or L + R) `delay` samples back.
    #[inline]
    fn sample(&self, delay: usize) -> f32 {
        if self.stereo {
            self.lines[0].tap(delay) + self.lines[1].tap(delay)
        } else {
            self.lines[0].tap(delay)
        }
    }

    /// Starts a crossfade to a second head: `up` jumps to a longer delay (the
    /// head runs towards zero), otherwise to a shorter one.
    fn splice(&mut self, up: bool, offset: f32) {
        let a = self.active;
        let base = (self.d[a] + offset).round().max(1.0) as usize;
        let jhi = self.window as usize;
        let l = self.corr;
        // Region in increasing delay (going back in time), from `start`.
        let start = if up { base } else { base.saturating_sub(jhi) };
        let len = jhi + l;
        for i in 0..len {
            self.scratch[i] = self.sample(start + i);
        }
        self.energy[0] = 0.0;
        for i in 0..len {
            let s = self.scratch[i] as f64;
            self.energy[i + 1] = self.energy[i] + s * s;
        }
        let ref_off = base - start;
        let jmax = if up { jhi } else { jhi.min(ref_off) };
        let jmin = jhi.saturating_sub(self.search).max(1).min(jmax);
        let e_ref = self.energy[ref_off + l] - self.energy[ref_off];
        let mut best = (jmax, -2.0f32);
        if e_ref > 1e-10 {
            let reference = &self.scratch[ref_off..ref_off + l];
            for j in jmin..=jmax {
                let off = if up { ref_off + j } else { ref_off - j };
                let e = self.energy[off + l] - self.energy[off];
                let score = dot(reference, &self.scratch[off..off + l]) / ((e_ref * e).sqrt() as f32 + 1e-12);
                if score > best.1 {
                    best = (j, score);
                }
            }
        }
        let j = best.0 as f32;
        self.d[1 - a] = self.d[a] + if up { j } else { -j };
        self.power = best.1 < 0.5;
        self.fade = 0.0;
    }
}

#[inline]
fn read(line: &DelayLine, h: Heads) -> f32 {
    let mut y = 0.0;
    if h.ga != 0.0 {
        y += h.ga * line.tap_hermite(h.da);
    }
    if h.gb != 0.0 {
        y += h.gb * line.tap_hermite(h.db);
    }
    y
}

/// Dot product with independent partial sums (lets the compiler vectorise).
#[inline]
pub fn dot(a: &[f32], b: &[f32]) -> f32 {
    let n = a.len().min(b.len());
    let (a, b) = (&a[..n], &b[..n]);
    let mut acc = [0.0f32; 8];
    let mut ca = a.chunks_exact(8);
    let mut cb = b.chunks_exact(8);
    for (x, y) in (&mut ca).zip(&mut cb) {
        for i in 0..8 {
            acc[i] += x[i] * y[i];
        }
    }
    let mut sum: f32 = acc.iter().sum();
    for (x, y) in ca.remainder().iter().zip(cb.remainder()) {
        sum += x * y;
    }
    sum
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::f32::consts::TAU;

    fn spec() -> ShifterSpec {
        ShifterSpec { max_offset_ms: 0.0, min_ms: 1.0, window_ms: 40.0, search_ms: 16.0, corr_ms: 12.0, fade_ms: 10.0 }
    }

    #[test]
    fn unity_ratio_is_a_clean_delay() {
        let sr = 48_000.0;
        let mut s = Shifter::new(spec(), sr, false);
        let lat = s.latency() as usize;
        let x: Vec<f32> = (0..4800).map(|i| (TAU * 440.0 * i as f32 / sr).sin()).collect();
        let y: Vec<f32> = x.iter().map(|&v| s.tick_mono(v, 1.0, 0.0)).collect();
        for i in 2000..4800 {
            assert!((y[i] - x[i - lat]).abs() < 1e-3);
        }
    }

    #[test]
    fn splices_stay_in_phase_on_a_sine() {
        // A fifth up on a steady sine: the output keeps a constant envelope
        // (no dips from cancelling crossfades).
        let sr = 48_000.0;
        let mut s = Shifter::new(spec(), sr, false);
        let ratio = 2f32.powf(7.0 / 12.0);
        let mut lo = f32::MAX;
        let mut hi = 0.0f32;
        let mut peak = 0.0f32;
        for i in 0..96_000 {
            let y = s.tick_mono((TAU * 220.0 * i as f32 / sr).sin(), ratio, 0.0);
            peak = peak.max(y.abs());
            if i % 480 == 479 {
                if i > 9600 {
                    lo = lo.min(peak);
                    hi = hi.max(peak);
                }
                peak = 0.0;
            }
        }
        assert!(lo > 0.9 && hi < 1.1, "{lo} {hi}");
    }

    #[test]
    fn harmonic_material_does_not_flange() {
        // A band-limited sawtooth shifted up and down: the level measured over
        // 40 ms windows stays within 1.5 dB (fixed-jump splicers swing several
        // dB as harmonics cancel in the crossfades).
        let sr = 48_000.0;
        for semis in [-5.0f32, 4.0, 12.0] {
            let mut s = Shifter::new(spec(), sr, false);
            let ratio = 2f32.powf(semis / 12.0);
            let (mut lo, mut hi, mut e) = (f32::MAX, 0.0f32, 0.0f32);
            for i in 0..72_000 {
                let t = i as f32 / sr;
                let x: f32 = (1..=8).map(|k| (TAU * 147.0 * k as f32 * t).sin() / k as f32).sum();
                let y = s.tick_mono(0.3 * x, ratio, 0.0);
                e += y * y;
                if i % 1920 == 1919 {
                    if i > 9600 {
                        lo = lo.min(e);
                        hi = hi.max(e);
                    }
                    e = 0.0;
                }
            }
            let spread = 10.0 * (hi / lo).log10();
            assert!(spread < 1.5, "{semis}: {spread:.2} dB");
        }
    }
}
