//! ROTARY: a Leslie 122 style rotating speaker.

use crate::wing::fx::blocks::{fast_tanh, DelayLine, OnePole, Smoothed, Svf};
use crate::wing::fx::{FxAlgo, FxContext};
use crate::wing::Params;
use std::f32::consts::{FRAC_1_SQRT_2, TAU};

const CROSSOVER_HZ: f32 = 800.0;
/// The drum turns a little slower than the horn (Leslie 122: 0.7/5.8 Hz
/// against 0.8/6.7 Hz).
const DRUM_RATIO: f32 = 0.85;
/// Direct path to the mics and the horn's radius as delay (15 cm / c).
const HORN_BASE_MS: f32 = 1.2;
const HORN_RADIUS_MS: f32 = 0.44;
/// Path of the reflection off the cabinet (opposite Doppler).
const HORN_REFLECT_MS: f32 = 2.6;
const DRUM_BASE_MS: f32 = 1.2;
const DRUM_RADIUS_MS: f32 = 0.3;
/// Time constants (s) to reach the new speed at ACCEL 0 % and 100 %; slowing
/// down takes 1.5x longer.
const HORN_TAU: (f32, f32) = (1.5, 0.08);
const DRUM_TAU: (f32, f32) = (5.0, 0.3);
const SLOW_DOWN: f32 = 1.5;

/// A rotor with inertia: its speed approaches the target exponentially.
#[derive(Clone, Copy, Default)]
struct Rotor {
    /// Turns, 0..1.
    angle: f32,
    /// Hz.
    rate: f32,
    target: f32,
    up: f32,
    down: f32,
}

impl Rotor {
    fn set_inertia(&mut self, tau_s: f32, sr: f32) {
        self.up = 1.0 - (-1.0 / (tau_s * sr)).exp();
        self.down = 1.0 - (-1.0 / (tau_s * SLOW_DOWN * sr)).exp();
    }

    #[inline]
    fn tick(&mut self, inv_sr: f32) -> f32 {
        let a = if self.target > self.rate { self.up } else { self.down };
        self.rate += (self.target - self.rate) * a;
        self.angle += self.rate * inv_sr;
        if self.angle >= 1.0 {
            self.angle -= 1.0;
        }
        self.angle
    }
}

/// Rotary Speaker (Leslie 122).
///
/// The mono input passes a little tube colour and an 800 Hz Linkwitz-Riley
/// crossover. The horn and the drum each read a short delay line whose
/// length follows the rotor angle (Doppler) and are amplitude-modulated by
/// their directivity; the horn also turns duller when facing away and adds
/// a cabinet reflection with the opposite Doppler shift. Two virtual mics
/// pick up each rotor for the stereo image. SPD CTL STOP/SLOW/FAST selects
/// the rotor speed (LO SPEED / HI SPEED for the horn, the drum a little
/// slower); the rotors get there with inertia set by HORN ACCEL and DRUM
/// ACCEL (the heavy drum takes several seconds), and STOP lets them coast to
/// a halt. DISTANCE moves the mics away: less modulation, a narrower image
/// and more reflection. BALANCE -100 is horn only, +100 drum only. MIX is the
/// device's own dry/wet balance.
pub(super) struct Rotary {
    sr: f32,
    horn_line: DelayLine,
    drum_line: DelayLine,
    xo_lp: [Svf; 2],
    xo_hp: [Svf; 2],
    /// Per mic: the horn's sound when facing away.
    dull: [OnePole; 2],
    horn: Rotor,
    drum: Rotor,
    horn_gain: Smoothed,
    drum_gain: Smoothed,
    mix: Smoothed,
    /// 0..1.
    dist: Smoothed,
    primed: bool,
}

impl Rotary {
    pub(super) fn new(ctx: FxContext) -> Self {
        let sr = ctx.sample_rate;
        let len = (0.001 * sr * (HORN_REFLECT_MS + HORN_RADIUS_MS + 1.0)) as usize + 8;
        let mut xo_lp = [Svf::default(); 2];
        let mut xo_hp = [Svf::default(); 2];
        for f in xo_lp.iter_mut().chain(xo_hp.iter_mut()) {
            f.set(CROSSOVER_HZ, FRAC_1_SQRT_2, sr);
        }
        let s = |v: f32, t: f32| Smoothed::new(v, t, sr);
        Self {
            sr,
            horn_line: DelayLine::new(len),
            drum_line: DelayLine::new(len),
            xo_lp,
            xo_hp,
            dull: [OnePole::new(2500.0, sr); 2],
            horn: Rotor::default(),
            drum: Rotor::default(),
            horn_gain: s(1.0, 30.0),
            drum_gain: s(1.0, 30.0),
            mix: s(1.0, 20.0),
            dist: s(0.5, 50.0),
            primed: false,
        }
    }

    fn smoothers(&mut self) -> [&mut Smoothed; 4] {
        [&mut self.horn_gain, &mut self.drum_gain, &mut self.mix, &mut self.dist]
    }

    fn snap_rotors(&mut self) {
        self.horn.rate = self.horn.target;
        self.drum.rate = self.drum.target;
    }
}

impl FxAlgo for Rotary {
    fn set_params(&mut self, p: &Params) {
        let sr = self.sr;
        let speed = match p.choice("sw") {
            0 => 0.0,
            1 => p.f("lo"),
            _ => p.f("hi"),
        };
        self.horn.target = speed;
        self.drum.target = speed * DRUM_RATIO;
        let tau = |(slow, fast): (f32, f32), accel: f32| slow * (fast / slow).powf(accel * 0.01);
        self.horn.set_inertia(tau(HORN_TAU, p.f("hac")), sr);
        self.drum.set_inertia(tau(DRUM_TAU, p.f("dac")), sr);
        let bal = p.f("bal") * 0.01;
        self.horn_gain.target = (1.0 - bal).min(1.0);
        self.drum_gain.target = (1.0 + bal).min(1.0);
        self.mix.target = p.f("mix") * 0.01;
        self.dist.target = p.f("dist") * 0.01;
        if !self.primed {
            // A newly created cabinet is already turning at its speed.
            self.primed = true;
            self.snap_rotors();
            for s in self.smoothers() {
                s.snap(s.target);
            }
        }
    }

    fn process(&mut self, left: &mut [f32], right: &mut [f32]) {
        let sr = self.sr;
        let inv_sr = 1.0 / sr;
        let (hb, hr, hx) = (0.001 * sr * HORN_BASE_MS, 0.001 * sr * HORN_RADIUS_MS, 0.001 * sr * HORN_REFLECT_MS);
        let (db, dr) = (0.001 * sr * DRUM_BASE_MS, 0.001 * sr * DRUM_RADIUS_MS);
        for (l, r) in left.iter_mut().zip(right.iter_mut()) {
            let (dl, drr) = (*l, *r);
            let m = 0.5 * (dl + drr);
            let x = 0.85 * m + 0.15 * fast_tanh(m);
            let lo = self.xo_lp[0].tick(x).lp;
            let lo = self.xo_lp[1].tick(lo).lp;
            let hi = self.xo_hp[0].tick(x).hp;
            let hi = self.xo_hp[1].tick(hi).hp;
            self.horn_line.push(hi);
            self.drum_line.push(lo);

            let ha = self.horn.tick(inv_sr);
            let da = self.drum.tick(inv_sr);
            let dist = self.dist.tick();
            let spread = 0.5 - 0.3 * dist;
            let am = 0.85 - 0.45 * dist;
            let am_drum = 0.6 * am;
            let refl = 0.2 + 0.3 * dist;
            let hg = self.horn_gain.tick();
            let dg = self.drum_gain.tick();

            let mut out = [0.0f32; 2];
            for (ch, o) in out.iter_mut().enumerate() {
                let mic = if ch == 0 { -0.5 * spread } else { 0.5 * spread };
                // Horn: facing the mic (c = 1) is closest, loudest and brightest.
                let c = (TAU * (ha - mic)).cos();
                let direct = self.horn_line.tap_hermite(hb - hr * c);
                let dull = self.dull[ch].lp(direct);
                let direct = dull + (direct - dull) * (0.5 + 0.5 * c);
                let reflect = self.horn_line.tap_hermite(hx + hr * c);
                let horn = direct * (1.0 - am * 0.5 * (1.0 - c)) + reflect * refl * (1.0 - am * 0.5 * (1.0 + c));
                // Drum: a rotating baffle, mostly amplitude modulation.
                let cd = (TAU * (da - mic)).cos();
                let drum = self.drum_line.tap_hermite(db - dr * cd) * (1.0 - am_drum * 0.5 * (1.0 - cd));
                *o = hg * horn + dg * drum;
            }
            let mix = self.mix.tick();
            *l = dl + (out[0] - dl) * mix;
            *r = drr + (out[1] - drr) * mix;
        }
    }

    fn reset(&mut self) {
        self.horn_line.clear();
        self.drum_line.clear();
        for f in self.xo_lp.iter_mut().chain(self.xo_hp.iter_mut()) {
            f.reset();
        }
        for f in &mut self.dull {
            f.reset();
        }
        self.horn.angle = 0.0;
        self.drum.angle = 0.0;
        self.snap_rotors();
        for s in self.smoothers() {
            s.snap(s.target);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::wing::{model_index, FX_MODELS, MAX_FX_PARAMS};

    #[test]
    fn rotors_accelerate_gradually() {
        let sr = 48_000.0;
        let spec = &FX_MODELS[model_index(FX_MODELS, "ROTARY").unwrap()];
        let mut v = [0.0; MAX_FX_PARAMS];
        spec.defaults_into(&mut v);
        let mut rot = Rotary::new(FxContext { sample_rate: sr, max_block: 480 });
        rot.set_params(&Params::new(spec, &v));
        let (slow, fast) = (v[spec.param_index("lo").unwrap()], v[spec.param_index("hi").unwrap()]);
        assert!((rot.horn.rate - slow).abs() < 1e-6, "starts at its speed");

        let run = |rot: &mut Rotary, secs: f32| {
            for _ in 0..(secs * 100.0) as usize {
                let mut l = vec![0.1; 480];
                let mut r = vec![0.1; 480];
                rot.process(&mut l, &mut r);
            }
        };
        v[spec.param_index("sw").unwrap()] = 2.0;
        rot.set_params(&Params::new(spec, &v));
        run(&mut rot, 0.1);
        assert!(rot.horn.rate < slow + 0.5 * (fast - slow), "horn jumped to {}", rot.horn.rate);
        run(&mut rot, 0.9);
        let drum_target = fast * DRUM_RATIO;
        let drum_slow = slow * DRUM_RATIO;
        assert!(rot.horn.rate > slow + 0.9 * (fast - slow), "horn too slow: {}", rot.horn.rate);
        assert!(rot.drum.rate < drum_slow + 0.75 * (drum_target - drum_slow), "the heavy drum lags: {}", rot.drum.rate);
        run(&mut rot, 6.0);
        assert!((rot.drum.rate - drum_target).abs() < 0.05 * drum_target, "drum: {}", rot.drum.rate);

        // STOP: the rotors coast down instead of stopping dead.
        v[spec.param_index("sw").unwrap()] = 0.0;
        rot.set_params(&Params::new(spec, &v));
        run(&mut rot, 0.1);
        assert!(rot.horn.rate > 0.5 * fast);
        run(&mut rot, 20.0);
        assert!(rot.horn.rate < 0.01 && rot.drum.rate < 0.05);
    }
}
