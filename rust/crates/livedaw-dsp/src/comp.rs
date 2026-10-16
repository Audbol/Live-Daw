//! Stereo-linked feed-forward compressor with a soft knee.

use crate::util::time_constant_coeff;

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct CompressorSettings {
    pub enabled: bool,
    pub threshold_db: f32,
    pub ratio: f32,
    pub attack_ms: f32,
    pub release_ms: f32,
    pub knee_db: f32,
    pub makeup_db: f32,
}

impl Default for CompressorSettings {
    fn default() -> Self {
        Self { enabled: false, threshold_db: -20.0, ratio: 4.0, attack_ms: 10.0, release_ms: 120.0, knee_db: 6.0, makeup_db: 0.0 }
    }
}

pub struct Compressor {
    settings: CompressorSettings,
    sample_rate: f64,
    attack: f32,
    release: f32,
    reduction_db: f32,
    last_max_reduction: f32,
}

impl Default for Compressor {
    fn default() -> Self {
        let mut c = Self {
            settings: CompressorSettings::default(),
            sample_rate: 48000.0,
            attack: 0.0,
            release: 0.0,
            reduction_db: 0.0,
            last_max_reduction: 0.0,
        };
        c.update_coefficients();
        c
    }
}

impl Compressor {
    pub fn prepare(&mut self, sample_rate: f64) {
        self.sample_rate = sample_rate;
        self.update_coefficients();
        self.reset();
    }

    pub fn reset(&mut self) {
        self.reduction_db = 0.0;
        self.last_max_reduction = 0.0;
    }

    pub fn set_settings(&mut self, s: CompressorSettings) {
        if s != self.settings {
            self.settings = s;
            self.update_coefficients();
        }
    }

    fn update_coefficients(&mut self) {
        self.attack = time_constant_coeff(self.settings.attack_ms.max(0.05), self.sample_rate);
        self.release = time_constant_coeff(self.settings.release_ms.max(1.0), self.sample_rate);
    }

    /// Static gain computer: gain reduction in dB (>= 0) for an input level in dB.
    pub fn gain_reduction_db(s: &CompressorSettings, input_db: f32) -> f32 {
        let slope = 1.0 - 1.0 / s.ratio.max(1.0);
        let knee = s.knee_db.max(0.0);
        let over = input_db - s.threshold_db;
        if knee > 0.0 && 2.0 * over.abs() <= knee {
            let x = over + knee * 0.5;
            return slope * x * x / (2.0 * knee);
        }
        if over > 0.0 {
            slope * over
        } else {
            0.0
        }
    }

    /// Largest gain reduction (dB) during the last processed block.
    pub fn last_block_gain_reduction_db(&self) -> f32 {
        self.last_max_reduction
    }

    pub fn process(&mut self, channels: &mut [&mut [f32]]) {
        if !self.settings.enabled {
            self.reduction_db = 0.0;
            self.last_max_reduction = 0.0;
            return;
        }
        let n = channels.first().map_or(0, |c| c.len());
        let makeup = self.settings.makeup_db;
        let mut max_reduction = 0.0f32;
        for i in 0..n {
            let level = channels.iter().fold(0.0f32, |l, c| l.max(c[i].abs()));
            let input_db = 20.0 * level.max(1e-6).log10();
            let target = Self::gain_reduction_db(&self.settings, input_db);
            let coeff = if target > self.reduction_db { self.attack } else { self.release };
            self.reduction_db = target + coeff * (self.reduction_db - target);
            let g = 10f32.powf((makeup - self.reduction_db) * 0.05);
            for c in channels.iter_mut() {
                c[i] *= g;
            }
            max_reduction = max_reduction.max(self.reduction_db);
        }
        self.last_max_reduction = max_reduction;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::util::{db_to_gain, peak_level};

    #[test]
    fn static_curve() {
        let mut s = CompressorSettings { threshold_db: -20.0, ratio: 4.0, knee_db: 0.0, ..Default::default() };
        assert!(Compressor::gain_reduction_db(&s, -30.0).abs() < 1e-6);
        assert!((Compressor::gain_reduction_db(&s, -8.0) - 9.0).abs() < 1e-4);
        s.knee_db = 6.0;
        assert!((Compressor::gain_reduction_db(&s, -20.0) - 0.5625).abs() < 1e-4);
        assert!((Compressor::gain_reduction_db(&s, -10.0) - 7.5).abs() < 1e-4);
    }

    #[test]
    fn reduces_loud_signals() {
        let mut c = Compressor::default();
        c.set_settings(CompressorSettings {
            enabled: true,
            threshold_db: -20.0,
            ratio: 4.0,
            knee_db: 0.0,
            attack_ms: 1.0,
            ..Default::default()
        });
        c.prepare(48000.0);
        let mut s: Vec<f32> = (0..48000).map(|i| (2.0 * std::f32::consts::PI * 1000.0 * i as f32 / 48000.0).sin()).collect();
        c.process(&mut [&mut s]);
        assert!((c.last_block_gain_reduction_db() - 15.0).abs() < 0.5);
        assert!(peak_level(&s[40000..]) < db_to_gain(-14.0));
    }
}
