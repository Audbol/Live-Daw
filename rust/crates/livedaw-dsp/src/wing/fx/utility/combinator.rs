//! Combinator: a 5-band compressor in the style of the TC Finalizer.
//!
//! The signal is split at 120 Hz, 500 Hz, 2 kHz and 7 kHz by Linkwitz-Riley
//! crossovers (SLOPE 24 or 48 dB/oct) with all-pass compensation on the lower
//! bands, so the bands add back up to a flat (all-pass) response when nothing
//! compresses. Each band has a stereo-linked RMS compressor at THR plus its
//! own offset (n-THR), the shared RATIO, ATTACK (0..20 ms) and RELEASE (AUTO
//! REL lengthens it after sustained reduction), its makeup (n-GAIN), its
//! stereo width (n-WIDTH: -50 = mono, 0 = as is, +50 = double side) and a
//! bypass (n-BYP: the band passes untouched). SBC (spectral balance control)
//! pulls every band's reduction halfway toward the average of all bands so
//! heavy compression in one band doesn't change the tonal balance; SBC SPEED
//! sets how fast that average follows. GAIN is the output level and MIX
//! blends with the (phase-matched) dry signal. BAND SELECT is UI only.

use super::{db_gain, power_db, slow_coef, Cascade};
use crate::wing::fx::blocks::{coef, db_to_gain, BiquadCoeffs, Smoothed};
use crate::wing::fx::FxAlgo;
use crate::wing::Params;
use std::f64::consts::FRAC_1_SQRT_2;

const XOVER_HZ: [f64; 4] = [120.0, 500.0, 2000.0, 7000.0];
const BANDS: usize = 5;
/// Compensation all-passes: band `b` passes through those of crossovers `b + 1 ..`.
const COMP_OF: [(usize, usize); 6] = [(0, 1), (0, 2), (0, 3), (1, 2), (1, 3), (2, 3)];
const KNEE_DB: f32 = 6.0;

const THR_IDS: [&str; BANDS] = ["thr_1", "thr_2", "thr_3", "thr_4", "thr_5"];
const GAIN_IDS: [&str; BANDS] = ["gain_1", "gain_2", "gain_3", "gain_4", "gain_5"];
const BYP_IDS: [&str; BANDS] = ["byp_1", "byp_2", "byp_3", "byp_4", "byp_5"];
const WIDTH_IDS: [&str; BANDS] = ["width_1", "width_2", "width_3", "width_4", "width_5"];

/// The bands are split block-wise, this many samples at a time.
const CHUNK: usize = 64;

#[derive(Clone, Copy, Default)]
struct BandState {
    ms: f32,
    gr: f32,
    slow: f32,
}

pub(crate) struct Combinator {
    sr: f32,
    slope48: Option<bool>,
    lp: [Cascade<4>; 4],
    hp: [Cascade<4>; 4],
    ap: [Cascade<2>; 6],
    state: [BandState; BANDS],
    sbc_avg: f32,
    // Controls.
    thr: [f32; BANDS],
    slope: f32,
    att: f32,
    rel_ms: f32,
    auto_rel: bool,
    sbc_on: bool,
    sbc_coef: f32,
    rms_coef: f32,
    makeup: [Smoothed; BANDS],
    bypass: [Smoothed; BANDS],
    width: [Smoothed; BANDS],
    gain: Smoothed,
    mix: Smoothed,
    meter: f32,
    primed: bool,
}

impl Combinator {
    pub(crate) fn new(sr: f32) -> Self {
        let sm = |v: f32| Smoothed::new(v, 20.0, sr);
        let mut c = Self {
            sr,
            slope48: None,
            lp: std::array::from_fn(|_| Cascade::new()),
            hp: std::array::from_fn(|_| Cascade::new()),
            ap: std::array::from_fn(|_| Cascade::new()),
            state: [BandState::default(); BANDS],
            sbc_avg: 0.0,
            thr: [-20.0; BANDS],
            slope: 0.0,
            att: 1.0,
            rel_ms: 200.0,
            auto_rel: false,
            sbc_on: false,
            sbc_coef: 0.0,
            rms_coef: coef(5.0, sr),
            makeup: [sm(1.0); BANDS],
            bypass: [sm(0.0); BANDS],
            width: [sm(1.0); BANDS],
            gain: sm(1.0),
            mix: sm(1.0),
            meter: 0.0,
            primed: false,
        };
        c.set_slope(false);
        c
    }

    /// Designs the crossovers: LR4 = two 2nd-order Butterworths per side, LR8 =
    /// two 4th-order ones; the compensation all-pass is the matching
    /// Butterworth all-pass (what LP + HP sum to).
    fn set_slope(&mut self, steep: bool) {
        if self.slope48 == Some(steep) {
            return;
        }
        self.slope48 = Some(steep);
        let sr = self.sr as f64;
        let qs: &[f64] = if steep { &[0.5412, 1.3066] } else { &[FRAC_1_SQRT_2] };
        for (k, &f) in XOVER_HZ.iter().enumerate() {
            let mut lp = [BiquadCoeffs::identity(); 4];
            let mut hp = [BiquadCoeffs::identity(); 4];
            for (i, q) in qs.iter().chain(qs).enumerate() {
                lp[i] = BiquadCoeffs::low_pass(sr, f, *q);
                hp[i] = BiquadCoeffs::high_pass(sr, f, *q);
            }
            self.lp[k].set(&lp[..2 * qs.len()]);
            self.hp[k].set(&hp[..2 * qs.len()]);
        }
        for (ap, &(_, k)) in self.ap.iter_mut().zip(&COMP_OF) {
            let mut c = [BiquadCoeffs::identity(); 2];
            for (c, q) in c.iter_mut().zip(qs) {
                *c = BiquadCoeffs::all_pass_2(sr, XOVER_HZ[k], *q);
            }
            ap.set(&c[..qs.len()]);
        }
        for c in self.lp.iter_mut().chain(self.hp.iter_mut()) {
            c.reset();
        }
        for c in &mut self.ap {
            c.reset();
        }
    }

    /// Splits a chunk into the five bands, both channels at once.
    fn split(&mut self, l: &[f32], r: &[f32], bl: &mut [[f32; CHUNK]; BANDS], br: &mut [[f32; CHUNK]; BANDS]) {
        let n = l.len();
        let (mut rl, mut rr) = ([0.0f32; CHUNK], [0.0f32; CHUNK]);
        rl[..n].copy_from_slice(l);
        rr[..n].copy_from_slice(r);
        for k in 0..4 {
            bl[k][..n].copy_from_slice(&rl[..n]);
            br[k][..n].copy_from_slice(&rr[..n]);
            self.lp[k].process(&mut bl[k][..n], &mut br[k][..n]);
            self.hp[k].process(&mut rl[..n], &mut rr[..n]);
        }
        bl[4][..n].copy_from_slice(&rl[..n]);
        br[4][..n].copy_from_slice(&rr[..n]);
        for (ap, &(b, _)) in self.ap.iter_mut().zip(&COMP_OF) {
            ap.process(&mut bl[b][..n], &mut br[b][..n]);
        }
    }

    /// One chunk (at most `CHUNK` samples); returns the deepest reduction.
    #[allow(clippy::needless_range_loop)] // the band buffers are indexed per sample
    fn process_chunk(&mut self, left: &mut [f32], right: &mut [f32]) -> f32 {
        let n = left.len().min(right.len());
        let mut bl = [[0.0f32; CHUNK]; BANDS];
        let mut br = [[0.0f32; CHUNK]; BANDS];
        self.split(&left[..n], &right[..n], &mut bl, &mut br);

        let rel_fixed = coef(self.rel_ms, self.sr);
        let slow = coef(1000.0, self.sr);
        let mut deepest = 0.0f32;
        for i in 0..n {
            // Gain computers.
            let mut gr = [0.0f32; BANDS];
            for (b, g) in gr.iter_mut().enumerate() {
                let s = &mut self.state[b];
                let x = 0.5 * (bl[b][i] * bl[b][i] + br[b][i] * br[b][i]);
                s.ms += (x - s.ms) * self.rms_coef;
                let lvl = power_db(s.ms) + 3.0;
                let over = lvl - self.thr[b];
                let target = if over >= 0.5 * KNEE_DB {
                    self.slope * over
                } else if over > -0.5 * KNEE_DB {
                    self.slope * (over + 0.5 * KNEE_DB).powi(2) / (2.0 * KNEE_DB)
                } else {
                    0.0
                };
                let rel = if self.auto_rel {
                    s.slow += (-s.gr - s.slow) * slow;
                    slow_coef((self.rel_ms * (1.0 + s.slow.max(0.0) / 3.0)).min(self.rel_ms * 5.0), self.sr)
                } else {
                    rel_fixed
                };
                s.gr += (target - s.gr) * if target < s.gr { self.att } else { rel };
                *g = s.gr;
            }
            if self.sbc_on {
                let avg = gr.iter().sum::<f32>() / BANDS as f32;
                self.sbc_avg += (avg - self.sbc_avg) * self.sbc_coef;
                for g in &mut gr {
                    *g += 0.5 * (self.sbc_avg - *g);
                }
            }

            let (mut wl, mut wr, mut dl, mut dr) = (0.0f32, 0.0f32, 0.0f32, 0.0f32);
            for b in 0..BANDS {
                let (xl, xr) = (bl[b][i], br[b][i]);
                let byp = self.bypass[b].tick();
                let width = self.width[b].tick();
                let g = db_gain(gr[b]) * self.makeup[b].tick();
                deepest = deepest.min(gr[b] * (1.0 - byp));
                let mid = 0.5 * (xl + xr) * g;
                let side = 0.5 * (xl - xr) * g * width;
                wl += xl + (1.0 - byp) * (mid + side - xl);
                wr += xr + (1.0 - byp) * (mid - side - xr);
                dl += xl;
                dr += xr;
            }
            let g = self.gain.tick();
            let mix = self.mix.tick();
            left[i] = dl + mix * (wl * g - dl);
            right[i] = dr + mix * (wr * g - dr);
        }
        deepest
    }

    fn smoothers(&mut self) -> impl Iterator<Item = &mut Smoothed> {
        self.makeup.iter_mut().chain(self.bypass.iter_mut()).chain(self.width.iter_mut()).chain([&mut self.gain, &mut self.mix])
    }
}

impl FxAlgo for Combinator {
    fn set_params(&mut self, p: &Params) {
        self.set_slope(p.choice("slope") == 1);
        let thr = p.f("thr");
        let gain = p.f("gain");
        for b in 0..BANDS {
            self.thr[b] = thr + p.f(THR_IDS[b]);
            self.makeup[b].target = db_to_gain(p.f(GAIN_IDS[b]));
            self.bypass[b].target = if p.on(BYP_IDS[b]) { 1.0 } else { 0.0 };
            self.width[b].target = 1.0 + p.f(WIDTH_IDS[b]) / 50.0;
        }
        let ratio = p.f("ratio");
        self.slope = if ratio >= 99.0 { -1.0 } else { 1.0 / ratio.max(1.0) - 1.0 };
        self.att = coef(p.f("att").max(0.05), self.sr);
        self.rel_ms = p.f("rel");
        self.auto_rel = p.on("arel");
        self.sbc_on = p.on("sbcon");
        self.sbc_coef = coef(2000.0 / p.f("sbc").max(1.0), self.sr);
        self.gain.target = db_to_gain(gain);
        self.mix.target = p.f("mix") / 100.0;
        if !self.primed {
            for s in self.smoothers() {
                s.snap(s.target);
            }
            self.primed = true;
        }
    }

    fn process(&mut self, left: &mut [f32], right: &mut [f32]) {
        let mut deepest = 0.0f32;
        for (l, r) in left.chunks_mut(CHUNK).zip(right.chunks_mut(CHUNK)) {
            deepest = deepest.min(self.process_chunk(l, r));
        }
        self.meter = deepest;
    }

    fn reset(&mut self) {
        for c in self.lp.iter_mut().chain(self.hp.iter_mut()) {
            c.reset();
        }
        for c in &mut self.ap {
            c.reset();
        }
        self.state = [BandState::default(); BANDS];
        self.sbc_avg = 0.0;
        self.meter = 0.0;
        for s in self.smoothers() {
            s.snap(s.target);
        }
    }

    fn meter(&self) -> f32 {
        self.meter
    }
}

#[cfg(test)]
mod tests {
    use super::super::tests::Rig;

    #[test]
    fn bands_sum_flat_without_compression() {
        for slope in [0.0, 1.0] {
            for freq in [40.0, 120.0, 300.0, 500.0, 1000.0, 2000.0, 4000.0, 7000.0, 12_000.0] {
                let mut rig = Rig::new("C5-CMB");
                rig.set("thr", 0.0).set("slope", slope);
                let g = rig.sine_gain_db(freq, 0.01);
                assert!(g.abs() < 0.5, "slope {slope}: {g} dB at {freq} Hz");
            }
        }
    }

    #[test]
    fn compresses_loud_bands() {
        let mut rig = Rig::new("C5-CMB");
        rig.set("ratio", 9.0); // 4:1
        let g = rig.sine_gain_db(1000.0, 0.7);
        assert!(g < -6.0, "{g}");
        assert!(rig.dev.meter() < -6.0);
        // Band 3 bypassed: the same tone passes.
        let mut rig = Rig::new("C5-CMB");
        rig.set("ratio", 9.0).set("byp_3", 1.0);
        let g = rig.sine_gain_db(1000.0, 0.7);
        assert!(g.abs() < 0.5, "{g}");
    }
}
