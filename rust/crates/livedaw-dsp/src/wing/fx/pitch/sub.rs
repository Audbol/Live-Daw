//! The sub-bass devices: sub octaver (SUB), sub monster (SUB-M) and psycho
//! bass (P-BASS).

use super::super::blocks::{db_to_gain, fast_tanh, Biquad, BiquadCoeffs, DcBlock, Envelope, Smoothed};
use super::super::{FxAlgo, FxContext};
use super::{add_mix, chain, fader_gain, SMOOTH_MS};
use crate::wing::Params;

const Q_BUTTER: f64 = std::f64::consts::FRAC_1_SQRT_2;

fn set_lp(f: &mut Biquad, sr: f32, hz: f32, q: f64) {
    f.set_coefficients(BiquadCoeffs::low_pass(sr as f64, hz as f64, q));
}

fn set_hp(f: &mut Biquad, sr: f32, hz: f32, q: f64) {
    f.set_coefficients(BiquadCoeffs::high_pass(sr as f64, hz as f64, q));
}

fn set_bp(f: &mut Biquad, sr: f32, hz: f32, q: f64) {
    f.set_coefficients(BiquadCoeffs::band_pass(sr as f64, hz as f64, q));
}

/// A comparator with hysteresis feeding a divide-by-two flip-flop: the
/// octave divider of analog octavers. `tick` returns the flip-flop output
/// (+-1) and, on the second output, the divide-by-four stage.
#[derive(Clone, Copy, Default)]
struct Divider {
    high: bool,
    ff1: bool,
    ff2: bool,
}

impl Divider {
    #[inline]
    fn tick(&mut self, x: f32, hysteresis: f32) -> (f32, f32) {
        if !self.high && x > hysteresis {
            self.high = true;
            self.ff1 = !self.ff1;
            if self.ff1 {
                self.ff2 = !self.ff2;
            }
        } else if self.high && x < -hysteresis {
            self.high = false;
        }
        (if self.ff1 { 1.0 } else { -1.0 }, if self.ff2 { 1.0 } else { -1.0 })
    }
}

// ---- SUB: Sub Octaver -----------------------------------------------------------------

/// Boss OC-2 style octaver. The (mono) input is band-limited to the RANGE the
/// tracker follows (LOW for bass, MID for baritone/low guitar, HIGH for
/// guitar), squared up by a comparator with hysteresis and divided by
/// flip-flops into one (OCTAVE 1) and two (OCTAVE 2) octaves down. The square
/// waves are shaped by the tracked signal's envelope and smoothed by a
/// low-pass, then added to the direct signal, which stays at unity like the
/// pedal's DIRECT LEVEL at noon.
pub(super) struct SubOctaver {
    sr: f32,
    range: usize,
    hp: Biquad,
    lp: [Biquad; 2],
    env: Envelope,
    div: Divider,
    post: Biquad,
    dc: DcBlock,
    oct1: Smoothed,
    oct2: Smoothed,
}

/// Tracker band (high-pass, low-pass) and the octave smoothing low-pass, Hz.
const SUB_RANGES: [(f32, f32, f32); 3] = [(25.0, 180.0, 160.0), (40.0, 340.0, 300.0), (70.0, 700.0, 600.0)];

impl SubOctaver {
    pub(super) fn new(ctx: FxContext) -> Self {
        let sr = ctx.sample_rate;
        let mut env = Envelope::default();
        env.set(1.0, 0.0, 40.0, sr);
        let mut s = Self {
            sr,
            range: usize::MAX,
            hp: Biquad::default(),
            lp: [Biquad::default(), Biquad::default()],
            env,
            div: Divider::default(),
            post: Biquad::default(),
            dc: DcBlock::new(sr),
            oct1: Smoothed::new(0.0, SMOOTH_MS, sr),
            oct2: Smoothed::new(0.0, SMOOTH_MS, sr),
        };
        s.set_range(1);
        s
    }

    fn set_range(&mut self, range: usize) {
        let range = range.min(SUB_RANGES.len() - 1);
        if range == self.range {
            return;
        }
        self.range = range;
        let (hp, lp, post) = SUB_RANGES[range];
        set_hp(&mut self.hp, self.sr, hp, Q_BUTTER);
        set_lp(&mut self.lp[0], self.sr, lp, 0.541);
        set_lp(&mut self.lp[1], self.sr, lp, 1.307);
        set_lp(&mut self.post, self.sr, post, Q_BUTTER);
    }
}

impl FxAlgo for SubOctaver {
    fn set_params(&mut self, p: &Params) {
        self.set_range(p.choice("rng"));
        self.oct1.target = p.f("oct1") * 0.01;
        self.oct2.target = p.f("oct2") * 0.01;
    }

    fn process(&mut self, left: &mut [f32], right: &mut [f32]) {
        for (l, r) in left.iter_mut().zip(right.iter_mut()) {
            let m = 0.5 * (*l + *r);
            let x = chain(&mut self.lp, self.hp.tick(m, 0), 0);
            let env = self.env.tick(x.abs());
            let (q1, q2) = self.div.tick(x, 0.3 * env + 1e-6);
            let (g1, g2) = (self.oct1.tick(), self.oct2.tick());
            let sub = self.post.tick((q1 * g1 + q2 * g2) * env * 1.2, 0);
            let sub = self.dc.tick(sub);
            *l += sub;
            *r += sub;
        }
    }

    fn reset(&mut self) {
        self.hp.reset();
        for f in &mut self.lp {
            f.reset();
        }
        self.post.reset();
        self.env.reset();
        self.div = Divider::default();
        self.dc.reset();
        self.oct1.snap(self.oct1.target);
        self.oct2.snap(self.oct2.target);
    }
}

// ---- SUB-M: Sub Monster ---------------------------------------------------------------

/// Spacing of the sub monster's bands (a fifth; the FREQ range 45-67.5 Hz
/// spans exactly one step).
const SUB_M_RATIO: f32 = 1.5;

/// One subharmonic band: analysis band-pass an octave above the band, an
/// octave divider, an envelope and a two-stage band-pass that turns the
/// divided square back into a round sub tone.
#[derive(Clone, Default)]
struct SubBand {
    analysis: Biquad,
    env: Envelope,
    div: Divider,
    shape: [Biquad; 2],
    level: Smoothed,
}

/// The Midas SUB MONSTER subharmonic synthesizer. Five bands a fifth apart
/// with BAND 3 on FREQ (FREQ / 2.25, / 1.5, x1, x1.5, x2.25: 24-152 Hz at
/// the default 55 Hz, matching the plug-in's band readouts), each
/// synthesising a tone at its frequency from the input's content an octave
/// above it (an octave divider following that content's level, smoothed into
/// a round tone by band-passes at the band frequency). The BAND faders set
/// each tone's level; MIX balances the dry signal and the generated sub (both
/// at full level at 50 %).
pub(super) struct SubMonster {
    sr: f32,
    freq: f32,
    freq_target: f32,
    bands: [SubBand; 5],
    dc: DcBlock,
    dry: Smoothed,
    wet: Smoothed,
}

impl SubMonster {
    pub(super) fn new(ctx: FxContext) -> Self {
        let sr = ctx.sample_rate;
        let band = || {
            let mut b = SubBand::default();
            b.env.set(3.0, 0.0, 80.0, sr);
            b.level = Smoothed::new(0.0, SMOOTH_MS, sr);
            b
        };
        let mut s = Self {
            sr,
            freq: 55.0,
            freq_target: 55.0,
            bands: [band(), band(), band(), band(), band()],
            dc: DcBlock::new(sr),
            dry: Smoothed::new(1.0, SMOOTH_MS, sr),
            wet: Smoothed::new(1.0, SMOOTH_MS, sr),
        };
        s.tune();
        s
    }

    fn tune(&mut self) {
        for (k, b) in self.bands.iter_mut().enumerate() {
            let fc = self.freq * SUB_M_RATIO.powi(k as i32 - 2);
            set_bp(&mut b.analysis, self.sr, 2.0 * fc, 1.6);
            set_bp(&mut b.shape[0], self.sr, fc, 1.4);
            set_bp(&mut b.shape[1], self.sr, fc, 1.4);
        }
    }
}

impl FxAlgo for SubMonster {
    fn set_params(&mut self, p: &Params) {
        let (dry, wet) = add_mix(p.f("mix"));
        self.dry.target = dry;
        self.wet.target = wet;
        self.freq_target = p.f("freq").clamp(20.0, 200.0);
        for (b, id) in self.bands.iter_mut().zip(["bd1", "bd2", "bd3", "bd4", "bd5"]) {
            b.level.target = p.f(id) * 0.01;
        }
    }

    fn process(&mut self, left: &mut [f32], right: &mut [f32]) {
        if self.freq != self.freq_target {
            // Glide over a few blocks (filters retuned per block).
            let step = (self.freq_target - self.freq) * 0.3;
            self.freq = if step.abs() < 0.05 { self.freq_target } else { self.freq + step };
            self.tune();
        }
        for (l, r) in left.iter_mut().zip(right.iter_mut()) {
            let m = 0.5 * (*l + *r);
            let mut sub = 0.0;
            for b in &mut self.bands {
                let level = b.level.tick();
                let x = b.analysis.tick(m, 0);
                let env = b.env.tick(x.abs());
                let (q, _) = b.div.tick(x, 0.2 * env + 1e-6);
                // Keep the band filters running even when muted, so a band
                // fades in without a transient.
                let y = chain(&mut b.shape, q * env, 0);
                sub += y * level;
            }
            let sub = self.dc.tick(sub * 1.4);
            let (dry, wet) = (self.dry.tick(), self.wet.tick());
            *l = *l * dry + sub * wet;
            *r = *r * dry + sub * wet;
        }
    }

    fn reset(&mut self) {
        for b in &mut self.bands {
            b.analysis.reset();
            b.env.reset();
            b.div = Divider::default();
            for f in &mut b.shape {
                f.reset();
            }
            b.level.snap(b.level.target);
        }
        self.dc.reset();
        self.dry.snap(self.dry.target);
        self.wet.snap(self.wet.target);
        if self.freq != self.freq_target {
            self.freq = self.freq_target;
            self.tune();
        }
    }
}

// ---- P-BASS: Psycho Bass --------------------------------------------------------------

/// Psychoacoustic bass enhancer (MaxxBass style). A Linkwitz-Riley crossover
/// at X/O FREQ splits the signal; the low band's harmonics (full-wave
/// rectified for the even ones, soft-clipped for the odd ones, normalised by
/// the band's envelope so they track its level linearly) are band-passed
/// above the crossover and added at INTENSITY, so the ear infers the
/// fundamental even on speakers that cannot reproduce it. BASS GAIN
/// attenuates the original low band to make up for the added loudness; SOLO
/// outputs only the band below X/O FREQ (the material the harmonics are made
/// from), as the console's manual describes.
pub(super) struct PsychoBass {
    sr: f32,
    xf: f32,
    xf_target: f32,
    lp: [Biquad; 2],
    hp: [Biquad; 2],
    env: Envelope,
    h_hp: [Biquad; 2],
    h_lp: Biquad,
    int: Smoothed,
    bass: Smoothed,
    /// 1 = normal output, 0 = SOLO.
    normal: Smoothed,
}

impl PsychoBass {
    pub(super) fn new(ctx: FxContext) -> Self {
        let sr = ctx.sample_rate;
        let mut env = Envelope::default();
        env.set(1.0, 5.0, 40.0, sr);
        let mut s = Self {
            sr,
            xf: 80.0,
            xf_target: 80.0,
            lp: Default::default(),
            hp: Default::default(),
            env,
            h_hp: Default::default(),
            h_lp: Biquad::default(),
            int: Smoothed::new(0.0, SMOOTH_MS, sr),
            bass: Smoothed::new(0.0, SMOOTH_MS, sr),
            normal: Smoothed::new(1.0, SMOOTH_MS, sr),
        };
        s.tune();
        s
    }

    fn tune(&mut self) {
        let (sr, xf) = (self.sr, self.xf);
        for f in &mut self.lp {
            set_lp(f, sr, xf, Q_BUTTER);
        }
        for f in &mut self.hp {
            set_hp(f, sr, xf, Q_BUTTER);
        }
        // Harmonics: from just above the crossover up to about its 5th harmonic.
        set_hp(&mut self.h_hp[0], sr, xf * 1.2, 0.6);
        set_hp(&mut self.h_hp[1], sr, xf * 1.2, 0.9);
        set_lp(&mut self.h_lp, sr, (xf * 5.0).min(1200.0), Q_BUTTER);
    }
}

impl FxAlgo for PsychoBass {
    fn set_params(&mut self, p: &Params) {
        self.int.target = db_to_gain(p.f("int"));
        self.bass.target = fader_gain(p.f("bass"));
        self.normal.target = if p.on("solo") { 0.0 } else { 1.0 };
        self.xf_target = p.f("xf").clamp(20.0, 400.0);
    }

    fn process(&mut self, left: &mut [f32], right: &mut [f32]) {
        if self.xf != self.xf_target {
            let ratio = self.xf_target / self.xf;
            self.xf = if (ratio - 1.0).abs() < 0.01 { self.xf_target } else { self.xf * ratio.powf(0.3) };
            self.tune();
        }
        for (l, r) in left.iter_mut().zip(right.iter_mut()) {
            let mut low = [0.0; 2];
            let mut high = [0.0; 2];
            for (ch, x) in [*l, *r].into_iter().enumerate() {
                low[ch] = chain(&mut self.lp, x, ch);
                high[ch] = chain(&mut self.hp, x, ch);
            }
            let m = 0.5 * (low[0] + low[1]);
            let env = self.env.tick(m.abs());
            let h = if env > 1e-9 {
                let u = (m / env).clamp(-1.5, 1.5);
                env * (0.8 * u.abs() + 0.6 * fast_tanh(1.5 * u))
            } else {
                0.0
            };
            let h = self.h_lp.tick(chain(&mut self.h_hp, h, 0), 0) * 2.5;
            let (int, bass, normal) = (self.int.tick(), self.bass.tick(), self.normal.tick());
            let fx = h * int;
            // LR4 bands sum flat with the high band's polarity as is.
            let solo = 1.0 - normal;
            *l = normal * (high[0] + low[0] * bass + fx) + solo * low[0];
            *r = normal * (high[1] + low[1] * bass + fx) + solo * low[1];
        }
    }

    fn reset(&mut self) {
        for f in self.lp.iter_mut().chain(&mut self.hp).chain(&mut self.h_hp) {
            f.reset();
        }
        self.h_lp.reset();
        self.env.reset();
        self.int.snap(self.int.target);
        self.bass.snap(self.bass.target);
        self.normal.snap(self.normal.target);
        if self.xf != self.xf_target {
            self.xf = self.xf_target;
            self.tune();
        }
    }
}
