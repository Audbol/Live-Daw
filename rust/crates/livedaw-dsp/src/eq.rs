//! Four-band parametric EQ with click-free parameter smoothing.

use crate::biquad::{Biquad, BiquadCoeffs};

pub const NUM_BANDS: usize = 4;
/// Coefficients are recomputed every SUB_BLOCK samples while parameters glide.
pub const SUB_BLOCK: usize = 32;

#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BandType {
    Bell = 0,
    LowShelf = 1,
    HighShelf = 2,
    LowCut = 3,
    HighCut = 4,
}

impl BandType {
    pub fn from_i32(v: i32) -> Self {
        match v {
            1 => Self::LowShelf,
            2 => Self::HighShelf,
            3 => Self::LowCut,
            4 => Self::HighCut,
            _ => Self::Bell,
        }
    }

    pub fn is_cut(self) -> bool {
        matches!(self, Self::LowCut | Self::HighCut)
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct EqBand {
    pub band_type: BandType,
    pub frequency: f32,
    pub gain_db: f32,
    pub q: f32,
    pub enabled: bool,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct EqSettings {
    pub enabled: bool,
    pub bands: [EqBand; NUM_BANDS],
}

impl EqSettings {
    pub fn default_bands() -> [EqBand; NUM_BANDS] {
        [
            EqBand { band_type: BandType::LowShelf, frequency: 100.0, gain_db: 0.0, q: 0.707, enabled: true },
            EqBand { band_type: BandType::Bell, frequency: 400.0, gain_db: 0.0, q: 1.0, enabled: true },
            EqBand { band_type: BandType::Bell, frequency: 2500.0, gain_db: 0.0, q: 1.0, enabled: true },
            EqBand { band_type: BandType::HighShelf, frequency: 8000.0, gain_db: 0.0, q: 0.707, enabled: true },
        ]
    }
}

impl Default for EqSettings {
    fn default() -> Self {
        Self { enabled: true, bands: Self::default_bands() }
    }
}

#[derive(Clone, Copy, Debug)]
struct Smoothed {
    freq: f32,
    gain_db: f32,
    q: f32,
}

pub struct ParametricEq {
    sample_rate: f64,
    target: EqSettings,
    current: [Smoothed; NUM_BANDS],
    designed: [Option<EqBand>; NUM_BANDS],
    filters: [Biquad; NUM_BANDS],
    smoothing: f32,
    needs_snap: bool,
}

impl Default for ParametricEq {
    fn default() -> Self {
        let mut eq = Self {
            sample_rate: 48000.0,
            target: EqSettings::default(),
            current: [Smoothed { freq: 1000.0, gain_db: 0.0, q: 0.707 }; NUM_BANDS],
            designed: [None; NUM_BANDS],
            filters: Default::default(),
            smoothing: 0.0,
            needs_snap: true,
        };
        eq.prepare(48000.0);
        eq
    }
}

impl ParametricEq {
    pub fn prepare(&mut self, sample_rate: f64) {
        self.sample_rate = sample_rate;
        // ~20 ms glide, evaluated once per sub-block.
        self.smoothing = (-(SUB_BLOCK as f64) / (0.02 * sample_rate)).exp() as f32;
        self.reset();
    }

    pub fn reset(&mut self) {
        self.filters.iter_mut().for_each(Biquad::reset);
        self.needs_snap = true;
    }

    pub fn set_settings(&mut self, s: EqSettings) {
        self.target = s;
    }

    pub fn design_band(band: &EqBand, sr: f64) -> BiquadCoeffs {
        if !band.enabled {
            return BiquadCoeffs::identity();
        }
        let (f, q, g) = (band.frequency as f64, band.q as f64, band.gain_db as f64);
        match band.band_type {
            BandType::LowShelf => BiquadCoeffs::low_shelf(sr, f, q, g),
            BandType::HighShelf => BiquadCoeffs::high_shelf(sr, f, q, g),
            BandType::LowCut => BiquadCoeffs::high_pass(sr, f, q),
            BandType::HighCut => BiquadCoeffs::low_pass(sr, f, q),
            BandType::Bell => BiquadCoeffs::peak(sr, f, q, g),
        }
    }

    /// Combined magnitude response in dB at `freq`.
    pub fn response_db(settings: &EqSettings, freq: f64, sr: f64) -> f64 {
        if !settings.enabled {
            return 0.0;
        }
        let mag: f64 =
            settings.bands.iter().filter(|b| b.enabled).map(|b| Self::design_band(b, sr).magnitude_at(freq, sr)).product();
        20.0 * mag.max(1e-6).log10()
    }

    fn update_coefficients(&mut self, snap: bool) {
        let k = if snap { 0.0 } else { self.smoothing };
        for b in 0..NUM_BANDS {
            let t = self.target.bands[b];
            let c = &mut self.current[b];
            let tf = t.frequency.max(10.0);
            let tq = t.q.max(0.025);

            // Smooth frequency and Q in the log domain so sweeps sound even.
            c.freq = (tf.ln() + k * (c.freq.max(10.0).ln() - tf.ln())).exp();
            c.q = (tq.ln() + k * (c.q.max(0.025).ln() - tq.ln())).exp();
            c.gain_db = t.gain_db + k * (c.gain_db - t.gain_db);

            // Snap once close enough so a settled band stops being redesigned.
            if (c.freq - tf).abs() < 1e-4 * tf {
                c.freq = tf;
            }
            if (c.q - tq).abs() < 1e-4 {
                c.q = tq;
            }
            if (c.gain_db - t.gain_db).abs() < 1e-3 {
                c.gain_db = t.gain_db;
            }

            let smoothed = EqBand { frequency: c.freq, q: c.q, gain_db: c.gain_db, ..t };
            if !snap && self.designed[b] == Some(smoothed) {
                continue;
            }
            self.filters[b].set_coefficients(Self::design_band(&smoothed, self.sample_rate));
            self.designed[b] = Some(smoothed);
        }
    }

    /// Processes up to two channels in place.
    pub fn process(&mut self, channels: &mut [&mut [f32]]) {
        if !self.target.enabled {
            self.needs_snap = true;
            return;
        }

        let n = channels.first().map_or(0, |c| c.len());
        let mut start = 0;
        while start < n {
            let len = SUB_BLOCK.min(n - start);
            let snap = self.needs_snap;
            self.update_coefficients(snap);
            self.needs_snap = false;

            for b in 0..NUM_BANDS {
                let band = self.target.bands[b];
                if !band.enabled {
                    continue;
                }
                // A flat bell or shelf is an identity filter: skip it to save CPU.
                if !band.band_type.is_cut() && self.current[b].gain_db.abs() < 1e-3 && band.gain_db.abs() < 1e-3 {
                    self.filters[b].reset();
                    continue;
                }
                for (ch, data) in channels.iter_mut().take(2).enumerate() {
                    self.filters[b].process(&mut data[start..start + len], ch);
                }
            }
            start += len;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::util::gain_to_db;

    const SR: f64 = 48000.0;

    fn sine(freq: f64, amp: f32, n: usize) -> Vec<f32> {
        (0..n).map(|i| amp * (2.0 * std::f64::consts::PI * freq * i as f64 / SR).sin() as f32).collect()
    }

    fn rms(v: &[f32]) -> f32 {
        (v.iter().map(|x| (*x as f64) * (*x as f64)).sum::<f64>() / v.len() as f64).sqrt() as f32
    }

    fn measure(eq: &mut ParametricEq, freq: f64) -> f32 {
        eq.reset();
        let mut s = sine(freq, 0.25, 24000);
        let before = rms(&s[12000..]);
        for chunk in s.chunks_mut(256) {
            eq.process(&mut [chunk]);
        }
        gain_to_db(rms(&s[12000..]) / before)
    }

    #[test]
    fn flat_is_transparent() {
        let mut eq = ParametricEq::default();
        assert!(measure(&mut eq, 1000.0).abs() < 0.01);
    }

    #[test]
    fn bells_match_analytic_response() {
        let mut eq = ParametricEq::default();
        let mut s = EqSettings::default();
        s.bands[1] = EqBand { band_type: BandType::Bell, frequency: 1000.0, gain_db: 9.0, q: 1.0, enabled: true };
        s.bands[2] = EqBand { band_type: BandType::Bell, frequency: 5000.0, gain_db: -12.0, q: 2.0, enabled: true };
        eq.set_settings(s);
        for f in [1000.0, 5000.0] {
            let expected = ParametricEq::response_db(&s, f, SR) as f32;
            assert!((measure(&mut eq, f) - expected).abs() < 0.1);
        }
        assert!((ParametricEq::response_db(&s, 1000.0, SR) - 9.0).abs() < 0.5);
        assert!((ParametricEq::response_db(&s, 5000.0, SR) + 12.0).abs() < 0.5);
    }

    #[test]
    fn shelves() {
        let mut eq = ParametricEq::default();
        let mut s = EqSettings::default();
        s.bands[0].gain_db = 6.0;
        s.bands[3].gain_db = -6.0;
        eq.set_settings(s);
        assert!((measure(&mut eq, 30.0) - 6.0).abs() < 0.5);
        assert!(measure(&mut eq, 1000.0).abs() < 0.5);
        assert!((measure(&mut eq, 18000.0) + 6.0).abs() < 0.5);
    }

    #[test]
    fn disabled_bypasses() {
        let mut eq = ParametricEq::default();
        let mut s = EqSettings { enabled: false, ..Default::default() };
        s.bands[1].gain_db = 12.0;
        eq.set_settings(s);
        assert!(measure(&mut eq, 400.0).abs() < 0.01);
    }
}
