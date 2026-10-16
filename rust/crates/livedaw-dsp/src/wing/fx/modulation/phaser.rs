//! PHASER: a chain of swept first-order allpasses with feedback.

use super::shared::{advance, loop_limit, skew_tri, Morph};
use crate::wing::fx::blocks::{coef, Envelope, Smoothed};
use crate::wing::fx::{FxAlgo, FxContext};
use crate::wing::Params;
use std::f32::consts::PI;

const MAX_STAGES: usize = 12;
/// RANGE 0..100 % places the sweep centre between 40 Hz and 16 kHz (log).
const RANGE_LO_HZ: f32 = 40.0;
const RANGE_SPAN: f32 = 400.0;
/// Sweep at 100 % DEPTH: +-3 octaves around the centre.
const DEPTH_OCTAVES: f32 = 3.0;
/// ENV MOD at +-100 % with a full-scale envelope: +-3 octaves.
const ENV_OCTAVES: f32 = 3.0;

/// Phaser.
///
/// STAGES (2-12) zero-delay-feedback first-order allpasses share one swept
/// break frequency per channel; mixed with the dry signal (MIX 50 % gives
/// the deepest notches) they cut STAGES/2 moving notches. RANGE sets the
/// centre of the sweep (40 Hz .. 16 kHz), DEPTH its width (up to +-3
/// octaves), SPEED the LFO rate and WAVE its shape (-50 falling ramp, 0
/// triangle, +50 rising ramp). PHASE delays the right channel's LFO. ENV
/// MOD moves the sweep with the input level (envelope with ATTACK, HOLD and
/// RELEASE); negative values sweep down. RESO feeds the chain's output back
/// to its input for sharper, ringing notches.
pub(super) struct Phaser {
    sr: f32,
    phase: f32,
    inc: f32,
    skew: Smoothed,
    offset: Smoothed,
    /// log2 of the sweep centre in Hz.
    centre: Smoothed,
    /// Octaves.
    depth: Smoothed,
    /// Octaves at full envelope.
    emod: Smoothed,
    fb: Smoothed,
    mix: Smoothed,
    env: Envelope,
    /// LFO after a short smoothing (rounds the ramp returns).
    sweep: [f32; 2],
    sweep_a: f32,
    state: [[f32; MAX_STAGES]; 2],
    last: [f32; 2],
    stages: usize,
    next_stages: usize,
    morph: Morph,
    primed: bool,
}

impl Phaser {
    pub(super) fn new(ctx: FxContext) -> Self {
        let sr = ctx.sample_rate;
        let s = |v: f32, t: f32| Smoothed::new(v, t, sr);
        Self {
            sr,
            phase: 0.0,
            inc: 0.0,
            skew: s(0.0, 50.0),
            offset: s(0.25, 100.0),
            centre: s(10.0, 50.0),
            depth: s(2.0, 50.0),
            emod: s(0.0, 50.0),
            fb: s(0.3, 30.0),
            mix: s(0.5, 20.0),
            env: Envelope::default(),
            sweep: [0.0; 2],
            sweep_a: coef(2.0, sr),
            state: [[0.0; MAX_STAGES]; 2],
            last: [0.0; 2],
            stages: 6,
            next_stages: 6,
            morph: Morph::new(8.0, sr),
            primed: false,
        }
    }

    fn smoothers(&mut self) -> [&mut Smoothed; 7] {
        [&mut self.skew, &mut self.offset, &mut self.centre, &mut self.depth, &mut self.emod, &mut self.fb, &mut self.mix]
    }

    fn clear(&mut self) {
        self.state = [[0.0; MAX_STAGES]; 2];
        self.last = [0.0; 2];
    }
}

impl FxAlgo for Phaser {
    fn set_params(&mut self, p: &Params) {
        let sr = self.sr;
        self.inc = (p.f("spd") / sr).min(0.5);
        self.skew.target = p.f("wave") / 50.0;
        self.offset.target = p.f("phase") / 360.0;
        self.centre.target = RANGE_LO_HZ.log2() + p.f("range") * 0.01 * RANGE_SPAN.log2();
        self.depth.target = p.f("depth") * 0.01 * DEPTH_OCTAVES;
        self.emod.target = p.f("emod") * 0.01 * ENV_OCTAVES;
        self.fb.target = p.f("reso") * 0.01;
        self.mix.target = p.f("mix") * 0.01;
        self.env.set(p.f("att"), p.f("hld"), p.f("rel"), sr);
        self.next_stages = (p.f("stg").round() as usize).clamp(2, MAX_STAGES);
        if !self.primed {
            self.primed = true;
            self.stages = self.next_stages;
            for s in self.smoothers() {
                s.snap(s.target);
            }
        } else if self.next_stages != self.stages {
            self.morph.request();
        }
    }

    fn process(&mut self, left: &mut [f32], right: &mut [f32]) {
        let (sr, nyq) = (self.sr, self.sr * 0.45);
        for (l, r) in left.iter_mut().zip(right.iter_mut()) {
            if self.morph.tick() {
                self.stages = self.next_stages;
                self.clear();
            }
            let level = self.env.tick(0.5 * (l.abs() + r.abs()));
            let env_oct = self.emod.tick() * (2.0 * level).min(2.0);
            advance(&mut self.phase, self.inc);
            let centre = self.centre.tick() + env_oct;
            let depth = self.depth.tick();
            let skew = self.skew.tick();
            let off = self.offset.tick();
            let fb = self.fb.tick();
            let mix = self.mix.tick();
            let wet_g = self.morph.gain();
            let stages = self.stages;
            for (ch, x) in [l, r].into_iter().enumerate() {
                let w = skew_tri(self.phase + off * ch as f32, skew);
                self.sweep[ch] += (w - self.sweep[ch]) * self.sweep_a;
                let hz = (centre + depth * self.sweep[ch]).exp2().clamp(20.0, nyq);
                let g = (PI * hz / sr).tan();
                let gg = g / (1.0 + g);
                let mut y = loop_limit(*x + fb * self.last[ch]);
                for s in self.state[ch].iter_mut().take(stages) {
                    let v = (y - *s) * gg;
                    let lp = v + *s;
                    *s = lp + v;
                    y = 2.0 * lp - y;
                }
                self.last[ch] = y;
                *x += (y * wet_g - *x) * mix;
            }
        }
    }

    fn reset(&mut self) {
        self.clear();
        self.stages = self.next_stages;
        self.morph.reset();
        self.env.reset();
        self.phase = 0.0;
        self.sweep = [0.0; 2];
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

    #[test]
    fn notches_move() {
        // A steady sine through the phaser at 50 % mix swells and dips as the
        // notches sweep across it.
        let sr = 48_000.0;
        let spec = &FX_MODELS[model_index(FX_MODELS, "PHASER").unwrap()];
        let mut v = [0.0; MAX_FX_PARAMS];
        spec.defaults_into(&mut v);
        v[spec.param_index("spd").unwrap()] = 1.0;
        v[spec.param_index("reso").unwrap()] = 0.0;
        let mut ph = Phaser::new(FxContext { sample_rate: sr, max_block: 256 });
        ph.set_params(&Params::new(spec, &v));
        let mut levels = Vec::new();
        let mut n = 0usize;
        for _ in 0..(2 * 48_000 / 480) {
            let mut l: Vec<f32> = (0..480).map(|i| (TAU * 800.0 * (n + i) as f32 / sr).sin() * 0.5).collect();
            let mut r = l.clone();
            n += 480;
            ph.process(&mut l, &mut r);
            levels.push(l.iter().map(|x| x * x).sum::<f32>().sqrt());
        }
        let (lo, hi) = levels[10..].iter().fold((f32::MAX, 0.0f32), |(a, b), &x| (a.min(x), b.max(x)));
        assert!(hi > 4.0 * lo, "sweep too shallow: {lo} .. {hi}");
    }
}
