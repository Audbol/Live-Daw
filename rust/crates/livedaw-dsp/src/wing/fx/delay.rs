//! Delays: stereo, ultratap, tape, oilcan and BBD.
//!
//! Every delay returns the wet signal only (the FX slot applies FX MIX). The
//! digital delays (ST-DL, TAP-DL) move their taps by crossfading when TIME
//! changes; the analog ones (TAPE-DL, OILCAN, BBD-DL) glide like a tape
//! transport or a BBD clock, bending the pitch of the repeats. Feedback loops
//! pass a saturator, so even maximum feedback settles instead of running away.
//! The lo/hi cut filters and the loop limiter are shared with the modulation
//! devices (`modulation::shared`).

mod bbd;
mod oilcan;
mod stereo;
mod tape;
mod ultratap;

use super::blocks::DelayLine;
use super::{FxAlgo, FxContext};

/// The devices this module implements.
pub(super) const TOKENS: &[&str] = &["ST-DL", "TAP-DL", "TAPE-DL", "OILCAN", "BBD-DL"];

pub(super) fn create(token: &str, ctx: FxContext) -> Option<Box<dyn FxAlgo>> {
    if !TOKENS.contains(&token) {
        return None;
    }
    Some(match token {
        "ST-DL" => Box::new(stereo::StereoDelay::new(ctx)),
        "TAP-DL" => Box::new(ultratap::Ultratap::new(ctx)),
        "TAPE-DL" => Box::new(tape::TapeDelay::new(ctx)),
        "OILCAN" => Box::new(oilcan::Oilcan::new(ctx)),
        "BBD-DL" => Box::new(bbd::Bbd::new(ctx)),
        _ => return None,
    })
}

/// Smooth 0..1 crossfade curve.
#[inline]
fn smoothstep(t: f32) -> f32 {
    t * t * (3.0 - 2.0 * t)
}

/// A fixed delay tap that moves by crossfading: when its target changes, the
/// output fades from the old read position to the new one (no pitch bend and
/// no click), as digital delays do. A change that arrives during a fade
/// starts its own fade once the current one is done.
#[derive(Clone, Copy)]
struct XfadeTap {
    cur: usize,
    next: usize,
    target: usize,
    /// 0 = not fading, else the fade position.
    fade: f32,
    step: f32,
}

impl XfadeTap {
    fn new(fade_ms: f32, sr: f32) -> Self {
        Self { cur: 0, next: 0, target: 0, fade: 0.0, step: 1.0 / (fade_ms * 0.001 * sr).max(1.0) }
    }

    /// New delay in samples (as a `DelayLine::tap` index).
    fn set(&mut self, d: usize) {
        self.target = d;
    }

    /// Jumps to the target.
    fn snap(&mut self) {
        self.cur = self.target;
        self.next = self.target;
        self.fade = 0.0;
    }

    #[inline]
    fn read(&mut self, line: &DelayLine) -> f32 {
        if self.fade == 0.0 {
            if self.target == self.cur {
                return line.tap(self.cur);
            }
            self.next = self.target;
        }
        self.fade = (self.fade + self.step).min(1.0);
        let a = line.tap(self.cur);
        let y = a + (line.tap(self.next) - a) * smoothstep(self.fade);
        if self.fade >= 1.0 {
            self.cur = self.next;
            self.fade = 0.0;
        }
        y
    }
}

#[cfg(test)]
mod tests {
    use crate::wing::fx::{model_index, FxContext, FxDevice};
    use crate::wing::{FX_MODELS, MAX_FX_PARAMS};
    use std::f32::consts::TAU;

    #[test]
    fn devices() {
        for token in super::TOKENS {
            crate::wing::fx::tests::check_device(token);
        }
    }

    pub(super) const SR: f32 = 48_000.0;

    /// A device at 48 kHz with its defaults, plus `settings`.
    pub(super) fn device(token: &str, settings: &[(&str, f32)]) -> (FxDevice, [f32; MAX_FX_PARAMS]) {
        let model = model_index(token).unwrap();
        let mut v = [0.0; MAX_FX_PARAMS];
        FX_MODELS[model].defaults_into(&mut v);
        set(token, &mut v, settings);
        (FxDevice::new(model, FxContext { sample_rate: SR, max_block: 256 }).unwrap(), v)
    }

    pub(super) fn set(token: &str, v: &mut [f32; MAX_FX_PARAMS], settings: &[(&str, f32)]) {
        let spec = &FX_MODELS[model_index(token).unwrap()];
        for (id, x) in settings {
            v[spec.param_index(id).unwrap()] = *x;
        }
    }

    /// Runs `secs` of a unit impulse (both channels) through the device.
    pub(super) fn impulse(dev: &mut FxDevice, v: &[f32], secs: f32) -> (Vec<f32>, Vec<f32>) {
        let n = (secs * SR) as usize;
        let mut l = vec![0.0; n];
        let mut r = vec![0.0; n];
        l[0] = 1.0;
        r[0] = 1.0;
        for (cl, cr) in l.chunks_mut(256).zip(r.chunks_mut(256)) {
            dev.process(v, cl, cr);
        }
        (l, r)
    }

    /// Index and magnitude of the largest sample in `x[from..to]`.
    pub(super) fn peak(x: &[f32], from: usize, to: usize) -> (usize, f32) {
        x[from..to.min(x.len())]
            .iter()
            .enumerate()
            .fold((from, 0.0), |a, (i, v)| if v.abs() > a.1 { (from + i, v.abs()) } else { a })
    }

    /// Largest sample-to-sample step of the output for a 200 Hz sine while
    /// the settings change from `before` to `after` halfway through, relative
    /// to the output's peak (a clean sine moves at most 0.026 per sample).
    pub(super) fn max_step(token: &str, before: &[(&str, f32)], after: &[(&str, f32)]) -> f32 {
        let (mut dev, mut v) = device(token, before);
        let (mut worst, mut top) = (0.0f32, 0.0f32);
        let mut last = 0.0f32;
        for b in 0..(2 * 48_000 / 256) {
            if b == 48_000 / 256 {
                set(token, &mut v, after);
            }
            let mut l: Vec<f32> = (0..256).map(|i| 0.5 * (TAU * 200.0 * (b * 256 + i) as f32 / SR).sin()).collect();
            let mut r = l.clone();
            dev.process(&v, &mut l, &mut r);
            for x in l {
                if b > 48_000 / 512 {
                    worst = worst.max((x - last).abs());
                    top = top.max(x.abs());
                }
                last = x;
            }
        }
        worst / top.max(1e-9)
    }
}
