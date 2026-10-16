//! Helpers shared by the modulation and delay devices: the LO CUT / HI CUT
//! filter pair, a feedback-loop limiter, delay-time glide, the fade used to
//! switch discrete settings without clicks and the LFO shapes.

use crate::wing::fx::blocks::{coef, fast_tanh, shape, Svf, Wave};
use std::f32::consts::FRAC_1_SQRT_2;

/// Milliseconds to (fractional) samples, without the 1-sample floor of
/// `blocks::ms_to_samples` (modulation depths may be 0).
#[inline]
pub(crate) fn ms(ms: f32, sr: f32) -> f32 {
    ms * 0.001 * sr
}

/// Advances a 0..1 phase by `inc` (< 1).
#[inline]
pub(crate) fn advance(phase: &mut f32, inc: f32) {
    *phase += inc;
    if *phase >= 1.0 {
        *phase -= 1.0;
    }
}

/// A triangle in phase with `sin(2 pi phase)` (zero at 0, peak at 0.25), -1..1.
#[inline]
pub(crate) fn triangle(phase: f32) -> f32 {
    shape(Wave::Triangle, phase + 0.25)
}

/// Triangle blended into sine, `tri` 0 = sine .. 1 = triangle (the chorus WAVE).
#[inline]
pub(crate) fn sine_tri(phase: f32, tri: f32) -> f32 {
    let s = shape(Wave::Sine, phase);
    s + (triangle(phase) - s) * tri
}

/// The SHAPE control of the phaser and the tremolo/panner, -1..1 over one
/// cycle: `morph` -1 = triangle, 0 = sine, 1 = square. The square is a
/// saturated sine with rounded edges, so a sweep or a gain never jumps.
#[inline]
pub(crate) fn tri_sine_square(phase: f32, morph: f32) -> f32 {
    if morph <= 0.0 {
        sine_tri(phase, (-morph).min(1.0))
    } else {
        let k = 0.01 + 16.0 * morph.min(1.0);
        fast_tanh(k * shape(Wave::Sine, phase)) / fast_tanh(k)
    }
}

/// Feedback-loop limiter: transparent up to 1.0, then saturating smoothly
/// towards about 2.0, so 100 % feedback settles instead of running away.
#[inline]
pub(crate) fn loop_limit(x: f32) -> f32 {
    let a = x.abs();
    if a <= 1.0 {
        x
    } else {
        (1.0 + fast_tanh(a - 1.0)).copysign(x)
    }
}

/// Stereo LO CUT / HI CUT pair (12 dB/oct Butterworth state-variable filters).
/// Cutoff changes glide over ~30 ms, updated once per block, so moving a
/// control never clicks.
#[derive(Clone, Copy)]
pub(crate) struct CutFilter {
    hp: [Svf; 2],
    lp: [Svf; 2],
    lc: f32,
    hc: f32,
    lc_target: f32,
    hc_target: f32,
    sr: f32,
    primed: bool,
}

impl CutFilter {
    pub(crate) fn new(sr: f32) -> Self {
        let mut f = Self {
            hp: [Svf::default(); 2],
            lp: [Svf::default(); 2],
            lc: 20.0,
            hc: 20_000.0,
            lc_target: 20.0,
            hc_target: 20_000.0,
            sr,
            primed: false,
        };
        f.apply();
        f
    }

    /// New cutoffs in Hz (the first call jumps straight there).
    pub(crate) fn set(&mut self, lc: f32, hc: f32) {
        self.lc_target = lc.clamp(5.0, self.sr * 0.45);
        self.hc_target = hc.clamp(5.0, self.sr * 0.45);
        if !self.primed {
            self.primed = true;
            self.snap();
        }
    }

    fn snap(&mut self) {
        self.lc = self.lc_target;
        self.hc = self.hc_target;
        self.apply();
    }

    fn apply(&mut self) {
        for f in &mut self.hp {
            f.set(self.lc, FRAC_1_SQRT_2, self.sr);
        }
        for f in &mut self.lp {
            f.set(self.hc, FRAC_1_SQRT_2, self.sr);
        }
    }

    /// Moves the cutoffs towards their targets; call once per block of `n` samples.
    pub(crate) fn update(&mut self, n: usize) {
        if self.lc == self.lc_target && self.hc == self.hc_target {
            return;
        }
        let a = 1.0 - (-(n as f32) / (0.03 * self.sr)).exp();
        self.lc *= (self.lc_target / self.lc).powf(a);
        self.hc *= (self.hc_target / self.hc).powf(a);
        if (self.lc / self.lc_target - 1.0).abs() < 1e-3 {
            self.lc = self.lc_target;
        }
        if (self.hc / self.hc_target - 1.0).abs() < 1e-3 {
            self.hc = self.hc_target;
        }
        self.apply();
    }

    /// Filters one sample of channel `ch` (0 = left, 1 = right).
    #[inline]
    pub(crate) fn tick(&mut self, ch: usize, x: f32) -> f32 {
        let h = self.hp[ch & 1].tick(x).hp;
        self.lp[ch & 1].tick(h).lp
    }

    pub(crate) fn reset(&mut self) {
        for f in self.hp.iter_mut().chain(self.lp.iter_mut()) {
            f.reset();
        }
        self.snap();
    }
}

/// A delay time (in samples) that glides like a tape transport or a BBD clock:
/// a one-pole approach whose slope is capped at `max_step` samples per sample,
/// so the pitch bend while the time moves stays within 1 +- `max_step`.
#[derive(Clone, Copy, Default)]
pub(crate) struct Glide {
    pub value: f32,
    pub target: f32,
    a: f32,
    max_step: f32,
}

impl Glide {
    pub(crate) fn new(ms: f32, max_step: f32, sr: f32) -> Self {
        Self { value: 0.0, target: 0.0, a: coef(ms, sr), max_step }
    }

    pub(crate) fn snap(&mut self) {
        self.value = self.target;
    }

    #[inline]
    pub(crate) fn tick(&mut self) -> f32 {
        self.value += ((self.target - self.value) * self.a).clamp(-self.max_step, self.max_step);
        self.value
    }
}

/// Switches a discrete setting (mode, stage count, filter type...) without a
/// click: the affected signal fades out, the change is applied at silence and
/// the signal fades back in.
#[derive(Clone, Copy)]
pub(crate) struct Morph {
    gain: f32,
    step: f32,
    pending: bool,
}

impl Morph {
    /// `fade_ms` each way.
    pub(crate) fn new(fade_ms: f32, sr: f32) -> Self {
        Self { gain: 1.0, step: 1.0 / ms(fade_ms, sr).max(1.0), pending: false }
    }

    /// Starts (or continues) a fade-out; [`Morph::tick`] reports when to apply.
    pub(crate) fn request(&mut self) {
        self.pending = true;
    }

    /// The gain to apply to the affected signal.
    #[inline]
    pub(crate) fn gain(&self) -> f32 {
        self.gain
    }

    /// Advances one sample; true exactly when the pending change should be applied.
    #[inline]
    pub(crate) fn tick(&mut self) -> bool {
        if self.pending {
            self.gain -= self.step;
            if self.gain <= 0.0 {
                self.gain = 0.0;
                self.pending = false;
                return true;
            }
        } else if self.gain < 1.0 {
            self.gain = (self.gain + self.step).min(1.0);
        }
        false
    }

    pub(crate) fn reset(&mut self) {
        self.gain = 1.0;
        self.pending = false;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn shapes_and_limiter() {
        // The triangle is in phase with the sine, so the blends keep their peaks.
        assert!(triangle(0.0).abs() < 1e-6 && (triangle(0.25) - 1.0).abs() < 1e-6);
        assert!((triangle(0.75) + 1.0).abs() < 1e-6);
        for i in 0..1000 {
            let p = i as f32 / 1000.0;
            for m in [-1.0, -0.3, 0.0, 0.7, 1.0] {
                assert!((-1.0..=1.0).contains(&tri_sine_square(p, m)));
            }
        }
        // SHAPE: -1 triangle, 0 sine, +1 a (rounded) square.
        assert!((tri_sine_square(0.1, -1.0) - triangle(0.1)).abs() < 1e-6);
        assert!((tri_sine_square(0.1, 0.0) - (0.2 * std::f32::consts::PI).sin()).abs() < 1e-3);
        assert!(tri_sine_square(0.1, 1.0) > 0.99 && tri_sine_square(0.6, 1.0) < -0.99);
        assert!((sine_tri(0.3, 1.0) - triangle(0.3)).abs() < 1e-6);
        assert_eq!(loop_limit(0.5), 0.5);
        assert!(loop_limit(100.0) <= 2.05 && loop_limit(-100.0) >= -2.05);
        assert!((loop_limit(1.001) - 1.001).abs() < 1e-4);
    }

    #[test]
    fn glide_and_morph() {
        let mut g = Glide::new(100.0, 0.5, 48_000.0);
        g.target = 1000.0;
        g.tick();
        assert!(g.value <= 0.5);
        let mut m = Morph::new(1.0, 48_000.0);
        m.request();
        let applied = (0..100).position(|_| m.tick());
        assert!(applied.is_some_and(|i| (46..=48).contains(&i)), "{applied:?}");
        for _ in 0..100 {
            m.tick();
        }
        assert_eq!(m.gain(), 1.0);
    }
}
