//! Stereo-linked noise gate with hold and hysteresis.

use crate::util::{db_to_gain, time_constant_coeff};

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct GateSettings {
    pub enabled: bool,
    pub threshold_db: f32,
    /// Attenuation applied while closed.
    pub range_db: f32,
    pub attack_ms: f32,
    pub hold_ms: f32,
    pub release_ms: f32,
    /// Closes this far below the threshold.
    pub hysteresis_db: f32,
}

impl Default for GateSettings {
    fn default() -> Self {
        Self {
            enabled: false,
            threshold_db: -50.0,
            range_db: -80.0,
            attack_ms: 0.5,
            hold_ms: 50.0,
            release_ms: 150.0,
            hysteresis_db: 4.0,
        }
    }
}

pub struct NoiseGate {
    settings: GateSettings,
    sample_rate: f64,
    open_threshold: f32,
    close_threshold: f32,
    range_gain: f32,
    attack: f32,
    release: f32,
    detector_release: f32,
    hold_samples: u32,
    envelope: f32,
    gain: f32,
    last_min_gain: f32,
    hold_counter: u32,
    open: bool,
}

impl Default for NoiseGate {
    fn default() -> Self {
        let mut g = Self {
            settings: GateSettings::default(),
            sample_rate: 48000.0,
            open_threshold: 0.0,
            close_threshold: 0.0,
            range_gain: 0.0,
            attack: 0.0,
            release: 0.0,
            detector_release: 0.0,
            hold_samples: 0,
            envelope: 0.0,
            gain: 1.0,
            last_min_gain: 1.0,
            hold_counter: 0,
            open: false,
        };
        g.update_coefficients();
        g
    }
}

impl NoiseGate {
    pub fn prepare(&mut self, sample_rate: f64) {
        self.sample_rate = sample_rate;
        self.update_coefficients();
        self.reset();
    }

    pub fn reset(&mut self) {
        self.envelope = 0.0;
        self.gain = if self.settings.enabled { self.range_gain } else { 1.0 };
        self.hold_counter = 0;
        self.open = false;
        self.last_min_gain = 1.0;
    }

    pub fn set_settings(&mut self, s: GateSettings) {
        if s == self.settings {
            return;
        }
        let was_enabled = self.settings.enabled;
        self.settings = s;
        self.update_coefficients();
        if was_enabled != s.enabled {
            self.gain = 1.0; // fade in from unity when switching on; the release takes it down
        }
    }

    fn update_coefficients(&mut self) {
        let s = &self.settings;
        self.open_threshold = db_to_gain(s.threshold_db);
        self.close_threshold = db_to_gain(s.threshold_db - s.hysteresis_db.max(0.0));
        self.range_gain = db_to_gain(s.range_db.min(0.0));
        self.attack = time_constant_coeff(s.attack_ms.max(0.01), self.sample_rate);
        self.release = time_constant_coeff(s.release_ms.max(1.0), self.sample_rate);
        self.detector_release = time_constant_coeff(10.0, self.sample_rate);
        self.hold_samples = (s.hold_ms.max(0.0) as f64 * 0.001 * self.sample_rate) as u32;
    }

    pub fn is_open(&self) -> bool {
        self.open
    }

    /// Lowest gain (linear) applied during the last processed block.
    pub fn last_block_min_gain(&self) -> f32 {
        self.last_min_gain
    }

    pub fn process(&mut self, channels: &mut [&mut [f32]]) {
        if !self.settings.enabled {
            self.last_min_gain = 1.0;
            return;
        }
        let n = channels.first().map_or(0, |c| c.len());
        let mut min_gain = 1.0f32;
        for i in 0..n {
            let level = channels.iter().fold(0.0f32, |l, c| l.max(c[i].abs()));
            self.envelope = if level > self.envelope { level } else { level + self.detector_release * (self.envelope - level) };

            if self.envelope >= self.open_threshold {
                self.open = true;
                self.hold_counter = self.hold_samples;
            } else if self.envelope < self.close_threshold {
                if self.hold_counter > 0 {
                    self.hold_counter -= 1;
                } else {
                    self.open = false;
                }
            }

            let target = if self.open { 1.0 } else { self.range_gain };
            let coeff = if target > self.gain { self.attack } else { self.release };
            self.gain = target + coeff * (self.gain - target);
            for c in channels.iter_mut() {
                c[i] *= self.gain;
            }
            min_gain = min_gain.min(self.gain);
        }
        self.last_min_gain = min_gain;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::util::peak_level;

    fn sine(amp: f32, n: usize) -> Vec<f32> {
        (0..n).map(|i| amp * (2.0 * std::f32::consts::PI * 500.0 * i as f32 / 48000.0).sin()).collect()
    }

    #[test]
    fn closes_on_quiet_and_opens_on_loud() {
        let mut g = NoiseGate::default();
        g.set_settings(GateSettings {
            enabled: true,
            threshold_db: -40.0,
            range_db: -60.0,
            release_ms: 20.0,
            hold_ms: 10.0,
            ..Default::default()
        });
        g.prepare(48000.0);

        let mut quiet = sine(db_to_gain(-60.0), 24000);
        g.process(&mut [&mut quiet]);
        assert!(!g.is_open());
        assert!(peak_level(&quiet[20000..]) < 3.2e-6);

        let mut loud = sine(db_to_gain(-10.0), 24000);
        g.process(&mut [&mut loud]);
        assert!(g.is_open());
        assert!((peak_level(&loud[20000..]) - db_to_gain(-10.0)).abs() < 0.01);
    }

    #[test]
    fn disabled_is_transparent() {
        let mut g = NoiseGate::default();
        let mut s = sine(0.5, 4800);
        let orig = s.clone();
        g.process(&mut [&mut s]);
        assert_eq!(s, orig);
    }
}
