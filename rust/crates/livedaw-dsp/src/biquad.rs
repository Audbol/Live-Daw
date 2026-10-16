//! Biquad filters (RBJ Audio EQ Cookbook designs, transposed direct form II).

use std::f64::consts::PI;

/// Normalised biquad coefficients (a0 == 1).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct BiquadCoeffs {
    pub b0: f64,
    pub b1: f64,
    pub b2: f64,
    pub a1: f64,
    pub a2: f64,
}

impl Default for BiquadCoeffs {
    fn default() -> Self {
        Self::identity()
    }
}

struct Prewarp {
    cosw: f64,
    alpha: f64,
}

fn prewarp(sample_rate: f64, freq: f64, q: f64) -> Prewarp {
    let freq = freq.clamp(10.0, sample_rate * 0.49);
    let q = q.max(0.025);
    let w0 = 2.0 * PI * freq / sample_rate;
    Prewarp { cosw: w0.cos(), alpha: w0.sin() / (2.0 * q) }
}

fn normalise(b0: f64, b1: f64, b2: f64, a0: f64, a1: f64, a2: f64) -> BiquadCoeffs {
    BiquadCoeffs { b0: b0 / a0, b1: b1 / a0, b2: b2 / a0, a1: a1 / a0, a2: a2 / a0 }
}

impl BiquadCoeffs {
    pub const fn identity() -> Self {
        Self { b0: 1.0, b1: 0.0, b2: 0.0, a1: 0.0, a2: 0.0 }
    }

    pub fn peak(sr: f64, freq: f64, q: f64, gain_db: f64) -> Self {
        let p = prewarp(sr, freq, q);
        let a = 10f64.powf(gain_db / 40.0);
        normalise(1.0 + p.alpha * a, -2.0 * p.cosw, 1.0 - p.alpha * a, 1.0 + p.alpha / a, -2.0 * p.cosw, 1.0 - p.alpha / a)
    }

    pub fn low_shelf(sr: f64, freq: f64, q: f64, gain_db: f64) -> Self {
        let p = prewarp(sr, freq, q);
        let a = 10f64.powf(gain_db / 40.0);
        let t = 2.0 * a.sqrt() * p.alpha;
        normalise(
            a * ((a + 1.0) - (a - 1.0) * p.cosw + t),
            2.0 * a * ((a - 1.0) - (a + 1.0) * p.cosw),
            a * ((a + 1.0) - (a - 1.0) * p.cosw - t),
            (a + 1.0) + (a - 1.0) * p.cosw + t,
            -2.0 * ((a - 1.0) + (a + 1.0) * p.cosw),
            (a + 1.0) + (a - 1.0) * p.cosw - t,
        )
    }

    pub fn high_shelf(sr: f64, freq: f64, q: f64, gain_db: f64) -> Self {
        let p = prewarp(sr, freq, q);
        let a = 10f64.powf(gain_db / 40.0);
        let t = 2.0 * a.sqrt() * p.alpha;
        normalise(
            a * ((a + 1.0) + (a - 1.0) * p.cosw + t),
            -2.0 * a * ((a - 1.0) + (a + 1.0) * p.cosw),
            a * ((a + 1.0) + (a - 1.0) * p.cosw - t),
            (a + 1.0) - (a - 1.0) * p.cosw + t,
            2.0 * ((a - 1.0) - (a + 1.0) * p.cosw),
            (a + 1.0) - (a - 1.0) * p.cosw - t,
        )
    }

    pub fn high_pass(sr: f64, freq: f64, q: f64) -> Self {
        let p = prewarp(sr, freq, q);
        normalise((1.0 + p.cosw) * 0.5, -(1.0 + p.cosw), (1.0 + p.cosw) * 0.5, 1.0 + p.alpha, -2.0 * p.cosw, 1.0 - p.alpha)
    }

    pub fn low_pass(sr: f64, freq: f64, q: f64) -> Self {
        let p = prewarp(sr, freq, q);
        normalise((1.0 - p.cosw) * 0.5, 1.0 - p.cosw, (1.0 - p.cosw) * 0.5, 1.0 + p.alpha, -2.0 * p.cosw, 1.0 - p.alpha)
    }

    /// First-order low pass (bilinear, 6 dB/oct).
    pub fn low_pass_1(sr: f64, freq: f64) -> Self {
        let k = (PI * freq.clamp(10.0, sr * 0.49) / sr).tan();
        normalise(k, k, 0.0, k + 1.0, k - 1.0, 0.0)
    }

    /// First-order high pass (bilinear, 6 dB/oct).
    pub fn high_pass_1(sr: f64, freq: f64) -> Self {
        let k = (PI * freq.clamp(10.0, sr * 0.49) / sr).tan();
        normalise(1.0, -1.0, 0.0, k + 1.0, k - 1.0, 0.0)
    }

    /// First-order all pass (90 degrees at `freq`).
    pub fn all_pass_1(sr: f64, freq: f64) -> Self {
        let k = (PI * freq.clamp(10.0, sr * 0.49) / sr).tan();
        let a = (k - 1.0) / (k + 1.0);
        Self { b0: a, b1: 1.0, b2: 0.0, a1: a, a2: 0.0 }
    }

    /// Second-order all pass (180 degrees at `freq`).
    pub fn all_pass_2(sr: f64, freq: f64, q: f64) -> Self {
        let p = prewarp(sr, freq, q);
        normalise(1.0 - p.alpha, -2.0 * p.cosw, 1.0 + p.alpha, 1.0 + p.alpha, -2.0 * p.cosw, 1.0 - p.alpha)
    }

    /// Band pass with 0 dB peak gain.
    pub fn band_pass(sr: f64, freq: f64, q: f64) -> Self {
        let p = prewarp(sr, freq, q);
        normalise(p.alpha, 0.0, -p.alpha, 1.0 + p.alpha, -2.0 * p.cosw, 1.0 - p.alpha)
    }

    pub fn notch(sr: f64, freq: f64, q: f64) -> Self {
        let p = prewarp(sr, freq, q);
        normalise(1.0, -2.0 * p.cosw, 1.0, 1.0 + p.alpha, -2.0 * p.cosw, 1.0 - p.alpha)
    }

    /// Linear magnitude response at `freq` (used to draw EQ curves).
    pub fn magnitude_at(&self, freq: f64, sample_rate: f64) -> f64 {
        let w = 2.0 * PI * freq / sample_rate;
        let (c1, s1) = (w.cos(), -w.sin()); // z^-1
        let (c2, s2) = ((2.0 * w).cos(), -(2.0 * w).sin()); // z^-2
        let num_re = self.b0 + self.b1 * c1 + self.b2 * c2;
        let num_im = self.b1 * s1 + self.b2 * s2;
        let den_re = 1.0 + self.a1 * c1 + self.a2 * c2;
        let den_im = self.a1 * s1 + self.a2 * s2;
        ((num_re * num_re + num_im * num_im) / (den_re * den_re + den_im * den_im)).sqrt()
    }
}

/// Biquad with state for up to two channels.
#[derive(Clone, Debug, Default)]
pub struct Biquad {
    coeffs: BiquadCoeffs,
    z1: [f64; 2],
    z2: [f64; 2],
}

impl Biquad {
    pub fn set_coefficients(&mut self, c: BiquadCoeffs) {
        self.coeffs = c;
    }

    pub fn coefficients(&self) -> &BiquadCoeffs {
        &self.coeffs
    }

    pub fn reset(&mut self) {
        self.z1 = [0.0; 2];
        self.z2 = [0.0; 2];
    }

    /// Filters one sample of `channel` (for per-sample sidechains and envelopes).
    #[inline]
    pub fn tick(&mut self, x: f32, channel: usize) -> f32 {
        let c = self.coeffs;
        let ch = channel.min(1);
        let input = x as f64;
        let out = c.b0 * input + self.z1[ch];
        self.z1[ch] = c.b1 * input - c.a1 * out + self.z2[ch];
        self.z2[ch] = c.b2 * input - c.a2 * out;
        out as f32
    }

    #[inline]
    pub fn process(&mut self, data: &mut [f32], channel: usize) {
        let c = self.coeffs;
        let ch = channel.min(1);
        let (mut z1, mut z2) = (self.z1[ch], self.z2[ch]);
        for x in data.iter_mut() {
            let input = *x as f64;
            let out = c.b0 * input + z1;
            z1 = c.b1 * input - c.a1 * out + z2;
            z2 = c.b2 * input - c.a2 * out;
            *x = out as f32;
        }
        self.z1[ch] = z1;
        self.z2[ch] = z2;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::util::gain_to_db;

    #[test]
    fn designs_hit_requested_gain() {
        let c = BiquadCoeffs::peak(48000.0, 1000.0, 1.0, 6.0);
        assert!((gain_to_db(c.magnitude_at(1000.0, 48000.0) as f32) - 6.0).abs() < 0.05);
        let hp = BiquadCoeffs::high_pass(48000.0, 100.0, std::f64::consts::FRAC_1_SQRT_2);
        assert!((gain_to_db(hp.magnitude_at(100.0, 48000.0) as f32) + 3.01).abs() < 0.05);
        assert!(hp.magnitude_at(20.0, 48000.0) < 0.05);
    }

    #[test]
    fn first_order_and_all_pass_designs() {
        let sr = 48000.0;
        let lp = BiquadCoeffs::low_pass_1(sr, 1000.0);
        assert!((gain_to_db(lp.magnitude_at(1000.0, sr) as f32) + 3.01).abs() < 0.05);
        let hp = BiquadCoeffs::high_pass_1(sr, 1000.0);
        assert!((gain_to_db(hp.magnitude_at(1000.0, sr) as f32) + 3.01).abs() < 0.05);
        for c in [BiquadCoeffs::all_pass_1(sr, 500.0), BiquadCoeffs::all_pass_2(sr, 2000.0, 0.7)] {
            for f in [50.0, 500.0, 2000.0, 15000.0] {
                assert!((c.magnitude_at(f, sr) - 1.0).abs() < 1e-6);
            }
        }
        assert!((BiquadCoeffs::band_pass(sr, 3000.0, 2.0).magnitude_at(3000.0, sr) - 1.0).abs() < 1e-6);
        assert!(BiquadCoeffs::notch(sr, 3000.0, 2.0).magnitude_at(3000.0, sr) < 1e-6);
    }
}
