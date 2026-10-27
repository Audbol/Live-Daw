//! Modulation: dimension chorus, stereo chorus and flanger, phaser, tremolo/panner,
//! rotary speaker and the mood filter.
//!
//! The chorus family sweeps short delay lines (Hermite taps); the phaser
//! sweeps a chain of zero-delay-feedback allpasses and the mood filter a
//! ladder filter. CHORUS, FLANGER, PHASER, ROTARY and MOOD apply their own
//! MIX; DIMCRS mixes in the dry signal itself when its DRY switch is on, and
//! PANNER outputs the panned signal. Every continuous control is smoothed and
//! discrete ones (phaser stages, filter type) switch under a short fade.

mod chorus;
mod mood;
mod panner;
mod phaser;
mod rotary;
pub(super) mod shared;

use super::{FxAlgo, FxContext};

/// The devices this module implements.
pub(super) const TOKENS: &[&str] = &["DIMCRS", "CHORUS", "FLANGER", "PHASER", "PANNER", "ROTARY", "MOOD"];

pub(super) fn create(token: &str, ctx: FxContext) -> Option<Box<dyn FxAlgo>> {
    if !TOKENS.contains(&token) {
        return None;
    }
    Some(match token {
        "DIMCRS" => Box::new(chorus::Dimension::new(ctx)),
        "CHORUS" => Box::new(chorus::Chorus::new(ctx)),
        "FLANGER" => Box::new(chorus::Flanger::new(ctx)),
        "PHASER" => Box::new(phaser::Phaser::new(ctx)),
        "PANNER" => Box::new(panner::Panner::new(ctx)),
        "ROTARY" => Box::new(rotary::Rotary::new(ctx)),
        "MOOD" => Box::new(mood::Mood::new(ctx)),
        _ => return None,
    })
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

    /// A device at 48 kHz with its defaults, plus `settings`.
    fn device(token: &str, settings: &[(&str, f32)]) -> (FxDevice, [f32; MAX_FX_PARAMS]) {
        let model = model_index(token).unwrap();
        let spec = &FX_MODELS[model];
        let mut v = [0.0; MAX_FX_PARAMS];
        spec.defaults_into(&mut v);
        for (id, x) in settings {
            v[spec.param_index(id).unwrap()] = *x;
        }
        (FxDevice::new(model, FxContext { sample_rate: 48_000.0, max_block: 256 }).unwrap(), v)
    }

    #[test]
    fn dimension_bypass_and_wet() {
        // No switch pressed and DRY on: the signal passes untouched.
        let (mut dev, v) = device("DIMCRS", &[("sw1", 0.0), ("sw2", 0.0), ("sw3", 0.0), ("sw4", 0.0), ("drysw", 1.0)]);
        let input: Vec<f32> = (0..4800).map(|i| (i as f32 * 0.05).sin() * 0.5).collect();
        let (mut l, mut r) = (input.clone(), input.clone());
        dev.process(&v, &mut l, &mut r);
        assert!(l.iter().zip(&input).all(|(a, b)| (a - b).abs() < 1e-6));
        // A switch pressed and DRY off: only the delayed (wet) signal.
        let (mut dev, v) = device("DIMCRS", &[("drysw", 0.0)]);
        let mut l = vec![0.0; 4800];
        let mut r = vec![0.0; 4800];
        l[0] = 1.0;
        r[0] = 1.0;
        dev.process(&v, &mut l, &mut r);
        assert!(l[0].abs() < 1e-3, "no dry signal");
        let peak = l.iter().enumerate().fold((0, 0.0f32), |a, (i, x)| if x.abs() > a.1 { (i, x.abs()) } else { a });
        assert!((150..400).contains(&peak.0), "wet peak at {} samples", peak.0);
    }

    #[test]
    fn flanger_sweeps() {
        // The flanger's comb moves across a steady tone, so its level wobbles.
        let (mut dev, v) = device(
            "FLANGER",
            &[("spd", 2.0), ("feed", 0.0), ("dlyl", 2.0), ("dlyr", 3.0), ("depl", 60.0), ("lc", 20.0), ("hc", 20_000.0)],
        );
        let mut levels = Vec::new();
        for b in 0..200 {
            let mut l: Vec<f32> = (0..480).map(|i| 0.5 * (TAU * 1500.0 * (b * 480 + i) as f32 / 48_000.0).sin()).collect();
            let mut r = l.clone();
            dev.process(&v, &mut l, &mut r);
            levels.push(l.iter().map(|x| x * x).sum::<f32>().sqrt());
        }
        let (lo, hi) = levels[5..].iter().fold((f32::MAX, 0.0f32), |(a, b), &x| (a.min(x), b.max(x)));
        assert!(hi > 3.0 * lo, "{lo} .. {hi}");
    }

    #[test]
    fn chorus_mix_zero_is_dry() {
        let (mut dev, v) = device("CHORUS", &[("mix", 0.0)]);
        let input: Vec<f32> = (0..4800).map(|i| (i as f32 * 0.03).sin() * 0.5).collect();
        let (mut l, mut r) = (input.clone(), input.clone());
        dev.process(&v, &mut l, &mut r);
        assert!(l.iter().zip(&input).all(|(a, b)| (a - b).abs() < 1e-6));
    }
}
