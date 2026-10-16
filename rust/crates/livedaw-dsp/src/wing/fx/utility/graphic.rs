//! Graphic EQs (WING manual 11.1).
//!
//! * GEQ - 31 bands on the ISO 1/3-octave centres (20 Hz .. 20 kHz, +-15 dB).
//!   STD behaves like an analog graphic: proportional-Q bands (broad for small
//!   moves, 1/3 octave at full throw) whose skirts add up, so neighbouring
//!   faders raise each other and the curve ripples. TRU is the TRUE CURVE
//!   function, which minimises the interaction between overlapping filters:
//!   the band gains are solved by weighted least squares so the combined
//!   response passes through every fader (and, more loosely, through the
//!   average of neighbours between them, so it doesn't ripple), then refined
//!   against the real response a few times. The solve runs in `set_params`,
//!   only when a fader moves. TRIM is the output level.
//! * PIA - the PIA 560 GEQ, after a 500-series (API 560) graphic: 10 bands an
//!   octave apart (31 Hz .. 16 kHz, +-12 dB) whose bandwidth narrows as the
//!   boost or cut grows. MIX (0..125 %) scales every band as all the console's
//!   EQ MIX controls do; GAIN is the output level.
//!
//! Only bands that are off zero run, so a flat graphic costs nothing and is
//! bit-transparent.

use super::{peak_coeffs, rail, sub_blocks, Glide, StereoBiquad};
use crate::wing::fx::blocks::{db_to_gain, BiquadCoeffs, Smoothed};
use crate::wing::fx::FxAlgo;
use crate::wing::Params;
use std::f64::consts::TAU;

/// A bank of peaking bands at fixed centres. Gains and Qs glide; bands at
/// exactly 0 dB are skipped.
struct Bank<const N: usize> {
    cosw: [f64; N],
    sinw: [f64; N],
    gain: Glide<N>,
    /// ln(Q).
    q: Glide<N>,
    filters: [StereoBiquad; N],
    active: [bool; N],
}

impl<const N: usize> Bank<N> {
    fn new(freqs: &[f64; N], sr: f64) -> Self {
        let w = |i: usize| TAU * freqs[i].min(sr * 0.49) / sr;
        Self {
            cosw: std::array::from_fn(|i| w(i).cos()),
            sinw: std::array::from_fn(|i| w(i).sin()),
            gain: Glide::new(0.0),
            q: Glide::new(0.0),
            filters: [StereoBiquad::default(); N],
            active: [false; N],
        }
    }

    fn set(&mut self, band: usize, gain_db: f32, q: f32) {
        self.gain.tgt[band] = gain_db;
        self.q.tgt[band] = q.max(0.1).ln();
    }

    fn snap(&mut self) {
        self.gain.snap();
        self.q.snap();
        self.redesign(u64::MAX);
    }

    fn redesign(&mut self, mask: u64) {
        for i in 0..N {
            if mask & (1u64 << i) == 0 {
                continue;
            }
            let g = self.gain.cur[i];
            if g == 0.0 {
                if self.active[i] {
                    self.filters[i].reset();
                }
                self.active[i] = false;
            } else {
                let c = peak_coeffs(self.cosw[i], self.sinw[i], self.q.cur[i].exp() as f64, g as f64);
                self.filters[i].set(c);
                self.active[i] = true;
            }
        }
    }

    fn process(&mut self, l: &mut [f32], r: &mut [f32]) {
        let n = l.len().min(r.len());
        sub_blocks(n, |a, b| {
            let moved = self.gain.step() | self.q.step();
            if moved != 0 {
                self.redesign(moved);
            }
            for (f, _) in self.filters.iter_mut().zip(&self.active).filter(|(_, on)| **on) {
                f.process(&mut l[a..b], &mut r[a..b]);
            }
        });
    }

    fn reset(&mut self) {
        for f in &mut self.filters {
            f.reset();
        }
        self.snap();
    }
}

// ---- GEQ -------------------------------------------------------------------------------

const BANDS: usize = 31;
/// Fader ids, low to high.
const GEQ_IDS: [&str; BANDS] = [
    "20", "25", "31", "40", "50", "63", "80", "100", "125", "160", "200", "250", "315", "400", "500", "630", "800", "1k", "1k25",
    "1k6", "2k", "2k5", "3k15", "4k", "5k", "6k3", "8k", "10k", "12k5", "16k", "20k",
];
/// STD: Q at the smallest and the largest boost/cut (1/3 octave = 4.32).
const STD_Q: (f32, f32) = (1.8, 4.3);
/// TRU: the bands' fixed Q (narrow enough for alternating +-15 dB faders).
const TRU_Q: f64 = 3.5;
/// TRU: weight of the midpoints between faders in the fit (the faders weigh 1).
const TRU_MID_WEIGHT: f64 = 0.3;

fn geq_freqs() -> [f64; BANDS] {
    std::array::from_fn(|i| 1000.0 * 2f64.powf((i as f64 - 17.0) / 3.0))
}

fn std_q(gain_db: f32) -> f32 {
    STD_Q.0 + (STD_Q.1 - STD_Q.0) * (gain_db.abs() / 15.0).min(1.0)
}

/// Response points of the TRU solver: the band centres and the midpoints between.
const POINTS: usize = 2 * BANDS - 1;

/// Least-squares design of the TRU band gains.
struct TruSolver {
    cosw: [f64; BANDS],
    sinw: [f64; BANDS],
    /// cos w, sin w, cos 2w, sin 2w at each point.
    trig: [[f64; 4]; POINTS],
    /// Band gains = `pinv` . (target dB at the points).
    pinv: [[f64; POINTS]; BANDS],
}

impl TruSolver {
    /// Gain of the prototype bands the interaction matrix is measured with.
    const PROTO_DB: f64 = 12.0;
    /// The fit plus refinements against the real response (it has converged by then).
    const PASSES: usize = 4;

    /// Message thread: builds the interaction matrix and its pseudo-inverse.
    fn new(sr: f64) -> Box<Self> {
        let f = geq_freqs().map(|x| x.min(sr * 0.49));
        let w = |hz: f64| TAU * hz / sr;
        let point = |k: usize| if k % 2 == 0 { f[k / 2] } else { (f[k / 2] * f[k / 2 + 1]).sqrt() };
        let mut s = Box::new(Self {
            cosw: f.map(|x| w(x).cos()),
            sinw: f.map(|x| w(x).sin()),
            trig: std::array::from_fn(|k| {
                let w = w(point(k));
                [w.cos(), w.sin(), (2.0 * w).cos(), (2.0 * w).sin()]
            }),
            pinv: [[0.0; POINTS]; BANDS],
        });

        // B[k][j]: response (dB) of band j at point k, per dB of band gain.
        let mut b = vec![[0.0f64; BANDS]; POINTS];
        for j in 0..BANDS {
            let c = s.coeffs(j, Self::PROTO_DB);
            for (row, t) in b.iter_mut().zip(&s.trig) {
                row[j] = 10.0 * mag2(&c, t).log10() / Self::PROTO_DB;
            }
        }
        let weight = |k: usize| if k % 2 == 0 { 1.0 } else { TRU_MID_WEIGHT };
        // pinv = (B'WB + lambda I)^-1 B'W
        let mut m = vec![[0.0f64; BANDS]; BANDS];
        for (i, mi) in m.iter_mut().enumerate() {
            for (j, mij) in mi.iter_mut().enumerate() {
                *mij = b.iter().enumerate().map(|(k, row)| weight(k) * row[i] * row[j]).sum::<f64>()
                    + if i == j { 1e-4 } else { 0.0 };
            }
        }
        let inv = invert(m);
        for (i, out) in s.pinv.iter_mut().enumerate() {
            for (k, o) in out.iter_mut().enumerate() {
                *o = weight(k) * (0..BANDS).map(|j| inv[i][j] * b[k][j]).sum::<f64>();
            }
        }
        s
    }

    fn coeffs(&self, j: usize, g: f64) -> BiquadCoeffs {
        peak_coeffs(self.cosw[j], self.sinw[j], TRU_Q, g)
    }

    /// Band gains that make the combined response follow `faders`.
    fn solve(&self, faders: &[f32; BANDS], out: &mut [f32; BANDS]) {
        let target: [f64; POINTS] =
            std::array::from_fn(
                |k| {
                    if k % 2 == 0 {
                        faders[k / 2] as f64
                    } else {
                        0.5 * (faders[k / 2] + faders[k / 2 + 1]) as f64
                    }
                },
            );
        let mut g = [0.0f64; BANDS];
        let mut err = target;
        for pass in 0..Self::PASSES {
            for (gi, row) in g.iter_mut().zip(&self.pinv) {
                *gi = (*gi + row.iter().zip(&err).map(|(p, e)| p * e).sum::<f64>()).clamp(-30.0, 30.0);
            }
            if pass + 1 < Self::PASSES {
                let resp = self.response(&g);
                for ((e, t), r) in err.iter_mut().zip(&target).zip(&resp) {
                    *e = t - r;
                }
            }
        }
        for (o, gi) in out.iter_mut().zip(g) {
            // Exactly flat faders give exactly zero gains (bands stay off).
            *o = if gi.abs() < 1e-4 { 0.0 } else { gi as f32 };
        }
    }

    /// Combined response (dB) of bands with gains `g` at every point.
    fn response(&self, g: &[f64; BANDS]) -> [f64; POINTS] {
        let mut prod = [1.0f64; POINTS];
        for (j, &gj) in g.iter().enumerate() {
            if gj == 0.0 {
                continue;
            }
            let c = self.coeffs(j, gj);
            for (p, t) in prod.iter_mut().zip(&self.trig) {
                *p *= mag2(&c, t);
            }
        }
        prod.map(|p| 10.0 * p.max(1e-30).log10())
    }
}

/// |H|^2 of a biquad at the point described by `t` = (cos w, sin w, cos 2w, sin 2w).
#[inline]
fn mag2(c: &BiquadCoeffs, t: &[f64; 4]) -> f64 {
    let nr = c.b0 + c.b1 * t[0] + c.b2 * t[2];
    let ni = c.b1 * t[1] + c.b2 * t[3];
    let dr = 1.0 + c.a1 * t[0] + c.a2 * t[2];
    let di = c.a1 * t[1] + c.a2 * t[3];
    (nr * nr + ni * ni) / (dr * dr + di * di)
}

/// Gauss-Jordan inverse with partial pivoting (message thread).
fn invert(mut m: Vec<[f64; BANDS]>) -> Vec<[f64; BANDS]> {
    let n = m.len();
    let mut inv: Vec<[f64; BANDS]> = (0..n).map(|i| std::array::from_fn(|j| if i == j { 1.0 } else { 0.0 })).collect();
    for col in 0..n {
        let pivot = (col..n).max_by(|&a, &b| m[a][col].abs().total_cmp(&m[b][col].abs())).unwrap_or(col);
        m.swap(col, pivot);
        inv.swap(col, pivot);
        let d = m[col][col];
        let d = if d.abs() < 1e-12 { 1e-12 } else { d };
        for j in 0..n {
            m[col][j] /= d;
            inv[col][j] /= d;
        }
        let (pivot_m, pivot_inv) = (m[col], inv[col]);
        for row in (0..n).filter(|&row| row != col) {
            let f = m[row][col];
            if f != 0.0 {
                for j in 0..n {
                    m[row][j] -= f * pivot_m[j];
                    inv[row][j] -= f * pivot_inv[j];
                }
            }
        }
    }
    inv
}

/// The 31-band graphic EQ.
pub(crate) struct Geq {
    bank: Bank<BANDS>,
    solver: Box<TruSolver>,
    trim: Smoothed,
    primed: bool,
}

impl Geq {
    pub(crate) fn new(sr: f32) -> Self {
        let sr64 = sr as f64;
        Self {
            bank: Bank::new(&geq_freqs(), sr64),
            solver: TruSolver::new(sr64),
            trim: Smoothed::new(1.0, 20.0, sr),
            primed: false,
        }
    }
}

impl FxAlgo for Geq {
    fn set_params(&mut self, p: &Params) {
        let mut faders = [0.0f32; BANDS];
        for (f, id) in faders.iter_mut().zip(GEQ_IDS) {
            *f = p.f(id);
        }
        if p.choice("type") == 1 {
            let mut gains = [0.0f32; BANDS];
            self.solver.solve(&faders, &mut gains);
            for (i, g) in gains.into_iter().enumerate() {
                self.bank.set(i, g, TRU_Q as f32);
            }
        } else {
            for (i, g) in faders.into_iter().enumerate() {
                self.bank.set(i, g, std_q(g));
            }
        }
        self.trim.target = db_to_gain(p.f("TRIM"));
        if !self.primed {
            self.bank.snap();
            self.trim.snap(self.trim.target);
            self.primed = true;
        }
    }

    fn process(&mut self, left: &mut [f32], right: &mut [f32]) {
        self.bank.process(left, right);
        for (l, r) in left.iter_mut().zip(right.iter_mut()) {
            let g = self.trim.tick();
            *l = rail(*l * g);
            *r = rail(*r * g);
        }
    }

    fn reset(&mut self) {
        self.bank.reset();
        self.trim.snap(self.trim.target);
    }
}

// ---- PIA 560 ---------------------------------------------------------------------------

const PIA_IDS: [&str; 10] = ["31", "63", "125", "250", "500", "1k", "2k", "4k", "8k", "16k"];
const PIA_FREQS: [f64; 10] = [31.25, 62.5, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0];

/// API 560 style proportional Q: about 1.5 octaves wide for small moves, under
/// an octave at full boost or cut.
fn pia_q(gain_db: f32) -> f32 {
    0.8 + 1.2 * (gain_db.abs() / 12.0).min(1.25)
}

/// The PIA 560 10-band graphic.
pub(crate) struct Pia {
    bank: Bank<10>,
    gain: Smoothed,
    primed: bool,
}

impl Pia {
    pub(crate) fn new(sr: f32) -> Self {
        Self { bank: Bank::new(&PIA_FREQS, sr as f64), gain: Smoothed::new(1.0, 20.0, sr), primed: false }
    }
}

impl FxAlgo for Pia {
    fn set_params(&mut self, p: &Params) {
        let k = p.f("mix") / 100.0;
        for (i, id) in PIA_IDS.iter().enumerate() {
            let g = p.f(id) * k;
            self.bank.set(i, g, pia_q(g));
        }
        self.gain.target = db_to_gain(p.f("g"));
        if !self.primed {
            self.bank.snap();
            self.gain.snap(self.gain.target);
            self.primed = true;
        }
    }

    fn process(&mut self, left: &mut [f32], right: &mut [f32]) {
        self.bank.process(left, right);
        for (l, r) in left.iter_mut().zip(right.iter_mut()) {
            let g = self.gain.tick();
            *l = rail(*l * g);
            *r = rail(*r * g);
        }
    }

    fn reset(&mut self) {
        self.bank.reset();
        self.gain.snap(self.gain.target);
    }
}

#[cfg(test)]
mod tests {
    use super::super::tests::Rig;
    use super::*;

    #[test]
    fn flat_geq_is_unity() {
        for kind in [0.0, 1.0] {
            let mut rig = Rig::new("GEQ");
            rig.set("type", kind);
            let x: Vec<f32> = (0..4800).map(|i| ((i * 7919) % 1000) as f32 / 1000.0 - 0.5).collect();
            let (mut l, mut r) = (x.clone(), x.clone());
            rig.run(&mut l, &mut r);
            assert_eq!(l, x);
            assert_eq!(r, x);
        }
    }

    #[test]
    fn geq_band_boosts_its_frequency() {
        for kind in [0.0, 1.0] {
            let mut rig = Rig::new("GEQ");
            rig.set("type", kind).set("1k", 12.0);
            let at = rig.sine_gain_db(1000.0, 0.05);
            assert!((at - 12.0).abs() < 0.5, "type {kind}: {at} dB at 1 kHz");
            let far = rig.sine_gain_db(100.0, 0.05);
            assert!(far.abs() < 0.5, "type {kind}: {far} dB at 100 Hz");
        }
    }

    #[test]
    fn tru_draws_what_you_set() {
        for sr in [44_100.0, 48_000.0, 96_000.0] {
            let solver = TruSolver::new(sr);
            let patterns: [[f32; BANDS]; 5] = [
                std::array::from_fn(|i| if (12..20).contains(&i) { 9.0 } else { 0.0 }),
                std::array::from_fn(|i| if i % 2 == 0 { 6.0 } else { -6.0 }),
                std::array::from_fn(|i| if i % 2 == 0 { 15.0 } else { -15.0 }),
                std::array::from_fn(|i| if i == 17 { 12.0 } else { 0.0 }),
                [15.0; BANDS],
            ];
            for faders in &patterns {
                let mut g = [0.0; BANDS];
                solver.solve(faders, &mut g);
                let resp = solver.response(&g.map(|x| x as f64));
                for (i, f) in faders.iter().enumerate() {
                    // Near Nyquist every band falls back to 0 dB, which costs the top
                    // faders (8 kHz up) a little when everything is boosted.
                    let tolerance = if i >= BANDS - 5 { 1.2 } else { 0.5 };
                    let err = (resp[2 * i] - *f as f64).abs();
                    assert!(err < tolerance, "{sr} Hz, band {i}: {:.2} dB for {f}", resp[2 * i]);
                }
            }
        }
        // STD, by contrast, lets neighbours add up.
        let mut rig = Rig::new("GEQ");
        rig.set("1k", 6.0).set("1k25", 6.0).set("800", 6.0);
        assert!(rig.sine_gain_db(1000.0, 0.05) > 8.0);
    }

    #[test]
    fn pia_mix_scales_the_bands() {
        let mut rig = Rig::new("PIA");
        rig.set("1k", 12.0);
        let full = rig.sine_gain_db(1000.0, 0.05);
        assert!((full - 12.0).abs() < 0.3, "{full}");
        let mut rig = Rig::new("PIA");
        rig.set("1k", 12.0).set("mix", 50.0);
        let half = rig.sine_gain_db(1000.0, 0.05);
        assert!((half - 6.0).abs() < 0.3, "{half}");
    }
}
