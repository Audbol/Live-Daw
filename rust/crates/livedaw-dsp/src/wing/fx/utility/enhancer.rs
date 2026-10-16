//! Enhancers: the Ultra Enhancer (a dynamic three-band harmonic/phase
//! enhancer) and the Exciter (after the Aphex Aural Exciter).
//!
//! Ultra Enhancer: the input is split into a bass band (low-pass at BASS FREQ,
//! 30..400 Hz), a mid band (band-pass at 1.2 kHz, MID Q 0.4..8) and a high
//! band (high-pass at HIGH FREQ, 1..12 kHz). Each band adds back a phase-shifted
//! copy of itself plus generated harmonics; the copy is ducked by the band's
//! own envelope, so the enhancement is strongest on quiet detail and backs off
//! on loud passages. BASS/MID/HIGH GAIN set the amounts. The enhancement is
//! then split into mono and stereo parts: MONO LVL and ST LVL scale them
//! (-100 % = off, 0 % = as made, +100 % = double), MONO PAN and ST PAN place
//! them, and LMF SPRD spreads the bass and mid enhancement across the stereo
//! field (positive adds a decorrelated copy to the side, negative narrows it).
//! GAIN is the output level; SOLO outputs the enhancement only.
//!
//! Exciter: a side chain high-passed at TUNE (PEAK adds resonance at the
//! corner; ZFILL fills in below it with a gentler slope) drives a harmonics
//! generator. HARM sets how much of the side chain is harmonics, TIMBRE
//! balances even (warm, negative) against odd (bright, positive) harmonics, and
//! MIX sets how much of it is added to the signal. DRY OFF outputs the exciter
//! signal alone.

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
    spread_ap: Allpass1,
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
        let mut spread_ap = Allpass1::default();
        spread_ap.set(250.0, sr);
        let mut s = Self {
            sr,
            bass: Biquad::default(),
            mid: Biquad::default(),
            high: Biquad::default(),
            mid_phase,
            high_phase: Allpass1::default(),
            spread_ap,
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
                let mut low_mid = [0.0f32; 2];
                let mut hi = [0.0f32; 2];
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
                    low_mid[c] = eb + em;
                    hi[c] = eh;
                }
                // Mono and stereo parts of the enhancement.
                let e = [low_mid[0] + hi[0], low_mid[1] + hi[1]];
                let mid = 0.5 * (e[0] + e[1]);
                let mut side = 0.5 * (e[0] - e[1]);
                let spread = self.spread.tick();
                if spread >= 0.0 {
                    side += spread * self.spread_ap.tick(0.5 * (low_mid[0] + low_mid[1]), 0);
                } else {
                    self.spread_ap.tick(0.5 * (low_mid[0] + low_mid[1]), 0);
                    side += spread * 0.5 * (low_mid[0] - low_mid[1]);
                }
                let mid = mid * self.mono_lvl.tick();
                let side = side * self.st_lvl.tick();
                let (pm, ps) = (self.mono_pan.tick(), self.st_pan.tick());
                let el = mid * (1.0 - pm).min(1.0) + side * (1.0 - ps).min(1.0);
                let er = mid * (1.0 + pm).min(1.0) - side * (1.0 + ps).min(1.0);
                let dry = 1.0 - self.solo.tick();
                let g = self.gain.tick();
                *l = rail((x[0] * dry + el) * g);
                *r = rail((x[1] * dry + er) * g);
            }
        });
    }

    fn reset(&mut self) {
        self.bass.reset();
        self.mid.reset();
        self.high.reset();
        self.mid_phase.reset();
        self.high_phase.reset();
        self.spread_ap.reset();
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
                    let gen = 0.5 * (1.0 + t) * odd + 0.5 * (1.0 - t) * even;
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
}
