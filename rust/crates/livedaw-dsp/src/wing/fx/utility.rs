//! Standard processors: graphic EQs, precision limiter, 2-band de-esser, enhancer,
//! exciter, velvet imager, speaker manager, combinator, triple DEQ, BodyRez and
//! tape machine.
//!
//! Each device lives in a submodule. This file holds what they share: parameter
//! glides that filters are redesigned from (so control moves never zipper), a
//! stereo biquad cascade, the console's high/low-pass families and the output
//! headroom stage of the devices whose gains can stack up.

mod body;
mod combinator;
mod deesser;
mod dyneq;
mod enhancer;
mod graphic;
mod imager;
mod limiter;
mod speaker;
mod tape;

use super::blocks::{fast_tanh, BiquadCoeffs};
use super::{FxAlgo, FxContext};
use std::f64::consts::FRAC_1_SQRT_2;

/// The devices this module implements.
pub(super) const TOKENS: &[&str] =
    &["GEQ", "PIA", "LIMITER", "DE-S2", "ENHANCE", "EXCITER", "V-IMG", "SPKMAN", "C5-CMB", "DEQ3", "BODY", "TAPE"];

pub(super) fn create(token: &str, ctx: FxContext) -> Option<Box<dyn FxAlgo>> {
    if !TOKENS.contains(&token) {
        return None;
    }
    let sr = ctx.sample_rate;
    Some(match token {
        "GEQ" => Box::new(graphic::Geq::new(sr)),
        "PIA" => Box::new(graphic::Pia::new(sr)),
        "LIMITER" => Box::new(limiter::PrecisionLimiter::new(sr)),
        "DE-S2" => Box::new(deesser::DeEsser::new(sr)),
        "ENHANCE" => Box::new(enhancer::Enhancer::new(sr)),
        "EXCITER" => Box::new(enhancer::Exciter::new(sr)),
        "V-IMG" => Box::new(imager::VelvetImager::new(sr)),
        "SPKMAN" => Box::new(speaker::SpeakerManager::new(sr)),
        "C5-CMB" => Box::new(combinator::Combinator::new(sr)),
        "DEQ3" => Box::new(dyneq::TripleDeq::new(sr)),
        "BODY" => Box::new(body::BodyRez::new(sr)),
        "TAPE" => Box::new(tape::Tape::new(sr)),
        _ => return None,
    })
}

// ---- shared pieces ---------------------------------------------------------------------

/// Filters are redesigned from glided parameters once per this many samples.
const SUB_BLOCK: usize = 32;

/// Calls `f(start, end)` for consecutive sub-blocks covering `0..len`.
#[inline]
fn sub_blocks(len: usize, mut f: impl FnMut(usize, usize)) {
    let mut start = 0;
    while start < len {
        let end = (start + SUB_BLOCK).min(len);
        f(start, end);
        start = end;
    }
}

/// Output headroom of the devices whose gains can stack (graphic EQs, dynamic
/// EQs, enhancers, channel strips): exactly linear up to +18 dBFS, then a soft
/// rail that never passes +24 dBFS, like an analog output stage. Normal levels
/// never reach it.
#[inline]
pub(super) fn rail(x: f32) -> f32 {
    const KNEE: f32 = 8.0;
    let a = x.abs();
    if a <= KNEE {
        x
    } else {
        // fast_tanh reaches exactly 1 (with zero slope) at 3.
        (KNEE + KNEE * fast_tanh(((a - KNEE) / KNEE).min(3.0))).copysign(x)
    }
}

/// log2 of a positive float, within 1e-5 (for level detectors, which run per sample).
#[inline]
fn fast_log2(x: f32) -> f32 {
    let bits = x.to_bits();
    let mut e = ((bits >> 23) & 0xff) as i32 - 127;
    let mut m = f32::from_bits((bits & 0x007f_ffff) | 0x3f80_0000);
    if m > std::f32::consts::SQRT_2 {
        m *= 0.5;
        e += 1;
    }
    // log2(m) = 2/ln 2 * atanh(t), t = (m - 1) / (m + 1), |t| < 0.18.
    let t = (m - 1.0) / (m + 1.0);
    let t2 = t * t;
    e as f32 + t * (2.885_39 + t2 * (0.961_797 + t2 * (0.577_078 + t2 * 0.412_199)))
}

/// 2^y, within 1e-6 relative (|y| <= 126).
#[inline]
fn fast_exp2(y: f32) -> f32 {
    let y = y.clamp(-126.0, 126.0);
    // floor() without the libm call (baseline x86-64 has no rounding instruction).
    let t = y as i32;
    let i = if (t as f32) > y { t - 1 } else { t };
    let f = y - i as f32;
    // Taylor series of e^(f ln 2) to the 7th power.
    let p = 1.0
        + f * (std::f32::consts::LN_2
            + f * (0.240_226_5
                + f * (0.055_504_11 + f * (0.009_618_129 + f * (0.001_333_355 + f * (1.540_353e-4 + f * 1.525_273e-5))))));
    p * f32::from_bits(((i + 127) as u32) << 23)
}

/// 20 log10(level); zero or tiny levels read as -400 dB.
#[inline]
fn level_db(x: f32) -> f32 {
    6.020_6 * fast_log2(x.max(1e-20))
}

/// 10 log10 of a mean square.
#[inline]
fn power_db(ms: f32) -> f32 {
    3.010_3 * fast_log2(ms.max(1e-30))
}

/// 10^(dB/20).
#[inline]
fn db_gain(db: f32) -> f32 {
    fast_exp2(db * (1.0 / 6.020_6))
}

/// One-pole coefficient for `ms` (as [`coef`](super::blocks::coef)), without
/// the exponential: for time constants of many samples, `1 - e^-x` is `x - x^2/2`
/// to well under 1e-6.
#[inline]
fn slow_coef(ms: f32, sr: f32) -> f32 {
    let x = (1000.0 / (ms * sr).max(1.0)).min(1.0);
    x * (1.0 - 0.5 * x)
}

/// `N` (<= 64) parameters gliding toward their targets, one step per sub-block.
/// Frequencies and Qs are glided as logarithms so they move evenly.
#[derive(Clone, Copy)]
struct Glide<const N: usize> {
    cur: [f32; N],
    tgt: [f32; N],
}

impl<const N: usize> Glide<N> {
    /// Fraction of the remaining distance covered per sub-block (about 10 ms to settle at 48 kHz).
    const RATE: f32 = 0.3;

    fn new(v: f32) -> Self {
        Self { cur: [v; N], tgt: [v; N] }
    }

    /// Jumps to the targets.
    fn snap(&mut self) {
        self.cur = self.tgt;
    }

    /// One sub-block's move. Bit `i` of the result is set if value `i` moved.
    #[inline]
    fn step(&mut self) -> u64 {
        let mut moved = 0;
        for (i, (c, &t)) in self.cur.iter_mut().zip(&self.tgt).enumerate() {
            if *c != t {
                let next = *c + (t - *c) * Self::RATE;
                *c = if (next - t).abs() <= 1e-4 * (1.0 + t.abs()) { t } else { next };
                moved |= 1u64 << i;
            }
        }
        moved
    }
}

/// A list of up to `N` biquad designs.
#[derive(Clone, Copy)]
struct Design<const N: usize> {
    c: [BiquadCoeffs; N],
    len: usize,
}

impl<const N: usize> Design<N> {
    fn new() -> Self {
        Self { c: [BiquadCoeffs::identity(); N], len: 0 }
    }

    fn push(&mut self, c: BiquadCoeffs) {
        if self.len < N {
            self.c[self.len] = c;
            self.len += 1;
        }
    }

    fn sections(&self) -> &[BiquadCoeffs] {
        &self.c[..self.len]
    }
}

/// A biquad running both channels in one loop (transposed direct form II,
/// double precision). The two channels are independent dependency chains, so
/// this runs about twice as fast as filtering them one after the other.
#[derive(Clone, Copy, Default)]
struct StereoBiquad {
    c: BiquadCoeffs,
    /// [channel][z1, z2].
    z: [[f64; 2]; 2],
}

impl StereoBiquad {
    fn set(&mut self, c: BiquadCoeffs) {
        self.c = c;
    }

    fn reset(&mut self) {
        self.z = [[0.0; 2]; 2];
    }

    #[inline]
    fn process(&mut self, l: &mut [f32], r: &mut [f32]) {
        let c = self.c;
        let [[mut l1, mut l2], [mut r1, mut r2]] = self.z;
        for (x, y) in l.iter_mut().zip(r.iter_mut()) {
            let (xl, xr) = (*x as f64, *y as f64);
            let ol = c.b0 * xl + l1;
            let or = c.b0 * xr + r1;
            l1 = c.b1 * xl - c.a1 * ol + l2;
            r1 = c.b1 * xr - c.a1 * or + r2;
            l2 = c.b2 * xl - c.a2 * ol;
            r2 = c.b2 * xr - c.a2 * or;
            *x = ol as f32;
            *y = or as f32;
        }
        self.z = [[l1, l2], [r1, r2]];
    }
}

/// Up to `N` biquads in series on both channels.
#[derive(Clone)]
struct Cascade<const N: usize> {
    sections: [StereoBiquad; N],
    len: usize,
}

impl<const N: usize> Cascade<N> {
    fn new() -> Self {
        Self { sections: [StereoBiquad::default(); N], len: 0 }
    }

    /// New coefficients. When the number of sections changes, the sections that
    /// come into use start clean.
    fn set(&mut self, design: &[BiquadCoeffs]) {
        let n = design.len().min(N);
        if n != self.len {
            for s in &mut self.sections[self.len.min(n)..] {
                s.reset();
            }
        }
        for (s, c) in self.sections.iter_mut().zip(design) {
            s.set(*c);
        }
        self.len = n;
    }

    fn process(&mut self, l: &mut [f32], r: &mut [f32]) {
        for s in &mut self.sections[..self.len] {
            s.process(l, r);
        }
    }

    fn reset(&mut self) {
        for s in &mut self.sections {
            s.reset();
        }
    }
}

/// Peaking EQ from a precomputed centre (`cos w`, `sin w`): the RBJ design
/// without the trigonometry, for banks of fixed-frequency bands.
#[inline]
fn peak_coeffs(cosw: f64, sinw: f64, q: f64, gain_db: f64) -> BiquadCoeffs {
    let alpha = sinw / (2.0 * q.max(0.025));
    let a = 10f64.powf(gain_db / 40.0);
    let a0 = 1.0 + alpha / a;
    BiquadCoeffs {
        b0: (1.0 + alpha * a) / a0,
        b1: -2.0 * cosw / a0,
        b2: (1.0 - alpha * a) / a0,
        a1: -2.0 * cosw / a0,
        a2: (1.0 - alpha / a) / a0,
    }
}

/// Appends a high- or low-pass of the console's family `kind` (FLAT, BW6, BW12,
/// BS12, LR12, BW18, BW24, BS24, LR24, BW48, LR48: Butterworth, Bessel and
/// Linkwitz-Riley at 6..48 dB/oct). Bessel sections are frequency-scaled so the
/// -3 dB point lands on `f`, as in the bus EQ's cut filters.
fn push_cut<const N: usize>(d: &mut Design<N>, sr: f64, kind: &str, f: f64, high_pass: bool) {
    let sec = |d: &mut Design<N>, q: f64, scale: f64| {
        let fs = if high_pass { f / scale } else { f * scale };
        d.push(if high_pass { BiquadCoeffs::high_pass(sr, fs, q) } else { BiquadCoeffs::low_pass(sr, fs, q) });
    };
    let first = |d: &mut Design<N>| {
        d.push(if high_pass { BiquadCoeffs::high_pass_1(sr, f) } else { BiquadCoeffs::low_pass_1(sr, f) });
    };
    match kind {
        "BW6" => first(d),
        "BW12" => sec(d, FRAC_1_SQRT_2, 1.0),
        "BS12" => sec(d, 0.5773, 1.2736),
        "LR12" => sec(d, 0.5, 1.0),
        "BW18" => {
            first(d);
            sec(d, 1.0, 1.0);
        }
        "BW24" => {
            sec(d, 0.5412, 1.0);
            sec(d, 1.3066, 1.0);
        }
        "BS24" => {
            sec(d, 0.5219, 1.4192);
            sec(d, 0.8055, 1.5912);
        }
        "LR24" => {
            sec(d, FRAC_1_SQRT_2, 1.0);
            sec(d, FRAC_1_SQRT_2, 1.0);
        }
        "BW48" => {
            for q in [0.5098, 0.6013, 0.9000, 2.5629] {
                sec(d, q, 1.0);
            }
        }
        "LR48" => {
            for q in [0.5412, 1.3066, 0.5412, 1.3066] {
                sec(d, q, 1.0);
            }
        }
        _ => {}
    }
}

/// Maps a 1..50 dial (the enhancer's frequency and Q knobs) onto `lo..hi`, logarithmically.
#[inline]
fn dial50(v: f32, lo: f32, hi: f32) -> f32 {
    lo * (hi / lo).powf(((v - 1.0) / 49.0).clamp(0.0, 1.0))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::wing::fx::{FxDevice, MAX_FX_PARAMS};
    use crate::wing::{model_index, FX_MODELS};
    use std::f32::consts::TAU;

    #[test]
    fn devices() {
        for token in super::TOKENS {
            crate::wing::fx::tests::check_device(token);
        }
    }

    /// A device at 48 kHz with its defaults, plus overrides by id.
    pub(super) struct Rig {
        pub dev: FxDevice,
        pub values: [f32; MAX_FX_PARAMS],
    }

    impl Rig {
        pub fn new(token: &str) -> Self {
            let model = model_index(FX_MODELS, token).unwrap();
            let dev = FxDevice::new(model, FxContext { sample_rate: 48_000.0, max_block: 256 }).unwrap();
            let mut values = [0.0; MAX_FX_PARAMS];
            FX_MODELS[model].defaults_into(&mut values);
            Self { dev, values }
        }

        pub fn set(&mut self, id: &str, v: f32) -> &mut Self {
            let i = self.dev.spec().param_index(id).unwrap_or_else(|| panic!("no parameter {id}"));
            self.values[i] = v;
            self
        }

        /// Processes `l`/`r` in place in blocks of 256.
        pub fn run(&mut self, l: &mut [f32], r: &mut [f32]) {
            for (a, b) in l.chunks_mut(256).zip(r.chunks_mut(256)) {
                self.dev.process(&self.values, a, b);
            }
        }

        /// Gain in dB of a sine at `freq` (same on both channels), measured on the left
        /// after the device has settled.
        pub fn sine_gain_db(&mut self, freq: f32, amp: f32) -> f32 {
            let n = 48_000;
            let x: Vec<f32> = (0..n).map(|i| (TAU * freq * i as f32 / 48_000.0).sin() * amp).collect();
            let (mut l, mut r) = (x.clone(), x.clone());
            self.run(&mut l, &mut r);
            let rms = |v: &[f32]| (v.iter().map(|s| s * s).sum::<f32>() / v.len() as f32).sqrt();
            20.0 * (rms(&l[n / 2..]) / rms(&x[n / 2..])).log10()
        }
    }

    #[test]
    fn glide_settles_and_reports_moves() {
        let mut g = Glide::<3>::new(0.0);
        g.tgt[1] = 6.0;
        assert_eq!(g.step(), 0b010);
        let mut steps = 1;
        while g.step() != 0 {
            steps += 1;
        }
        assert_eq!(g.cur[1], 6.0);
        assert!((10..60).contains(&steps), "{steps}");
    }

    #[test]
    fn fast_conversions_are_accurate() {
        let mut x = 1e-12f32;
        while x < 1e6 {
            assert!((level_db(x) - 20.0 * x.log10()).abs() < 1e-3, "{x}");
            assert!((power_db(x) - 10.0 * x.log10()).abs() < 1e-3, "{x}");
            x *= 1.37;
        }
        let mut d = -200.0f32;
        while d < 100.0 {
            let exact = 10f32.powf(d / 20.0);
            assert!((db_gain(d) / exact - 1.0).abs() < 1e-5, "{d}");
            d += 0.731;
        }
        assert_eq!(level_db(0.0), level_db(1e-20));
        for ms in [20.0f32, 200.0, 3000.0] {
            let exact = 1.0 - (-1000.0 / (ms as f64 * 44_100.0)).exp();
            assert!((slow_coef(ms, 44_100.0) as f64 / exact - 1.0).abs() < 1e-5, "{ms}");
        }
    }

    #[test]
    fn rail_is_linear_below_its_knee() {
        assert_eq!(rail(0.5), 0.5);
        assert_eq!(rail(-7.9), -7.9);
        assert!(rail(1000.0) <= 16.0 && rail(-1000.0) >= -16.0);
        assert!(rail(9.0) > rail(8.5));
    }

    #[test]
    fn cut_families_cut() {
        let sr = 48_000.0;
        for kind in ["BW6", "BW12", "BS12", "LR12", "BW18", "BW24", "BS24", "LR24", "BW48", "LR48"] {
            let mut d = Design::<4>::new();
            push_cut(&mut d, sr, kind, 1000.0, true);
            let mag = |f: f64| d.sections().iter().map(|c| c.magnitude_at(f, sr)).product::<f64>();
            assert!(mag(100.0) < 0.35, "{kind}");
            assert!((mag(15_000.0) - 1.0).abs() < 0.05, "{kind}");
        }
    }
}
