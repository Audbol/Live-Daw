//! Level detectors for the devices' screens ([`FxAlgo::readouts`](super::FxAlgo::readouts)).
//!
//! The screen polls a device's readouts a few dozen times a second, long
//! after most audio blocks have gone by, so a level readout must not be the
//! last block's peak: it is a peak that rises at once and falls at the app's
//! meter rate ([`FALL_DB_PER_S`], the 60 dB/s of the UI's meter ballistics),
//! so whatever the screen samples is what a meter fed every sample would
//! show. VU readouts integrate the signal's power over the VU's 300 ms
//! instead. All levels are linear (0 = silence: the idle value of every
//! readout is 0).

use super::blocks::{coef, Biquad, BiquadCoeffs};

/// How fast a level readout falls, dB per second (the UI's meter ballistics).
pub const FALL_DB_PER_S: f32 = 60.0;

/// A peak level: instant rise, [`FALL_DB_PER_S`] fall.
#[derive(Clone, Copy, Debug)]
pub struct PeakMeter {
    level: f32,
    /// Fall per sample (a gain < 1).
    fall: f32,
}

impl PeakMeter {
    pub fn new(sr: f32) -> Self {
        Self { level: 0.0, fall: 10f32.powf(-FALL_DB_PER_S / 20.0 / sr.max(1.0)) }
    }

    /// One sample.
    #[inline]
    pub fn tick(&mut self, x: f32) {
        self.level = (self.level * self.fall).max(x.abs());
    }

    /// A block at once: its peak against the level fallen over its length
    /// (the same result as ticking each sample, give or take where in the
    /// block the peak sits).
    pub fn block(&mut self, x: &[f32]) {
        let peak = x.iter().fold(0.0f32, |a, v| a.max(v.abs()));
        self.level = (self.level * self.fall.powi(x.len() as i32)).max(peak);
        if self.level < 1e-9 {
            self.level = 0.0;
        }
    }

    /// The level (linear peak).
    pub fn level(&self) -> f32 {
        self.level
    }

    pub fn reset(&mut self) {
        self.level = 0.0;
    }
}

/// A VU level: the RMS of the signal over the VU's integration time (300 ms
/// to 99 %), relative to the level that reads 0 VU.
#[derive(Clone, Copy, Debug)]
pub struct VuMeter {
    ms: f32,
    coef: f32,
    /// Mean square that reads 0 VU.
    zero: f32,
}

impl VuMeter {
    /// `zero_dbfs`: the RMS level (dBFS) that reads 0 VU.
    pub fn new(sr: f32, zero_dbfs: f32) -> Self {
        // A first-order integrator reaching 99 % in 300 ms: tau = 300 / ln(100).
        Self { ms: 0.0, coef: coef(300.0 / 4.605, sr), zero: 10f32.powf(zero_dbfs / 10.0) }
    }

    #[inline]
    pub fn tick(&mut self, x: f32) {
        self.ms += (x * x - self.ms) * self.coef;
    }

    /// The level relative to 0 VU (linear: 1 = 0 VU, 0 = silence).
    pub fn level(&self) -> f32 {
        if self.ms < 1e-14 {
            0.0
        } else {
            (self.ms / self.zero).sqrt()
        }
    }

    pub fn reset(&mut self) {
        self.ms = 0.0;
    }
}

/// Bands of [`Spectrum`]: third octaves from 25 Hz to 20 kHz.
pub const SPECTRUM_BANDS: usize = 30;

/// The centre frequency of spectrum band `k` (third octaves round 1 kHz).
pub fn spectrum_hz(k: usize) -> f32 {
    1000.0 * 2f32.powf((k as f32 - 16.0) / 3.0)
}

/// A third-octave analyser of a mono signal: a band-pass per band and a
/// [`PeakMeter`] on each (for the spectrum a screen draws behind its bands).
/// Bands above the Nyquist range read 0.
pub struct Spectrum {
    filters: [Biquad; SPECTRUM_BANDS],
    meters: [PeakMeter; SPECTRUM_BANDS],
    bands: usize,
}

impl Spectrum {
    pub fn new(sr: f32) -> Self {
        let mut filters: [Biquad; SPECTRUM_BANDS] = Default::default();
        let mut bands = 0;
        for (k, f) in filters.iter_mut().enumerate() {
            let hz = spectrum_hz(k);
            if hz < sr * 0.45 {
                // Q of a third-octave band: 4.32.
                f.set_coefficients(BiquadCoeffs::band_pass(f64::from(sr), f64::from(hz), 4.32));
                bands = k + 1;
            }
        }
        Self { filters, meters: [PeakMeter::new(sr); SPECTRUM_BANDS], bands }
    }

    /// One block of the (mono) signal.
    pub fn process(&mut self, x: &[f32]) {
        for (f, m) in self.filters.iter_mut().zip(self.meters.iter_mut()).take(self.bands) {
            for &v in x {
                // The band-pass peaks at unity: a sine at the band's centre reads its own level.
                m.tick(f.tick(v, 0));
            }
        }
    }

    /// The bands' levels (linear peaks).
    pub fn levels(&self, out: &mut [f32]) {
        for (o, m) in out.iter_mut().zip(&self.meters) {
            *o = m.level();
        }
    }

    pub fn reset(&mut self) {
        for f in &mut self.filters {
            f.reset();
        }
        for m in &mut self.meters {
            m.reset();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::f32::consts::TAU;

    #[test]
    fn peak_rises_at_once_and_falls_at_the_meter_rate() {
        let sr = 48_000.0;
        let mut m = PeakMeter::new(sr);
        m.block(&[0.0, 0.5, -1.0, 0.2]);
        assert_eq!(m.level(), 1.0);
        // Half a second of silence: 30 dB down.
        m.block(&vec![0.0; 24_000]);
        let db = 20.0 * m.level().log10();
        assert!((db + 30.0).abs() < 0.1, "{db}");
        let mut t = PeakMeter::new(sr);
        t.tick(1.0);
        for _ in 0..24_000 {
            t.tick(0.0);
        }
        assert!((t.level() / m.level() - 1.0).abs() < 1e-3);
    }

    #[test]
    fn vu_reads_rms_against_its_zero() {
        let sr = 48_000.0;
        let mut vu = VuMeter::new(sr, -18.0);
        // A sine with an RMS of -18 dBFS reads 0 VU once settled.
        let amp = 10f32.powf(-18.0 / 20.0) * std::f32::consts::SQRT_2;
        for i in 0..48_000 {
            vu.tick((TAU * 1000.0 * i as f32 / sr).sin() * amp);
        }
        assert!((20.0 * vu.level().log10()).abs() < 0.2, "{}", vu.level());
        // 300 ms after a step it's within 1 % of its power.
        let mut vu = VuMeter::new(sr, 0.0);
        for _ in 0..(0.3 * sr) as usize {
            vu.tick(1.0);
        }
        assert!(vu.level() > 0.99f32.sqrt() - 1e-3, "{}", vu.level());
    }

    #[test]
    fn spectrum_finds_a_tone_in_its_band() {
        let sr = 48_000.0;
        let mut s = Spectrum::new(sr);
        let x: Vec<f32> = (0..9600).map(|i| (TAU * 1000.0 * i as f32 / sr).sin() * 0.5).collect();
        s.process(&x);
        let mut lv = [0.0; SPECTRUM_BANDS];
        s.levels(&mut lv);
        let k1 = 16; // 1 kHz
        assert!((spectrum_hz(k1) - 1000.0).abs() < 1e-3);
        assert!((lv[k1] - 0.5).abs() < 0.03, "{}", lv[k1]);
        assert!(lv[k1 - 6] < 0.05 && lv[k1 + 6] < 0.05, "{lv:?}");
        assert!(lv.iter().all(|v| v.is_finite()));
    }
}
