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

    /// A peaking filter that keeps the analog bell's shape up to Nyquist
    /// (no cramping of high bells): Orfanidis' design with the analog gain at
    /// Nyquist, the band edges at the half-gain (dB) points `freq / q` apart.
    /// Falls back to [`Self::peak`] where the design doesn't apply (a bell too
    /// wide for the band, or no gain).
    pub fn peak_decramped(sr: f64, freq: f64, q: f64, gain_db: f64) -> Self {
        let freq = freq.clamp(10.0, sr * 0.49);
        let q = q.max(0.025);
        if gain_db.abs() < 1e-3 {
            return Self::peak(sr, freq, q, gain_db);
        }
        // The analog bell's upper half-gain edge: past Nyquist the band
        // can't be prescribed, so the bell is matched by its poles instead.
        let span = freq / q;
        let upper = 0.5 * (span + (span * span + 4.0 * freq * freq).sqrt());
        if upper >= 0.95 * sr * 0.5 {
            return Self::peak_matched(sr, freq, q, gain_db);
        }
        let w0 = 2.0 * PI * freq / sr;
        let dw = w0 / q;
        let g = 10f64.powf(gain_db / 20.0);
        let gb2 = g; // the half-gain level, squared
        let f = (g * g - gb2).abs();
        let g00 = (g * g - 1.0).abs();
        let f00 = (gb2 - 1.0).abs();
        let pi2 = PI * PI;
        let num = (w0 * w0 - pi2).powi(2) + g * g * f00 * pi2 * dw * dw / f;
        let den = (w0 * w0 - pi2).powi(2) + f00 * pi2 * dw * dw / f;
        let g1 = (num / den).sqrt();
        let g01 = (g * g - g1).abs();
        let g11 = (g * g - g1 * g1).abs();
        let f01 = (gb2 - g1).abs();
        let f11 = (gb2 - g1 * g1).abs();
        if f11 < 1e-12 || g00 < 1e-12 {
            return Self::peak(sr, freq, q, gain_db);
        }
        let w2 = (g11 / g00).sqrt() * (w0 / 2.0).tan().powi(2);
        let dww = (1.0 + (f00 / f11).sqrt() * w2) * (dw / 2.0).tan();
        let c = f11 * dww * dww - 2.0 * w2 * (f01 - (f00 * f11).sqrt());
        let d = 2.0 * w2 * (g01 - (g00 * g11).sqrt());
        let a = ((c + d) / f).sqrt();
        let b = ((g * g * c + gb2 * d) / f).sqrt();
        if !(a.is_finite() && b.is_finite()) {
            return Self::peak(sr, freq, q, gain_db);
        }
        normalise(g1 + w2 + b, -2.0 * (g1 - w2), g1 - b + w2, 1.0 + w2 + a, -2.0 * (1.0 - w2), 1.0 + w2 - a)
    }

    /// Vicanek's matched peaking filter: the analog bell's poles by impulse
    /// invariance, the zeros matching its magnitude at DC, the centre and
    /// Nyquist. For bells reaching past Nyquist.
    fn peak_matched(sr: f64, freq: f64, q: f64, gain_db: f64) -> Self {
        let g = 10f64.powf(gain_db / 20.0);
        let w0 = 2.0 * PI * freq / sr;
        let zeta = 1.0 / (2.0 * q * g.sqrt());
        let a1 = if zeta <= 1.0 {
            -2.0 * (-zeta * w0).exp() * ((1.0 - zeta * zeta).sqrt() * w0).cos()
        } else {
            -2.0 * (-zeta * w0).exp() * ((zeta * zeta - 1.0).sqrt() * w0).cosh()
        };
        let a2 = (-2.0 * zeta * w0).exp();
        let (aa0, aa1, aa2) = ((1.0 + a1 + a2).powi(2), (1.0 - a1 + a2).powi(2), -4.0 * a2);
        let p1 = (w0 / 2.0).sin().powi(2);
        let p0 = 1.0 - p1;
        let p2 = 4.0 * p0 * p1;
        let r1 = (aa0 * p0 + aa1 * p1 + aa2 * p2) * g * g;
        let r2 = (-aa0 + aa1 + 4.0 * (p0 - p1) * aa2) * g * g;
        let bb0 = aa0;
        let bb2 = (r1 - r2 * p1 - bb0) / (4.0 * p1 * p1);
        let bb1 = r2 + bb0 + 4.0 * (p1 - p0) * bb2;
        let (s0, s1) = (bb0.sqrt(), bb1.max(0.0).sqrt());
        let w = 0.5 * (s0 + s1);
        let b0 = 0.5 * (w + (w * w + bb2).max(0.0).sqrt());
        let c = BiquadCoeffs { b0, b1: 0.5 * (s0 - s1), b2: -bb2 / (4.0 * b0), a1, a2 };
        if [c.b0, c.b1, c.b2, c.a1, c.a2].iter().all(|v| v.is_finite()) {
            c
        } else {
            Self::peak(sr, freq, q, gain_db)
        }
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
    fn decramped_peak_keeps_the_analog_bell() {
        // A 12 kHz +6 dB bell (Q 1.413): a WING measured +2.35 dB at 18 kHz
        // and +1.6 dB at 20 kHz, the analog bell's levels; the bilinear
        // design falls well short.
        let db = |c: BiquadCoeffs, f: f64| gain_to_db(c.magnitude_at(f, 48000.0) as f32);
        let c = BiquadCoeffs::peak_decramped(48000.0, 12000.0, 1.413, 6.0);
        assert!((db(c, 12000.0) - 6.0).abs() < 0.05);
        assert!((db(c, 18000.0) - 2.35).abs() < 0.1 && (db(c, 20000.0) - 1.6).abs() < 0.1);
        assert!(db(BiquadCoeffs::peak(48000.0, 12000.0, 1.413, 6.0), 20000.0) < 1.0);
        // Low bells and cuts are the ordinary peaking filter.
        let (a, b) = (BiquadCoeffs::peak_decramped(48000.0, 1500.0, 2.38, -9.0), BiquadCoeffs::peak(48000.0, 1500.0, 2.38, -9.0));
        for f in [500.0, 1500.0, 3000.0] {
            assert!((db(a, f) - db(b, f)).abs() < 0.05);
        }
        assert_eq!(BiquadCoeffs::peak_decramped(48000.0, 1000.0, 1.0, 0.0), BiquadCoeffs::peak(48000.0, 1000.0, 1.0, 0.0));
        // A 15 kHz -6 dB cut (Q 0.707) reaches past Nyquist: a WING measured
        // -3.6 dB at 8 kHz, -6.0 at 15 kHz and -4.8 at 20 kHz.
        let c = BiquadCoeffs::peak_decramped(48000.0, 15000.0, 0.7065, -6.0);
        assert!((db(c, 15000.0) + 6.0).abs() < 0.05);
        assert!((db(c, 8000.0) + 3.6).abs() < 0.7 && (db(c, 20000.0) + 4.8).abs() < 0.7);
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
