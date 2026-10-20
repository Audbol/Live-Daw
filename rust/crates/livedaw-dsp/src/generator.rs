//! The WING's oscillator (the OSC source group): a sine of any frequency,
//! pink noise or white noise at a level in dBFS.
//!
//! LEVEL is the sine's peak; the noises are set to the same RMS as that sine
//! (the level minus 3 dB), so switching the type keeps the loudness on the
//! meters about the same. White noise is uniform (a xorshift generator); pink
//! noise filters it with Paul Kellet's refined filter (public domain, -3 dB per
//! octave within +-0.05 dB from 10 Hz at 44.1 kHz), its poles moved for the
//! sample rate, and normalised to the RMS its response gives.

use crate::util::db_to_gain;
use std::f64::consts::TAU;

/// MODE (`/io/in/OSC/n/osc/mode`).
pub const GENERATOR_MODES: [&str; 3] = ["SINE", "PINK", "WHITE"];

pub mod generator_mode {
    pub const SINE: i32 = 0;
    pub const PINK: i32 = 1;
    pub const WHITE: i32 = 2;
}

/// LEVEL range in dBFS, and FREQ in Hz (the WING's).
pub const GENERATOR_LEVEL_MIN_DB: f32 = -40.0;
pub const GENERATOR_LEVEL_MAX_DB: f32 = -6.0;
pub const GENERATOR_FREQ_MIN_HZ: f32 = 20.0;
pub const GENERATOR_FREQ_MAX_HZ: f32 = 20000.0;

/// Kellet's filter at 44.1 kHz: six one-pole sections (pole, input gain), a
/// direct term and a one-sample-delayed term.
const KELLET_RATE: f64 = 44100.0;
const KELLET_POLES: [(f64, f64); 6] = [
    (0.99886, 0.0555179),
    (0.99332, 0.0750759),
    (0.96900, 0.1538520),
    (0.86650, 0.3104856),
    (0.55000, 0.5329522),
    (-0.7616, -0.0168980),
];
const KELLET_DIRECT: f64 = 0.5362;
const KELLET_DELAYED: f64 = 0.115926;

/// One oscillator channel. `process` is realtime-safe.
#[derive(Clone, Debug)]
pub struct SignalGenerator {
    sample_rate: f64,
    /// Sine phase in cycles (0..1).
    phase: f64,
    rng: u32,
    /// The pink filter at this rate, its state and the gain that makes its
    /// output's RMS that of the white noise feeding it.
    poles: [(f32, f32); 6],
    pink: [f32; 6],
    last_white: f32,
    pink_norm: f32,
    /// The amplitude of the last block (ramped to the new one).
    gain: f32,
}

impl SignalGenerator {
    /// `seed` picks the noise sequence (two channels with different seeds are uncorrelated).
    pub fn new(sample_rate: f64, seed: u32) -> Self {
        let mut g = Self {
            sample_rate: 0.0,
            phase: 0.0,
            rng: seed.max(1),
            poles: [(0.0, 0.0); 6],
            pink: [0.0; 6],
            last_white: 0.0,
            pink_norm: 1.0,
            gain: 0.0,
        };
        g.prepare(sample_rate);
        g
    }

    /// Sets the sample rate (not realtime: once per device configuration).
    pub fn prepare(&mut self, sample_rate: f64) {
        let sample_rate = if sample_rate > 0.0 { sample_rate } else { 48000.0 };
        self.sample_rate = sample_rate;
        // The low-frequency sections keep their corner frequencies: pole p
        // becomes p^(44.1k / rate) and the gain scales with the rate, so each
        // section's contribution in Hz is unchanged. The near-Nyquist
        // correction section stays as it is.
        let ratio = KELLET_RATE / sample_rate;
        let poles: [(f64, f64); 6] = std::array::from_fn(|i| match KELLET_POLES[i] {
            (p, g) if p > 0.0 => (p.powf(ratio), g * ratio),
            pg => pg,
        });
        // The output's variance for unit-variance white input: the sum of
        // the squared impulse response, h[0] = sum(g) + direct and
        // h[m] = sum(g p^m) + delayed * [m == 1].
        let h0: f64 = poles.iter().map(|(_, g)| g).sum::<f64>() + KELLET_DIRECT;
        let mut var = h0 * h0 + KELLET_DELAYED * KELLET_DELAYED;
        for &(pj, gj) in &poles {
            var += 2.0 * KELLET_DELAYED * gj * pj;
            for &(pk, gk) in &poles {
                var += gj * gk * pj * pk / (1.0 - pj * pk);
            }
        }
        self.poles = poles.map(|(p, g)| (p as f32, g as f32));
        self.pink_norm = (1.0 / var.sqrt()) as f32;
        self.reset();
    }

    /// Clears the filter state and the level ramp (the next block fades in).
    pub fn reset(&mut self) {
        self.pink = [0.0; 6];
        self.last_white = 0.0;
        self.gain = 0.0;
    }

    pub fn phase(&self) -> f64 {
        self.phase
    }

    /// Sets the sine phase (cycles), to run two channels in phase.
    pub fn set_phase(&mut self, phase: f64) {
        self.phase = phase.rem_euclid(1.0);
    }

    /// Uniform white noise in [-1, 1) (variance 1/3).
    #[inline]
    fn white(&mut self) -> f32 {
        let mut x = self.rng;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        self.rng = x;
        (x as i32) as f32 * (1.0 / 2_147_483_648.0)
    }

    /// Fills `out` with `mode` (generator_mode) at `freq_hz` and `level_db`.
    /// The amplitude ramps from the last block's; the sine's phase runs on
    /// across blocks and frequency changes.
    pub fn process(&mut self, out: &mut [f32], mode: i32, freq_hz: f32, level_db: f32) {
        if out.is_empty() {
            return;
        }
        let peak = db_to_gain(level_db.clamp(GENERATOR_LEVEL_MIN_DB, GENERATOR_LEVEL_MAX_DB));
        // The noises at the sine's RMS (peak / sqrt 2); uniform noise's RMS is 1 / sqrt 3.
        let target = match mode {
            generator_mode::PINK | generator_mode::WHITE => peak * (1.5f32).sqrt(),
            _ => peak,
        };
        let step = (target - self.gain) / out.len() as f32;
        let mut gain = self.gain;
        match mode {
            generator_mode::PINK => {
                // Pink normalised to the white noise's RMS.
                let norm = self.pink_norm;
                for x in out.iter_mut() {
                    let w = self.white();
                    let mut sum = KELLET_DIRECT as f32 * w + KELLET_DELAYED as f32 * self.last_white;
                    for (s, &(p, g)) in self.pink.iter_mut().zip(&self.poles) {
                        *s = p * *s + g * w;
                        sum += *s;
                    }
                    self.last_white = w;
                    gain += step;
                    *x = sum * norm * gain;
                }
            }
            generator_mode::WHITE => {
                for x in out.iter_mut() {
                    gain += step;
                    *x = self.white() * gain;
                }
            }
            _ => {
                let nyquist = (self.sample_rate * 0.49) as f32;
                let inc = freq_hz.clamp(GENERATOR_FREQ_MIN_HZ, GENERATOR_FREQ_MAX_HZ.min(nyquist)) as f64 / self.sample_rate;
                let mut phase = self.phase;
                for x in out.iter_mut() {
                    gain += step;
                    *x = (phase * TAU).sin() as f32 * gain;
                    phase += inc;
                    if phase >= 1.0 {
                        phase -= 1.0;
                    }
                }
                self.phase = phase;
            }
        }
        self.gain = target;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::biquad::{Biquad, BiquadCoeffs};

    const SR: f64 = 48000.0;

    fn render(g: &mut SignalGenerator, mode: i32, freq: f32, level: f32, n: usize) -> Vec<f32> {
        let mut out = vec![0.0; n];
        for chunk in out.chunks_mut(256) {
            g.process(chunk, mode, freq, level);
        }
        out
    }

    fn rms(x: &[f32]) -> f64 {
        (x.iter().map(|&v| v as f64 * v as f64).sum::<f64>() / x.len() as f64).sqrt()
    }

    fn db(x: f64) -> f64 {
        20.0 * x.log10()
    }

    #[test]
    fn sine_level_and_frequency() {
        for (freq, level) in [(1000.0f32, -20.0f32), (100.0, -6.0), (12345.0, -40.0)] {
            let mut g = SignalGenerator::new(SR, 1);
            let x = render(&mut g, generator_mode::SINE, freq, level, SR as usize);
            // Past the fade-in: the peak is the level, the RMS 3 dB below.
            let tail = &x[4800..];
            let peak = tail.iter().fold(0.0f32, |m, v| m.max(v.abs()));
            assert!((db(peak as f64) - level as f64).abs() < 0.05, "{freq} Hz: peak {} dB", db(peak as f64));
            assert!((db(rms(tail)) - (level as f64 - 3.0103)).abs() < 0.05);
            // Frequency from the positive-going zero crossings over one second.
            let crossings = x.windows(2).filter(|w| w[0] < 0.0 && w[1] >= 0.0).count() as f32;
            assert!((crossings - freq).abs() <= 1.0, "{freq} Hz: {crossings} crossings");
        }
    }

    #[test]
    fn noise_levels_match_the_sine_rms() {
        for mode in [generator_mode::PINK, generator_mode::WHITE] {
            for sr in [44100.0, 48000.0, 96000.0] {
                let mut g = SignalGenerator::new(sr, 7);
                let x = render(&mut g, mode, 1000.0, -20.0, sr as usize * 8);
                let level = db(rms(&x[4800..]));
                assert!((level - (-23.0103)).abs() < 0.3, "mode {mode} at {sr}: {level} dB RMS");
            }
        }
    }

    /// Power in octave bands (constant-Q band-pass filters), in dB.
    fn octave_bands(x: &[f32], sr: f64, centres: &[f64]) -> Vec<f64> {
        centres
            .iter()
            .map(|&f| {
                let mut bp = Biquad::default();
                bp.set_coefficients(BiquadCoeffs::band_pass(sr, f, std::f64::consts::SQRT_2));
                let mut y = x.to_vec();
                bp.process(&mut y, 0);
                10.0 * (y[4800..].iter().map(|&v| v as f64 * v as f64).sum::<f64>()).log10()
            })
            .collect()
    }

    #[test]
    fn pink_falls_3_db_per_octave_and_white_is_flat() {
        let centres = [63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0];
        for sr in [48000.0, 96000.0] {
            let mut g = SignalGenerator::new(sr, 3);
            let pink = render(&mut g, generator_mode::PINK, 1000.0, -20.0, sr as usize * 10);
            let white = render(&mut g, generator_mode::WHITE, 1000.0, -20.0, sr as usize * 10);
            // Constant-Q bands: white gains 3 dB per octave (flat density);
            // pink, measured against it, loses 3 dB per octave (its density).
            // The band-pass filters cramp near Nyquist, so white's own steps
            // are checked up to 2 kHz; the difference cancels that error.
            let p = octave_bands(&pink, sr, &centres);
            let w = octave_bands(&white, sr, &centres);
            for i in 1..centres.len() {
                let slope = (p[i] - w[i]) - (p[i - 1] - w[i - 1]);
                assert!((slope + 3.01).abs() < 0.5, "{sr}: pink {} -> {} Hz: {slope:.2} dB/oct", centres[i - 1], centres[i]);
                let white_step = w[i] - w[i - 1];
                if centres[i] <= 2000.0 {
                    assert!(
                        (white_step - 3.01).abs() < 0.3,
                        "{sr}: white {} -> {} Hz: {white_step:.2} dB",
                        centres[i - 1],
                        centres[i]
                    );
                }
            }
        }
    }

    #[test]
    fn level_changes_ramp_and_the_phase_runs_on() {
        let mut g = SignalGenerator::new(SR, 1);
        let mut a = vec![0.0; 64];
        g.process(&mut a, generator_mode::SINE, 1000.0, -6.0);
        // The first block fades in from silence.
        assert!(a[0].abs() < 0.05);
        let phase = g.phase();
        let mut b = vec![0.0; 64];
        g.process(&mut b, generator_mode::SINE, 1000.0, -6.0);
        assert!((phase - (64.0 * 1000.0 / SR).rem_euclid(1.0)).abs() < 1e-9);
        // Continuous across blocks (no jump bigger than one sample's slope).
        let max_slope = (TAU * 1000.0 / SR) as f32 * db_to_gain(-6.0) * 1.01;
        assert!((b[0] - a[63]).abs() <= max_slope);
        for v in [a, b].concat() {
            assert!(v.is_finite() && v.abs() <= db_to_gain(-6.0) + 1e-6);
        }
    }

    #[test]
    fn different_seeds_are_uncorrelated() {
        let mut a = SignalGenerator::new(SR, 1);
        let mut b = SignalGenerator::new(SR, 2);
        let x = render(&mut a, generator_mode::WHITE, 1000.0, -20.0, 48000);
        let y = render(&mut b, generator_mode::WHITE, 1000.0, -20.0, 48000);
        let c: f64 = x.iter().zip(&y).map(|(&p, &q)| p as f64 * q as f64).sum::<f64>() / (rms(&x) * rms(&y) * 48000.0);
        assert!(c.abs() < 0.03, "correlation {c}");
    }
}
