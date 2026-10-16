//! The aural exciters (WING manual 11.8).
//!
//! Ultra Enhancer, after the tube version of a German stereo processor from
//! the 1990s, made to "unmask" sounds for more intelligible, detailed mixes.
//! The input is split into a bass band (low-pass at BASS FREQ, 30..400 Hz), a
//! mid band (band-pass at 1.2 kHz, MID Q 0.4..8) and a high band (high-pass at
//! HIGH FREQ, 1..12 kHz). Each band adds back a phase-shifted copy of itself
//! plus generated harmonics, accentuating its range; the copy is ducked by the
//! band's own envelope, so the enhancement is strongest on quiet detail and
//! backs off on loud passages. BASS/MID/HIGH GAIN set the amounts. The output
//! is then split into its stereo (side) and mono (mid) information: ST LVL and
//! MONO LVL set their levels (-100 % = off, 0 % = unchanged, +100 % = double),
//! ST PAN and MONO PAN pan them, and LMF SPRD sets the stereo width of the
//! low-mid content (150 Hz .. 1.5 kHz: -100 % = mono, +100 % = double side).
//! GAIN is the overall output level; SOLO outputs only the added enhancement.
//!
//! Exciter, after the mid-1970s aural exciter that added harmonics while
//! keeping the output level: a side chain low-cut at TUNE (PEAK is its
//! resonance: it emphasises TUNE and deepens the dip just below it, where the
//! side chain is out of phase with the signal; ZERO FILL smooths that dip with
//! a gentler slope) drives a harmonics generator. HARMONICS sets the amount of
//! harmonics (the filtered side chain still brightens with it at minimum),
//! TIMBRE adds odd (negative) or even (positive) harmonics, and MIX sets the
//! balance between processed and unprocessed signal (the processed signal is
//! the input plus the exciter signal). DRY OFF removes the unprocessed signal,
//! leaving the exciter signal alone (for send/return use).

use super::{dial50, rail, sub_blocks, Glide};
use crate::wing::fx::blocks::{coef, db_to_gain, fast_tanh, Biquad, BiquadCoeffs, Smoothed};
use crate::wing::fx::FxAlgo;
use crate::wing::Params;

/// Distortion residual of a soft clipper at drive `d`: about `d^2 y^3 / 3` for small
/// `y` (odd harmonics growing with level), bounded by `y`.
#[inline]
fn harmonics(y: f32, d: f32) -> f32 {
    y - fast_tanh(d * y) / d
}

/// First-order all-pass on two channels (phase shift without changing the level).
#[derive(Clone, Copy, Default)]
struct Allpass1 {
    a: f32,
    x1: [f32; 2],
    y1: [f32; 2],
}

impl Allpass1 {
    fn set(&mut self, hz: f32, sr: f32) {
        let k = (std::f32::consts::PI * hz.min(sr * 0.45) / sr).tan();
        self.a = (k - 1.0) / (k + 1.0);
    }

    #[inline]
    fn tick(&mut self, x: f32, ch: usize) -> f32 {
        let y = self.a * x + self.x1[ch] - self.a * self.y1[ch];
        self.x1[ch] = x;
        self.y1[ch] = y;
        y
    }

    fn reset(&mut self) {
        self.x1 = [0.0; 2];
        self.y1 = [0.0; 2];
    }
}

const MID_HZ: f64 = 1200.0;

pub(crate) struct Enhancer {
    sr: f32,
    bass: Biquad,
    mid: Biquad,
    high: Biquad,
    mid_phase: Allpass1,
    high_phase: Allpass1,
    /// The low-mid range of the side signal (LMF SPRD).
    low_mid: Biquad,
    /// ln(bass Hz), ln(mid Q), ln(high Hz).
    glide: Glide<3>,
    env: [[f32; 2]; 3],
    env_att: f32,
    env_rel: f32,
    amount: [Smoothed; 3],
    mono_lvl: Smoothed,
    st_lvl: Smoothed,
    mono_pan: Smoothed,
    st_pan: Smoothed,
    spread: Smoothed,
    gain: Smoothed,
    solo: Smoothed,
    primed: bool,
}

impl Enhancer {
    pub(crate) fn new(sr: f32) -> Self {
        let sm = |v: f32| Smoothed::new(v, 30.0, sr);
        let mut mid_phase = Allpass1::default();
        mid_phase.set(MID_HZ as f32, sr);
        let mut low_mid = Biquad::default();
        low_mid.set_coefficients(BiquadCoeffs::band_pass(sr as f64, 475.0, 0.45));
        let mut s = Self {
            sr,
            bass: Biquad::default(),
            mid: Biquad::default(),
            high: Biquad::default(),
            mid_phase,
            high_phase: Allpass1::default(),
            low_mid,
            glide: Glide::new(0.0),
            env: [[0.0; 2]; 3],
            env_att: coef(5.0, sr),
            env_rel: coef(120.0, sr),
            amount: [sm(0.0); 3],
            mono_lvl: sm(1.0),
            st_lvl: sm(1.0),
            mono_pan: sm(0.0),
            st_pan: sm(0.0),
            spread: sm(0.0),
            gain: sm(1.0),
            solo: sm(0.0),
            primed: false,
        };
        s.glide.tgt = [100f32.ln(), 1.5f32.ln(), 3000f32.ln()];
        s.glide.snap();
        s.redesign();
        s
    }

    fn redesign(&mut self) {
        let sr = self.sr as f64;
        let [bf, mq, hf] = self.glide.cur.map(|v| v.exp() as f64);
        self.bass.set_coefficients(BiquadCoeffs::low_pass(sr, bf, 0.6));
        self.mid.set_coefficients(BiquadCoeffs::band_pass(sr, MID_HZ, mq));
        self.high.set_coefficients(BiquadCoeffs::high_pass(sr, hf, 0.6));
        self.high_phase.set(hf as f32 * 1.5, self.sr);
    }

    fn smoothers(&mut self) -> [&mut Smoothed; 10] {
        let [a, b, c] = &mut self.amount;
        [
            a,
            b,
            c,
            &mut self.mono_lvl,
            &mut self.st_lvl,
            &mut self.mono_pan,
            &mut self.st_pan,
            &mut self.spread,
            &mut self.gain,
            &mut self.solo,
        ]
    }
}

impl FxAlgo for Enhancer {
    fn set_params(&mut self, p: &Params) {
        self.glide.tgt = [
            dial50(p.f("bassf"), 30.0, 400.0).ln(),
            dial50(p.f("midq"), 0.4, 8.0).ln(),
            dial50(p.f("highf"), 1000.0, 12_000.0).ln(),
        ];
        let pct = |id: &str| p.f(id) / 100.0;
        self.amount[0].target = pct("bass") * 1.2;
        self.amount[1].target = pct("mid") * 1.0;
        self.amount[2].target = pct("high") * 1.4;
        self.mono_lvl.target = 1.0 + pct("mlvl");
        self.st_lvl.target = 1.0 + pct("stlvl");
        self.mono_pan.target = pct("m");
        self.st_pan.target = pct("st");
        self.spread.target = pct("lmf");
        self.gain.target = db_to_gain(p.f("g"));
        self.solo.target = if p.on("solo") { 1.0 } else { 0.0 };
        if !self.primed {
            for s in self.smoothers() {
                s.snap(s.target);
            }
            self.glide.snap();
            self.redesign();
            self.primed = true;
        }
    }

    fn process(&mut self, left: &mut [f32], right: &mut [f32]) {
        let n = left.len().min(right.len());
        sub_blocks(n, |a, b| {
            if self.glide.step() != 0 {
                self.redesign();
            }
            for (l, r) in left[a..b].iter_mut().zip(right[a..b].iter_mut()) {
                let [gb, gm, gh] = [self.amount[0].tick(), self.amount[1].tick(), self.amount[2].tick()];
                let x = [*l, *r];
                let mut enh = [0.0f32; 2];
                for c in 0..2 {
                    let bands = [self.bass.tick(x[c], c), self.mid.tick(x[c], c), self.high.tick(x[c], c)];
                    let mut duck = [0.0f32; 3];
                    for ((d, env), y) in duck.iter_mut().zip(self.env.iter_mut()).zip(bands) {
                        let v = y.abs();
                        env[c] += (v - env[c]) * if v > env[c] { self.env_att } else { self.env_rel };
                        *d = 1.0 / (1.0 + 3.0 * env[c]);
                    }
                    let eb = gb * (duck[0] * bands[0] + 1.5 * harmonics(bands[0], 2.0));
                    let em = gm * (duck[1] * self.mid_phase.tick(bands[1], c) + harmonics(bands[1], 2.5));
                    let eh = gh * (duck[2] * self.high_phase.tick(bands[2], c) + 2.0 * harmonics(bands[2], 4.0));
                    enh[c] = eb + em + eh;
                }
                let dry = 1.0 - self.solo.tick();
                let y = [x[0] * dry + enh[0], x[1] * dry + enh[1]];
                // Mono and stereo information, the low-mid width, levels and pans.
                let mid = 0.5 * (y[0] + y[1]);
                let mut side = 0.5 * (y[0] - y[1]);
                side += self.spread.tick() * self.low_mid.tick(side, 0);
                let mid = mid * self.mono_lvl.tick();
                let side = side * self.st_lvl.tick();
                let (pm, ps) = (self.mono_pan.tick(), self.st_pan.tick());
                let g = self.gain.tick();
                *l = rail((mid * (1.0 - pm).min(1.0) + side * (1.0 - ps).min(1.0)) * g);
                *r = rail((mid * (1.0 + pm).min(1.0) - side * (1.0 + ps).min(1.0)) * g);
            }
        });
    }

    fn reset(&mut self) {
        self.bass.reset();
        self.mid.reset();
        self.high.reset();
        self.mid_phase.reset();
        self.high_phase.reset();
        self.low_mid.reset();
        self.env = [[0.0; 2]; 3];
        for s in self.smoothers() {
            s.snap(s.target);
        }
        self.glide.snap();
        self.redesign();
    }
}

// ---- Exciter ---------------------------------------------------------------------------

pub(crate) struct Exciter {
    sr: f32,
    hp2: Biquad,
    hp1: Biquad,
    /// Removes the DC and low intermodulation the even harmonics make.
    post: Biquad,
    /// ln(TUNE Hz), ln(Q).
    glide: Glide<2>,
    zfill: Smoothed,
    harm: Smoothed,
    timbre: Smoothed,
    mix: Smoothed,
    dry: Smoothed,
    primed: bool,
}

impl Exciter {
    pub(crate) fn new(sr: f32) -> Self {
        let sm = |v: f32| Smoothed::new(v, 30.0, sr);
        let mut s = Self {
            sr,
            hp2: Biquad::default(),
            hp1: Biquad::default(),
            post: Biquad::default(),
            glide: Glide::new(0.0),
            zfill: sm(0.0),
            harm: sm(0.0),
            timbre: sm(0.0),
            mix: sm(0.0),
            dry: sm(1.0),
            primed: false,
        };
        s.glide.tgt = [4000f32.ln(), 0.7f32.ln()];
        s.glide.snap();
        s.redesign();
        s
    }

    fn redesign(&mut self) {
        let sr = self.sr as f64;
        let [f, q] = self.glide.cur.map(|v| v.exp() as f64);
        self.hp2.set_coefficients(BiquadCoeffs::high_pass(sr, f, q));
        self.hp1.set_coefficients(BiquadCoeffs::high_pass_1(sr, f));
        self.post.set_coefficients(BiquadCoeffs::high_pass_1(sr, f * 0.25));
    }

    fn smoothers(&mut self) -> [&mut Smoothed; 5] {
        [&mut self.zfill, &mut self.harm, &mut self.timbre, &mut self.mix, &mut self.dry]
    }
}

impl FxAlgo for Exciter {
    fn set_params(&mut self, p: &Params) {
        let pct = |id: &str| p.f(id) / 100.0;
        self.glide.tgt = [p.f("tune").ln(), (0.6 + 2.4 * pct("peak")).ln()];
        self.zfill.target = pct("zero");
        self.harm.target = pct("hrm");
        self.timbre.target = p.f("tmb") / 50.0;
        self.mix.target = pct("mix");
        self.dry.target = if p.on("solo") { 0.0 } else { 1.0 };
        if !self.primed {
            for s in self.smoothers() {
                s.snap(s.target);
            }
            self.glide.snap();
            self.redesign();
            self.primed = true;
        }
    }

    fn process(&mut self, left: &mut [f32], right: &mut [f32]) {
        let n = left.len().min(right.len());
        sub_blocks(n, |a, b| {
            if self.glide.step() != 0 {
                self.redesign();
            }
            for (l, r) in left[a..b].iter_mut().zip(right[a..b].iter_mut()) {
                let zf = self.zfill.tick();
                let h = self.harm.tick();
                let t = self.timbre.tick();
                let mix = self.mix.tick();
                let dry = self.dry.tick();
                let d = 1.0 + 12.0 * h;
                for (c, x) in [l, r].into_iter().enumerate() {
                    let s2 = self.hp2.tick(*x, c);
                    let s1 = self.hp1.tick(*x, c);
                    let s = s2 + zf * (s1 - s2);
                    let u = d * s;
                    let odd = fast_tanh(u) / d;
                    let even = fast_tanh(u * u.abs()) / d;
                    let gen = 0.5 * (1.0 - t) * odd + 0.5 * (1.0 + t) * even;
                    let side = self.post.tick((1.0 - h) * s + h * 2.0 * gen, c);
                    *x = *x * dry + mix * side;
                }
            }
        });
    }

    fn reset(&mut self) {
        self.hp2.reset();
        self.hp1.reset();
        self.post.reset();
        for s in self.smoothers() {
            s.snap(s.target);
        }
        self.glide.snap();
        self.redesign();
    }
}

#[cfg(test)]
mod tests {
    use super::super::tests::Rig;

    #[test]
    fn enhancer_adds_top_and_solo_isolates_it() {
        let mut rig = Rig::new("ENHANCE");
        rig.set("high", 100.0).set("bass", 0.0).set("mid", 0.0);
        let top = rig.sine_gain_db(8000.0, 0.05);
        assert!(top > 1.0, "{top}");
        let mut rig = Rig::new("ENHANCE");
        rig.set("high", 100.0).set("bass", 0.0).set("mid", 0.0).set("solo", 1.0);
        let low = rig.sine_gain_db(60.0, 0.05);
        assert!(low < -30.0, "{low}");
    }

    #[test]
    fn exciter_leaves_lows_alone() {
        let mut rig = Rig::new("EXCITER");
        let low = rig.sine_gain_db(100.0, 0.2);
        assert!(low.abs() < 0.3, "{low}");
        let mut rig = Rig::new("EXCITER");
        rig.set("mix", 100.0);
        let high = rig.sine_gain_db(8000.0, 0.2);
        assert!(high > 1.0, "{high}");
    }

    #[test]
    fn exciter_peak_makes_the_dip_and_the_peak() {
        // As in the manual's response plots: a dip just below TUNE, a peak just above.
        let response = |peak: f32, freq: f32| {
            let mut rig = Rig::new("EXCITER");
            rig.set("tune", 1000.0).set("peak", peak).set("zero", 0.0).set("hrm", 0.0).set("mix", 50.0);
            rig.sine_gain_db(freq, 0.01)
        };
        assert!(response(100.0, 800.0) < -2.0);
        assert!(response(100.0, 1300.0) > 4.0);
        assert!(response(0.0, 800.0) > response(100.0, 800.0) + 1.0);
        // ZERO FILL smooths the dip.
        let mut rig = Rig::new("EXCITER");
        rig.set("tune", 1000.0).set("peak", 100.0).set("zero", 100.0).set("hrm", 0.0).set("mix", 50.0);
        assert!(rig.sine_gain_db(800.0, 0.01) > response(100.0, 800.0));
    }
}
