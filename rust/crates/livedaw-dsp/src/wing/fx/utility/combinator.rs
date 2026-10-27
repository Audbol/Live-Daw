//! C5-Combinator: a 5-band compressor with variable crossover slope and
//! minimal phase shift, inspired by the Behringer Combinator MDX-8000 (WING
//! manual 11.2).
//!
//! The signal is split by Linkwitz-Riley crossovers (SLOPE 24 or 48 dB/oct)
//! with all-pass compensation on the lower bands, so the bands add back up to
//! a flat (all-pass) response when nothing compresses. The crossovers sit at
//! 120 Hz, 500 Hz, 2 kHz and 7 kHz; each band's WIDTH (-50..50) adjusts the
//! frequency range it covers by moving both of its edges by up to half an
//! octave (out for positive, in for negative), which also moves the
//! neighbours' edges; adjacent crossovers stay at least a third of an octave
//! apart.
//!
//! Each band has a stereo-linked RMS compressor: the global THR plus the
//! band's offset (n-THR: -10 dB on a -10 dB THR gives -20 dB), the shared
//! RATIO, ATTACK (a relative 0..20 scale, 1..100 ms) and RELEASE (AUTO
//! lengthens it after sustained reduction), then the band's makeup (n-GAIN).
//! n-BYP disables the band's compression. SBC (spectrum balance control)
//! adjusts every band's makeup automatically to keep the bands' levels
//! relatively uniform: a band compressed more than the average gets back half
//! the difference, one compressed less gives half of it up, so heavy
//! compression in one band doesn't tilt the spectrum; SBC SPEED sets how fast
//! that makeup follows. GAIN is the output level and MIX blends with the
//! (phase-matched) dry signal. BAND SELECT and the band solo are UI only.
//!
//! Readouts (the screen's band display): 0-4 each band's gain reduction (dB,
//! <= 0; 0 while the band is bypassed), 5-9 each band's SBC makeup (dB,
//! signed), 10-39 the input's spectrum in third octaves from 25 Hz to 20 kHz
//! ([`spectrum_hz`](crate::wing::fx::meters::spectrum_hz); linear peaks), 40-44 each band's output level (linear
//! peak).

use super::{db_gain, power_db, slow_coef, Cascade, Glide};
use crate::wing::fx::blocks::{coef, db_to_gain, BiquadCoeffs, Smoothed};
use crate::wing::fx::meters::{PeakMeter, Spectrum, SPECTRUM_BANDS};
use crate::wing::fx::{FxAlgo, FX_READOUTS};
use crate::wing::Params;
use std::f64::consts::FRAC_1_SQRT_2;

const XOVER_HZ: [f32; 4] = [120.0, 500.0, 2000.0, 7000.0];
const BANDS: usize = 5;
/// Compensation all-passes: band `b` passes through those of crossovers `b + 1 ..`.
const COMP_OF: [(usize, usize); 6] = [(0, 1), (0, 2), (0, 3), (1, 2), (1, 3), (2, 3)];
const KNEE_DB: f32 = 6.0;
/// Octaves each band edge moves at WIDTH +-50.
const EDGE_OCT: f32 = 0.5;
/// Closest two crossovers may get, in octaves.
const MIN_GAP_OCT: f32 = 1.0 / 3.0;

const THR_IDS: [&str; BANDS] = ["thr_1", "thr_2", "thr_3", "thr_4", "thr_5"];
const GAIN_IDS: [&str; BANDS] = ["gain_1", "gain_2", "gain_3", "gain_4", "gain_5"];
const BYP_IDS: [&str; BANDS] = ["byp_1", "byp_2", "byp_3", "byp_4", "byp_5"];
const WIDTH_IDS: [&str; BANDS] = ["width_1", "width_2", "width_3", "width_4", "width_5"];

/// The bands are split block-wise, this many samples at a time.
const CHUNK: usize = 64;

/// Crossover frequencies for the bands' WIDTH settings (-50..50).
fn crossovers(width: &[f32; BANDS]) -> [f32; 4] {
    let mut f: [f32; 4] = std::array::from_fn(|k| XOVER_HZ[k] * 2f32.powf(EDGE_OCT * (width[k] - width[k + 1]) / 50.0));
    let gap = 2f32.powf(MIN_GAP_OCT);
    f[0] = f[0].clamp(40.0, 4000.0);
    for k in 1..4 {
        f[k] = f[k].max(f[k - 1] * gap);
    }
    f[3] = f[3].min(14_000.0);
    for k in (0..3).rev() {
        f[k] = f[k].min(f[k + 1] / gap);
    }
    f
}

#[derive(Clone, Copy, Default)]
struct BandState {
    ms: f32,
    gr: f32,
    slow: f32,
    /// SBC makeup in dB.
    sbc: f32,
}

pub(crate) struct Combinator {
    sr: f32,
    slope48: bool,
    lp: [Cascade<4>; 4],
    hp: [Cascade<4>; 4],
    ap: [Cascade<2>; 6],
    /// ln(crossover Hz).
    xover: Glide<4>,
    state: [BandState; BANDS],
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
    gain: Smoothed,
    mix: Smoothed,
    meter: f32,
    /// The screen's readouts: band reductions and SBC makeups at the end of
    /// the last chunk, the input's spectrum and the bands' output levels.
    shown_gr: [f32; BANDS],
    shown_sbc: [f32; BANDS],
    spectrum: Spectrum,
    band_level: [PeakMeter; BANDS],
    primed: bool,
}

/// Where the readouts start (see the module's documentation).
const RD_SBC: usize = BANDS;
const RD_SPECTRUM: usize = 2 * BANDS;
const RD_LEVEL: usize = RD_SPECTRUM + SPECTRUM_BANDS;

impl Combinator {
    pub(crate) fn new(sr: f32) -> Self {
        let sm = |v: f32| Smoothed::new(v, 20.0, sr);
        let mut c = Self {
            sr,
            slope48: false,
            lp: std::array::from_fn(|_| Cascade::new()),
            hp: std::array::from_fn(|_| Cascade::new()),
            ap: std::array::from_fn(|_| Cascade::new()),
            xover: Glide::new(0.0),
            state: [BandState::default(); BANDS],
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
            gain: sm(1.0),
            mix: sm(1.0),
            meter: 0.0,
            shown_gr: [0.0; BANDS],
            shown_sbc: [0.0; BANDS],
            spectrum: Spectrum::new(sr),
            band_level: [PeakMeter::new(sr); BANDS],
            primed: false,
        };
        c.xover.tgt = XOVER_HZ.map(f32::ln);
        c.xover.snap();
        c.design();
        c
    }

    /// Designs the crossovers at the glided frequencies: LR4 = two 2nd-order
    /// Butterworths per side, LR8 = two 4th-order ones; the compensation
    /// all-pass is the matching Butterworth all-pass (what LP + HP sum to).
    fn design(&mut self) {
        let sr = self.sr as f64;
        let qs: &[f64] = if self.slope48 { &[0.5412, 1.3066] } else { &[FRAC_1_SQRT_2] };
        let f = self.xover.cur.map(|v| v.exp() as f64);
        for ((lp_k, hp_k), &fk) in self.lp.iter_mut().zip(self.hp.iter_mut()).zip(&f) {
            let mut lp = [BiquadCoeffs::identity(); 4];
            let mut hp = [BiquadCoeffs::identity(); 4];
            for (i, q) in qs.iter().chain(qs).enumerate() {
                lp[i] = BiquadCoeffs::low_pass(sr, fk, *q);
                hp[i] = BiquadCoeffs::high_pass(sr, fk, *q);
            }
            lp_k.set(&lp[..2 * qs.len()]);
            hp_k.set(&hp[..2 * qs.len()]);
        }
        for (ap, &(_, k)) in self.ap.iter_mut().zip(&COMP_OF) {
            let mut c = [BiquadCoeffs::identity(); 2];
            for (c, q) in c.iter_mut().zip(qs) {
                *c = BiquadCoeffs::all_pass_2(sr, f[k], *q);
            }
            ap.set(&c[..qs.len()]);
        }
    }

    fn reset_filters(&mut self) {
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
        if self.xover.step() != 0 {
            self.design();
        }
        let mut bl = [[0.0f32; CHUNK]; BANDS];
        let mut br = [[0.0f32; CHUNK]; BANDS];
        self.split(&left[..n], &right[..n], &mut bl, &mut br);

        let rel_fixed = coef(self.rel_ms, self.sr);
        let slow = coef(1000.0, self.sr);
        let mut deepest = 0.0f32;
        for i in 0..n {
            // Gain computers (a bypassed band doesn't compress).
            let mut gr = [0.0f32; BANDS];
            let mut active = [0.0f32; BANDS];
            for (b, (g, a)) in gr.iter_mut().zip(active.iter_mut()).enumerate() {
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
                *a = 1.0 - self.bypass[b].tick();
                *g = s.gr * *a;
            }
            // SBC: makeup toward the average reduction of the compressing bands.
            let weight: f32 = active.iter().sum();
            let avg = if weight > 1e-3 { gr.iter().sum::<f32>() / weight } else { 0.0 };
            for (b, (g, a)) in gr.iter_mut().zip(&active).enumerate() {
                let s = &mut self.state[b];
                let want = if self.sbc_on { 0.5 * (avg - *g) * a } else { 0.0 };
                s.sbc += (want - s.sbc) * self.sbc_coef;
                deepest = deepest.min(*g);
                self.shown_gr[b] = *g;
                self.shown_sbc[b] = s.sbc;
                *g += s.sbc;
            }

            let (mut wl, mut wr, mut dl, mut dr) = (0.0f32, 0.0f32, 0.0f32, 0.0f32);
            for b in 0..BANDS {
                let (xl, xr) = (bl[b][i], br[b][i]);
                let g = db_gain(gr[b]) * self.makeup[b].tick();
                let (yl, yr) = (xl * g, xr * g);
                self.band_level[b].tick(yl.abs().max(yr.abs()));
                wl += yl;
                wr += yr;
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
        self.makeup.iter_mut().chain(self.bypass.iter_mut()).chain([&mut self.gain, &mut self.mix])
    }
}

impl FxAlgo for Combinator {
    fn set_params(&mut self, p: &Params) {
        let thr = p.f("thr");
        let mut width = [0.0f32; BANDS];
        for b in 0..BANDS {
            self.thr[b] = thr + p.f(THR_IDS[b]);
            self.makeup[b].target = db_to_gain(p.f(GAIN_IDS[b]));
            self.bypass[b].target = if p.on(BYP_IDS[b]) { 1.0 } else { 0.0 };
            width[b] = p.f(WIDTH_IDS[b]);
        }
        self.xover.tgt = crossovers(&width).map(f32::ln);
        let ratio = p.f("ratio");
        self.slope = if ratio >= 99.0 { -1.0 } else { 1.0 / ratio.max(1.0) - 1.0 };
        // ATTACK is relative (0..20): 1 ms .. 100 ms.
        self.att = coef(100f32.powf(p.f("att") / 20.0), self.sr);
        self.rel_ms = p.f("rel");
        self.auto_rel = p.on("arel");
        self.sbc_on = p.on("sbcon");
        // SBC SPEED 1..10: makeup time constant 2 s .. 200 ms.
        self.sbc_coef = coef(2000.0 / p.f("sbc").max(1.0), self.sr);
        self.gain.target = db_to_gain(p.f("gain"));
        self.mix.target = p.f("mix") / 100.0;
        let steep = p.choice("slope") == 1;
        if steep != self.slope48 || !self.primed {
            self.slope48 = steep;
            self.xover.snap();
            self.design();
            self.reset_filters();
        }
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
            let mut mono = [0.0f32; CHUNK];
            for ((m, a), b) in mono.iter_mut().zip(l.iter()).zip(r.iter()) {
                *m = 0.5 * (a + b);
            }
            self.spectrum.process(&mono[..l.len()]);
            deepest = deepest.min(self.process_chunk(l, r));
        }
        self.meter = deepest;
    }

    fn reset(&mut self) {
        self.xover.snap();
        self.design();
        self.reset_filters();
        self.state = [BandState::default(); BANDS];
        self.meter = 0.0;
        self.shown_gr = [0.0; BANDS];
        self.shown_sbc = [0.0; BANDS];
        self.spectrum.reset();
        self.band_level.iter_mut().for_each(PeakMeter::reset);
        for s in self.smoothers() {
            s.snap(s.target);
        }
    }

    fn meter(&self) -> f32 {
        self.meter
    }

    fn readouts(&self, out: &mut [f32; FX_READOUTS]) {
        out[..BANDS].copy_from_slice(&self.shown_gr);
        out[RD_SBC..RD_SBC + BANDS].copy_from_slice(&self.shown_sbc);
        self.spectrum.levels(&mut out[RD_SPECTRUM..RD_SPECTRUM + SPECTRUM_BANDS]);
        for (o, m) in out[RD_LEVEL..RD_LEVEL + BANDS].iter_mut().zip(&self.band_level) {
            *o = m.level();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::super::tests::Rig;
    use super::*;

    #[test]
    fn bands_sum_flat_without_compression() {
        for (slope, widths) in [(0.0, [0.0; 5]), (1.0, [0.0; 5]), (0.0, [50.0, -50.0, 20.0, 0.0, -30.0])] {
            for freq in [40.0, 120.0, 300.0, 500.0, 1000.0, 2000.0, 4000.0, 7000.0, 12_000.0] {
                let mut rig = Rig::new("C5-CMB");
                rig.set("thr", 0.0).set("slope", slope);
                for (id, w) in WIDTH_IDS.iter().zip(widths) {
                    rig.set(id, w);
                }
                let g = rig.sine_gain_db(freq, 0.01);
                assert!(g.abs() < 0.5, "slope {slope}, widths {widths:?}: {g} dB at {freq} Hz");
            }
        }
    }

    #[test]
    fn width_moves_the_band_edges() {
        assert_eq!(crossovers(&[0.0; 5]), XOVER_HZ);
        // Band 3 wider: its edges move out by half an octave each.
        let f = crossovers(&[0.0, 0.0, 50.0, 0.0, 0.0]);
        assert!((f[1] - 500.0 / 2f32.sqrt()).abs() < 1.0 && (f[2] - 2000.0 * 2f32.sqrt()).abs() < 1.0, "{f:?}");
        // Squeezed bands keep a third of an octave between crossovers.
        let f = crossovers(&[50.0, -50.0, 50.0, -50.0, 50.0]);
        for k in 1..4 {
            assert!(f[k] / f[k - 1] >= 2f32.powf(MIN_GAP_OCT) * 0.999, "{f:?}");
        }
    }

    #[test]
    fn compresses_loud_bands() {
        let mut rig = Rig::new("C5-CMB");
        rig.set("ratio", 9.0).set("thr", -20.0).set("sbcon", 0.0); // 4:1
        let g = rig.sine_gain_db(1000.0, 0.7);
        assert!(g < -6.0, "{g}");
        assert!(rig.dev.meter() < -6.0);
        // Band 3's compression disabled: the same tone passes (its makeup still applies).
        let mut rig = Rig::new("C5-CMB");
        rig.set("ratio", 9.0).set("thr", -20.0).set("sbcon", 0.0).set("byp_3", 1.0).set("gain_3", 2.0);
        let g = rig.sine_gain_db(1000.0, 0.7);
        assert!((g - 2.0).abs() < 0.5, "{g}");
    }

    fn readouts(rig: &Rig) -> [f32; FX_READOUTS] {
        let mut r = [0.0; FX_READOUTS];
        rig.dev.readouts(&mut r);
        r
    }

    #[test]
    fn readouts_show_the_band_that_works() {
        use crate::wing::fx::meters::spectrum_hz;
        // A loud 1 kHz tone: band 3 compresses, the others don't; the spectrum
        // and the band levels show it at 1 kHz, in band 3.
        for (freq, band) in [(60.0f32, 0usize), (1000.0, 2), (10_000.0, 4)] {
            let mut rig = Rig::new("C5-CMB");
            rig.set("ratio", 9.0).set("thr", -20.0).set("sbcon", 0.0);
            rig.sine_gain_db(freq, 0.7);
            let r = readouts(&rig);
            for (b, gr) in r[..BANDS].iter().enumerate() {
                if b == band {
                    assert!(*gr < -6.0, "{freq} Hz: band {b} GR {gr}");
                } else {
                    assert!(*gr > -0.5, "{freq} Hz: band {b} GR {gr}");
                }
            }
            let spec = &r[RD_SPECTRUM..RD_SPECTRUM + SPECTRUM_BANDS];
            let top = (0..SPECTRUM_BANDS).max_by(|a, b| spec[*a].total_cmp(&spec[*b])).unwrap();
            assert!((spectrum_hz(top) / freq).log2().abs() < 0.2, "{freq} Hz: spectrum peaks at {}", spectrum_hz(top));
            let lv = &r[RD_LEVEL..RD_LEVEL + BANDS];
            let loudest = (0..BANDS).max_by(|a, b| lv[*a].total_cmp(&lv[*b])).unwrap();
            assert_eq!(loudest, band, "{freq} Hz: {lv:?}");
        }
        // Silence: the readouts fall back to idle (in RELEASE; AUTO lengthens it).
        let mut rig = Rig::new("C5-CMB");
        rig.set("ratio", 9.0).set("thr", -20.0).set("arel", 0.0);
        rig.sine_gain_db(1000.0, 0.7);
        let (mut l, mut r) = (vec![0.0; 96_000], vec![0.0; 96_000]);
        rig.run(&mut l, &mut r);
        let ro = readouts(&rig);
        assert!(ro[..BANDS].iter().all(|g| *g > -0.5), "{:?}", &ro[..BANDS]);
        assert!(ro[RD_SPECTRUM..].iter().all(|v| *v < 1e-3));
        // A bypassed band shows no reduction.
        let mut rig = Rig::new("C5-CMB");
        rig.set("ratio", 9.0).set("thr", -20.0).set("byp_3", 1.0);
        rig.sine_gain_db(1000.0, 0.7);
        assert!(readouts(&rig)[2] > -0.1);
    }

    #[test]
    fn threshold_ratio_and_band_offsets_set_the_reduction() {
        let gr = |set: &[(&str, f32)]| {
            let mut rig = Rig::new("C5-CMB");
            rig.set("sbcon", 0.0).set("ratio", 5.0).set("thr", -20.0);
            for (id, v) in set {
                rig.set(id, *v);
            }
            rig.sine_gain_db(1000.0, 0.5);
            readouts(&rig)[2]
        };
        let base = gr(&[]);
        assert!(base < -2.0, "{base}");
        assert!(gr(&[("thr", -30.0)]) < base - 3.0);
        assert!(gr(&[("ratio", 9.0)]) < base - 1.0);
        // Band 3's own offset deepens it; another band's doesn't.
        assert!(gr(&[("thr_3", -10.0)]) < base - 3.0);
        assert!((gr(&[("thr_1", -10.0)]) - base).abs() < 0.3);
        // Below the threshold nothing happens.
        assert!(gr(&[("thr", 0.0)]) > -0.2);
    }

    #[test]
    fn attack_gain_and_mix() {
        // Attack 0 grabs a burst within a few ms, attack 20 lets it through.
        let early = |att: f32| {
            let mut rig = Rig::new("C5-CMB");
            rig.set("sbcon", 0.0).set("ratio", 9.0).set("thr", -30.0).set("att", att);
            let x: Vec<f32> = (0..240).map(|i| (std::f32::consts::TAU * 1000.0 * i as f32 / 48_000.0).sin() * 0.7).collect();
            let (mut l, mut r) = (x.clone(), x);
            rig.run(&mut l, &mut r);
            readouts(&rig)[2]
        };
        assert!(early(0.0) < early(20.0) - 3.0, "{} vs {}", early(0.0), early(20.0));
        // GAIN is the output level; MIX 0 is the dry signal however hard it compresses.
        let mut rig = Rig::new("C5-CMB");
        rig.set("thr", 0.0).set("gain", 6.0);
        assert!((rig.sine_gain_db(500.0, 0.05) - 6.0).abs() < 0.5);
        let mut rig = Rig::new("C5-CMB");
        rig.set("thr", -40.0).set("ratio", 9.0).set("mix", 0.0);
        assert!(rig.sine_gain_db(1000.0, 0.5).abs() < 0.3);
    }

    #[test]
    fn sbc_evens_out_the_reduction() {
        // A loud tone in band 3 only: SBC gives half its reduction back.
        let mut plain = Rig::new("C5-CMB");
        plain.set("ratio", 9.0).set("thr", -20.0).set("sbcon", 0.0);
        let without = plain.sine_gain_db(1000.0, 0.7);
        let mut sbc = Rig::new("C5-CMB");
        sbc.set("ratio", 9.0).set("thr", -20.0).set("sbcon", 1.0).set("sbc", 10.0);
        let with = sbc.sine_gain_db(1000.0, 0.7);
        assert!(with > without + 2.0, "{with} vs {without}");
    }
}
