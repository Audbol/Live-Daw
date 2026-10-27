//! MOOD: a Moog-style ladder filter with envelope and LFO sweeps.

use super::shared::Morph;
use crate::wing::fx::blocks::{coef, fast_tanh, Envelope, Lfo, Smoothed, Wave};
use crate::wing::fx::{FxAlgo, FxContext};
use crate::wing::Params;
use std::f32::consts::PI;

/// Output mix of (input, stage 1..4) for each TYPE x SLOPE (Oberheim Xpander
/// style multimode ladder): LP, HP, BP and NOTCH at 12 and 24 dB/oct.
const MODES: [[f32; 5]; 8] = [
    [0.0, 0.0, 1.0, 0.0, 0.0],
    [0.0, 0.0, 0.0, 0.0, 1.0],
    [1.0, -2.0, 1.0, 0.0, 0.0],
    [1.0, -4.0, 6.0, -4.0, 1.0],
    [0.0, 2.0, -2.0, 0.0, 0.0],
    [0.0, 0.0, 4.0, -8.0, 4.0],
    [1.0, -2.0, 2.0, 0.0, 0.0],
    [1.0, -4.0, 8.0, -8.0, 4.0],
];
/// Ladder feedback at RESO 10 (4 = self-oscillation).
const MAX_RESO: f32 = 3.95;
/// The low-pass modes win back half the bass the resonance takes.
const LP_COMP: f32 = 0.5;
/// DRIVE 10 = +18 dB into the ladder.
const DRIVE_DB: f32 = 18.0;
/// ENV at +-100 % with a full-scale envelope: +-4 octaves (up to 6 when hot).
const ENV_OCTAVES: f32 = 4.0;
/// LFO at 100 %: +-3 octaves.
const LFO_OCTAVES: f32 = 3.0;

/// LFO WAVE choices.
#[derive(Clone, Copy, PartialEq, Eq)]
enum MoodWave {
    Std(Wave),
    /// A rising ramp that accelerates (curved saw).
    Ramp,
}

const WAVES: [MoodWave; 7] = [
    MoodWave::Std(Wave::Triangle),
    MoodWave::Std(Wave::Sine),
    MoodWave::Std(Wave::SawUp),
    MoodWave::Std(Wave::SawDown),
    MoodWave::Ramp,
    MoodWave::Std(Wave::Square),
    MoodWave::Std(Wave::Random),
];

#[inline]
fn ramp(phase: f32) -> f32 {
    let p = phase.rem_euclid(1.0);
    2.0 * p * p - 1.0
}

/// Mood Filter (the filter section of a 1970s analog synthesizer).
///
/// A four-pole zero-delay-feedback ladder with a saturating input stage;
/// mixing its stage outputs gives TYPE LP/HP/BP/NOTCH at SLOPE 12 or 24
/// dB/oct. BASE is the cutoff at rest, RESO the ladder feedback (up to the
/// edge of self-oscillation) and DRIVE the level into the saturating input
/// (+18 dB at 10, partly made up at the output). ENV (-100..100 %) sweeps the
/// cutoff with the input level (follower with ATTACK, HOLD, RELEASE): up to
/// +-4 octaves at full scale. LFO sets a +-3 octave sweep at SPEED with LFO
/// WAVE (TRI, SIN, SAW+, SAW-, RMP = curved ramp, SQU, RND = smoothed
/// random); PHASE offsets the right channel's LFO. MIX is the device's own
/// dry/wet balance.
pub(super) struct Mood {
    sr: f32,
    lfo: Lfo,
    wave: MoodWave,
    /// LFO values after a short smoothing (softens SQU and the saw edges).
    lfo_sm: [f32; 2],
    lfo_a: f32,
    /// log2 of BASE in Hz.
    base: Smoothed,
    reso: Smoothed,
    pre: Smoothed,
    post: Smoothed,
    env_amt: Smoothed,
    lfo_amt: Smoothed,
    offset: Smoothed,
    mix: Smoothed,
    env: Envelope,
    stages: [[f32; 4]; 2],
    mode: usize,
    next_mode: usize,
    morph: Morph,
    primed: bool,
}

impl Mood {
    pub(super) fn new(ctx: FxContext) -> Self {
        let sr = ctx.sample_rate;
        let s = |v: f32, t: f32| Smoothed::new(v, t, sr);
        Self {
            sr,
            lfo: Lfo::default(),
            wave: WAVES[0],
            lfo_sm: [0.0; 2],
            lfo_a: coef(1.5, sr),
            base: s(800f32.log2(), 30.0),
            reso: s(0.0, 30.0),
            pre: s(1.0, 30.0),
            post: s(1.0, 30.0),
            env_amt: s(0.0, 30.0),
            lfo_amt: s(0.0, 30.0),
            offset: s(0.0, 100.0),
            mix: s(1.0, 20.0),
            env: Envelope::default(),
            stages: [[0.0; 4]; 2],
            mode: 1,
            next_mode: 1,
            morph: Morph::new(6.0, sr),
            primed: false,
        }
    }

    fn smoothers(&mut self) -> [&mut Smoothed; 8] {
        [
            &mut self.base,
            &mut self.reso,
            &mut self.pre,
            &mut self.post,
            &mut self.env_amt,
            &mut self.lfo_amt,
            &mut self.offset,
            &mut self.mix,
        ]
    }

    /// Advances the LFO; returns the left and right values.
    #[inline]
    fn lfo_tick(&mut self, offset: f32) -> (f32, f32) {
        match self.wave {
            MoodWave::Std(w) => {
                let l = self.lfo.next(w, 0.0);
                (l, self.lfo.at(w, offset))
            }
            MoodWave::Ramp => {
                self.lfo.next(Wave::SawUp, 0.0);
                (ramp(self.lfo.phase), ramp(self.lfo.phase + offset))
            }
        }
    }
}

impl FxAlgo for Mood {
    fn set_params(&mut self, p: &Params) {
        let sr = self.sr;
        self.base.target = p.f("fbase").max(1.0).log2();
        self.reso.target = p.f("reso") * 0.1 * MAX_RESO;
        let pre = 10f32.powf(p.f("drv") * 0.1 * DRIVE_DB / 20.0);
        self.pre.target = pre;
        self.post.target = pre.powf(-0.8);
        self.env_amt.target = p.f("env") * 0.01 * ENV_OCTAVES;
        self.lfo_amt.target = p.f("lfo") * 0.01 * LFO_OCTAVES;
        self.offset.target = p.f("phase") / 360.0;
        self.mix.target = p.f("mix") * 0.01;
        self.env.set(p.f("att"), p.f("hld"), p.f("rel"), sr);
        self.lfo.set_rate(p.f("spd"), sr);
        self.wave = WAVES.get(p.choice("wave")).copied().unwrap_or(WAVES[0]);
        self.next_mode = (p.choice("filt") * 2 + p.choice("slope").min(1)).min(MODES.len() - 1);
        if !self.primed {
            self.primed = true;
            self.mode = self.next_mode;
            for s in self.smoothers() {
                s.snap(s.target);
            }
        } else if self.next_mode != self.mode {
            self.morph.request();
        }
    }

    fn process(&mut self, left: &mut [f32], right: &mut [f32]) {
        let (sr, nyq) = (self.sr, self.sr * 0.45);
        for (l, r) in left.iter_mut().zip(right.iter_mut()) {
            if self.morph.tick() {
                self.mode = self.next_mode;
            }
            let level = (2.0 * self.env.tick(0.5 * (l.abs() + r.abs()))).min(1.5);
            let off = self.offset.tick();
            let (vl, vr) = self.lfo_tick(off);
            self.lfo_sm[0] += (vl - self.lfo_sm[0]) * self.lfo_a;
            self.lfo_sm[1] += (vr - self.lfo_sm[1]) * self.lfo_a;
            let centre = self.base.tick() + self.env_amt.tick() * level;
            let lfo_amt = self.lfo_amt.tick();
            let k = self.reso.tick();
            let pre = self.pre.tick();
            let post = self.post.tick() * self.morph.gain();
            let mix = self.mix.tick();
            let c = MODES[self.mode.min(MODES.len() - 1)];
            let comp = if self.mode < 2 { 1.0 + LP_COMP * k } else { 1.0 };

            for (ch, x) in [l, r].into_iter().enumerate() {
                let hz = (centre + lfo_amt * self.lfo_sm[ch]).exp2().clamp(20.0, nyq);
                let g = (PI * hz / sr).tan();
                let gg = g / (1.0 + g);
                let b = 1.0 - gg;
                let s = &mut self.stages[ch];
                // Solve the feedback loop for the linear ladder, then saturate its input.
                let xin = *x * pre * comp;
                let g2 = gg * gg;
                let sigma = b * (g2 * gg * s[0] + g2 * s[1] + gg * s[2] + s[3]);
                let y4 = (g2 * g2 * xin + sigma) / (1.0 + k * g2 * g2);
                let u = 2.0 * fast_tanh(0.5 * (xin - k * y4));
                let mut y = u;
                let mut out = c[0] * u;
                for (st, ci) in s.iter_mut().zip(&c[1..]) {
                    let v = (y - *st) * gg;
                    y = v + *st;
                    *st = y + v;
                    out += ci * y;
                }
                *x += (out * post - *x) * mix;
            }
        }
    }

    fn reset(&mut self) {
        self.stages = [[0.0; 4]; 2];
        self.mode = self.next_mode;
        self.morph.reset();
        self.env.reset();
        self.lfo.reset(0.0);
        self.lfo_sm = [0.0; 2];
        for s in self.smoothers() {
            s.snap(s.target);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::wing::{model_index, FX_MODELS, MAX_FX_PARAMS};
    use std::f32::consts::TAU;

    fn rms_out(settings: &[(&str, f32)], hz: f32, amp: f32) -> f32 {
        let sr = 48_000.0;
        let spec = &FX_MODELS[model_index(FX_MODELS, "MOOD").unwrap()];
        let mut v = [0.0; MAX_FX_PARAMS];
        spec.defaults_into(&mut v);
        for (id, x) in settings {
            v[spec.param_index(id).unwrap()] = *x;
        }
        let mut f = Mood::new(FxContext { sample_rate: sr, max_block: 512 });
        f.set_params(&Params::new(spec, &v));
        let mut l: Vec<f32> = (0..24_000).map(|i| amp * (TAU * hz * i as f32 / sr).sin()).collect();
        let mut r = l.clone();
        for (cl, cr) in l.chunks_mut(512).zip(r.chunks_mut(512)) {
            f.process(cl, cr);
        }
        (l[12_000..].iter().map(|x| x * x).sum::<f32>() / 12_000.0).sqrt() / amp
    }

    #[test]
    fn types_and_envelope() {
        let base = [("fbase", 500.0), ("env", 0.0), ("reso", 0.0), ("drv", 0.0), ("lfo", 0.0)];
        let with = |extra: &[(&'static str, f32)]| -> Vec<(&'static str, f32)> { base.iter().chain(extra).copied().collect() };
        let lp = with(&[("filt", 0.0)]);
        assert!(rms_out(&lp, 5000.0, 0.3) < 0.05 * rms_out(&lp, 100.0, 0.3));
        let hp = with(&[("filt", 1.0)]);
        assert!(rms_out(&hp, 100.0, 0.3) < 0.05 * rms_out(&hp, 5000.0, 0.3));
        let notch = with(&[("filt", 3.0)]);
        assert!(rms_out(&notch, 500.0, 0.3) < 0.1 * rms_out(&notch, 4000.0, 0.3));
        // ENV opens the low-pass for loud signals only.
        let env = [("fbase", 300.0), ("env", 100.0), ("reso", 0.0), ("drv", 0.0), ("lfo", 0.0)];
        assert!(rms_out(&env, 3000.0, 0.6) > 10.0 * rms_out(&env, 3000.0, 0.01));
    }
}
