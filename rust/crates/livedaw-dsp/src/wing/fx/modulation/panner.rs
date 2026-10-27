//! PANNER: LFO tremolo / auto-pan with envelope control.

use super::shared::{advance, tri_sine_square};
use crate::wing::fx::blocks::{coef, Envelope, Smoothed};
use crate::wing::fx::readout::{ENVELOPE, LFO_L, LFO_R, NEEDLE};
use crate::wing::fx::{FxAlgo, FxContext, FX_READOUTS};
use crate::wing::Params;
use std::f32::consts::FRAC_PI_4;

/// ENV>SPEED at 100 % with a full envelope: two octaves faster.
const ENV_SPEED_OCTAVES: f32 = 2.0;

/// Tremolo Panner.
///
/// One LFO (SPEED; SHAPE morphs triangle (-50) through sine (0) to square
/// (+50), the square with rounded edges so it never clicks) drives a gain
/// per channel; the right channel's LFO runs PHASE L/R degrees
/// apart, so 0 is a tremolo, +-180 an auto-pan and anything between a
/// rotating mix of both. The gains follow a sine law (equal power when
/// panning) and DEPTH scales the swing. An envelope follower (ATTACK, HOLD,
/// RELEASE) on the input can speed the LFO up (ENV>SPEED, up to 4x) and
/// scale the depth with the level (ENV>DEPTH: at 100 % quiet passages stay
/// untouched and loud ones get the full DEPTH). Outputs the processed signal.
pub(super) struct Panner {
    sr: f32,
    phase: f32,
    rate: f32,
    /// Octaves of speed-up at full envelope.
    espd: Smoothed,
    edep: Smoothed,
    depth: Smoothed,
    /// Right LFO offset, cycles.
    offset: Smoothed,
    /// SHAPE, -1 triangle .. 0 sine .. 1 square.
    shape: Smoothed,
    env: Envelope,
    /// Smoothed channel gains (no clicks at the square's edges).
    gain: [f32; 2],
    gain_a: f32,
    primed: bool,
}

impl Panner {
    pub(super) fn new(ctx: FxContext) -> Self {
        let sr = ctx.sample_rate;
        let s = |v: f32, t: f32| Smoothed::new(v, t, sr);
        Self {
            sr,
            phase: 0.0,
            rate: 2.0,
            espd: s(0.0, 30.0),
            edep: s(0.0, 30.0),
            depth: s(0.7, 30.0),
            offset: s(0.5, 100.0),
            shape: s(0.0, 50.0),
            env: Envelope::default(),
            gain: [1.0; 2],
            gain_a: coef(1.5, sr),
            primed: false,
        }
    }

    fn smoothers(&mut self) -> [&mut Smoothed; 5] {
        [&mut self.espd, &mut self.edep, &mut self.depth, &mut self.offset, &mut self.shape]
    }

    /// Channel gains for the current LFO position (no smoothing).
    fn target_gains(&self, depth: f32, offset: f32, shape: f32) -> [f32; 2] {
        let g = |w: f32| 1.0 - depth + depth * (FRAC_PI_4 * (1.0 + w)).sin();
        [g(tri_sine_square(self.phase, shape)), g(tri_sine_square(self.phase + offset, shape))]
    }
}

impl FxAlgo for Panner {
    fn set_params(&mut self, p: &Params) {
        self.rate = p.f("spd");
        self.espd.target = p.f("espd") * 0.01 * ENV_SPEED_OCTAVES;
        self.edep.target = p.f("edep") * 0.01;
        self.depth.target = p.f("depth") * 0.01;
        self.offset.target = p.f("phase") / 360.0;
        self.shape.target = p.f("wave") / 50.0;
        self.env.set(p.f("att"), p.f("hld"), p.f("rel"), self.sr);
        if !self.primed {
            self.primed = true;
            for s in self.smoothers() {
                s.snap(s.target);
            }
            self.gain = self.target_gains(self.depth.value, self.offset.value, self.shape.value);
        }
    }

    fn process(&mut self, left: &mut [f32], right: &mut [f32]) {
        let inv_sr = 1.0 / self.sr;
        for (l, r) in left.iter_mut().zip(right.iter_mut()) {
            let level = (2.5 * self.env.tick(0.5 * (l.abs() + r.abs()))).min(1.0);
            let rate = self.rate * (self.espd.tick() * level).exp2();
            advance(&mut self.phase, (rate * inv_sr).min(0.5));
            let edep = self.edep.tick();
            let depth = self.depth.tick() * (1.0 - edep + edep * level);
            let (offset, shape) = (self.offset.tick(), self.shape.tick());
            let target = self.target_gains(depth, offset, shape);
            for (g, t) in self.gain.iter_mut().zip(target) {
                *g += (t - *g) * self.gain_a;
            }
            *l *= self.gain[0];
            *r *= self.gain[1];
        }
    }

    fn readouts(&self, out: &mut [f32; FX_READOUTS]) {
        out[LFO_L] = self.phase;
        out[LFO_R] = (self.phase + self.offset.value).rem_euclid(1.0);
        out[ENVELOPE] = (2.5 * self.env.env).clamp(0.0, 1.0);
        // Where the sound sits between the speakers: -1 left .. 1 right.
        let [l, r] = self.gain;
        out[NEEDLE] = ((r - l) / (l + r).max(1e-6)).clamp(-1.0, 1.0);
    }

    fn reset(&mut self) {
        self.env.reset();
        self.phase = 0.0;
        for s in self.smoothers() {
            s.snap(s.target);
        }
        self.gain = self.target_gains(self.depth.value, self.offset.value, self.shape.value);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::wing::{model_index, FX_MODELS, MAX_FX_PARAMS};

    fn gains(phase_deg: f32) -> Vec<(f32, f32)> {
        let spec = &FX_MODELS[model_index(FX_MODELS, "PANNER").unwrap()];
        let mut v = [0.0; MAX_FX_PARAMS];
        spec.defaults_into(&mut v);
        v[spec.param_index("phase").unwrap()] = phase_deg;
        v[spec.param_index("depth").unwrap()] = 100.0;
        let mut pan = Panner::new(FxContext { sample_rate: 48_000.0, max_block: 512 });
        pan.set_params(&Params::new(spec, &v));
        let mut l = vec![1.0; 48_000];
        let mut r = vec![1.0; 48_000];
        for (cl, cr) in l.chunks_mut(512).zip(r.chunks_mut(512)) {
            pan.process(cl, cr);
        }
        l.into_iter().zip(r).collect()
    }

    #[test]
    fn tremolo_and_auto_pan() {
        // PHASE 0: both channels move together, all the way down.
        let trem = gains(0.0);
        assert!(trem.iter().all(|(l, r)| (l - r).abs() < 1e-4));
        assert!(trem.iter().any(|(l, _)| *l < 0.05) && trem.iter().any(|(l, _)| *l > 0.95));
        // PHASE 180: equal-power auto-pan.
        let pan = gains(180.0);
        assert!(pan[1000..].iter().all(|(l, r)| (l * l + r * r - 1.0).abs() < 0.02));
        assert!(pan.iter().any(|(l, r)| *l > 0.95 && *r < 0.3));
        assert!(pan.iter().any(|(l, r)| *r > 0.95 && *l < 0.3));
    }
}
