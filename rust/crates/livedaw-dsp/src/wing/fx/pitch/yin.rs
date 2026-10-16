//! Monophonic pitch detection for the pitch fix: YIN (de Cheveigné &
//! Kawahara) on a decimated copy of the input, refined at the full rate.
//!
//! Every hop (about 5 ms) the cumulative-mean-normalised difference function
//! of the last ~35 ms is searched for its first clear dip; its depth decides
//! voiced/unvoiced, and a short search of the full-rate difference function
//! around the coarse lag (with parabolic interpolation) gives the period to a
//! fraction of a cent.

use super::super::blocks::{Biquad, BiquadCoeffs};
use super::shifter::dot;

/// Lowest and highest fundamental tracked (Hz).
const F_MIN: f32 = 60.0;
const F_MAX: f32 = 1100.0;
/// CMNDF dip that counts as periodic.
const THRESHOLD: f32 = 0.15;
/// Deepest CMNDF minimum still called voiced.
const VOICED: f32 = 0.3;
/// RMS below which the input counts as silence (about -54 dBFS).
const GATE: f32 = 0.002;

/// One analysis result.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Detection {
    pub voiced: bool,
    /// Fundamental in Hz (valid when voiced).
    pub hz: f32,
}

pub struct PitchDetector {
    sr: f32,
    dec: usize,
    phase: usize,
    hop: usize,
    count: usize,
    hp: Biquad,
    lp: [Biquad; 2],
    /// Full-rate (filtered) history.
    full: Vec<f32>,
    full_pos: usize,
    /// Decimated history.
    low: Vec<f32>,
    low_pos: usize,
    tau_min: usize,
    tau_max: usize,
    win: usize,
    frame: Vec<f32>,
    ffull: Vec<f32>,
    diff: Vec<f32>,
}

impl PitchDetector {
    /// Message thread (allocates).
    pub fn new(sr: f32) -> Self {
        let dec = ((sr / 10_000.0) as usize).max(1);
        let fs = sr / dec as f32;
        let tau_max = (fs / F_MIN).ceil() as usize + 2;
        let tau_min = ((fs / F_MAX).floor() as usize).max(2);
        let win = tau_max;
        let frame_len = win + tau_max + 1;
        let full_len = (frame_len + 2) * dec + 8;
        let mut hp = Biquad::default();
        hp.set_coefficients(BiquadCoeffs::high_pass(sr as f64, 50.0, 0.707));
        let mut lp = [Biquad::default(), Biquad::default()];
        let cut = (0.3 * fs).min(4000.0) as f64;
        lp[0].set_coefficients(BiquadCoeffs::low_pass(sr as f64, cut, 0.541));
        lp[1].set_coefficients(BiquadCoeffs::low_pass(sr as f64, cut, 1.307));
        Self {
            sr,
            dec,
            phase: 0,
            hop: ((0.005 * fs) as usize).max(1) * dec,
            count: 0,
            hp,
            lp,
            full: vec![0.0; full_len.next_power_of_two()],
            full_pos: 0,
            low: vec![0.0; frame_len.next_power_of_two()],
            low_pos: 0,
            tau_min,
            tau_max,
            win,
            frame: vec![0.0; frame_len],
            ffull: vec![0.0; full_len],
            diff: vec![0.0; tau_max + 2],
        }
    }

    pub fn reset(&mut self) {
        self.hp.reset();
        for f in &mut self.lp {
            f.reset();
        }
        self.full.fill(0.0);
        self.low.fill(0.0);
        self.phase = 0;
        self.count = 0;
    }

    /// Feeds one sample; returns a detection once per hop.
    #[inline]
    pub fn push(&mut self, x: f32) -> Option<Detection> {
        let y = super::chain(&mut self.lp, self.hp.tick(x, 0), 0);
        self.full_pos = (self.full_pos + 1) & (self.full.len() - 1);
        self.full[self.full_pos] = y;
        self.phase += 1;
        if self.phase >= self.dec {
            self.phase = 0;
            self.low_pos = (self.low_pos + 1) & (self.low.len() - 1);
            self.low[self.low_pos] = y;
        }
        self.count += 1;
        if self.count >= self.hop {
            self.count = 0;
            Some(self.analyse())
        } else {
            None
        }
    }

    fn analyse(&mut self) -> Detection {
        let unvoiced = Detection { voiced: false, hz: 0.0 };
        // Decimated frame, oldest first.
        let n = self.frame.len();
        let mask = self.low.len() - 1;
        for (i, f) in self.frame.iter_mut().enumerate() {
            *f = self.low[(self.low_pos + mask + 1 - (n - 1 - i)) & mask];
        }
        let win = self.win;
        let recent = &self.frame[n - win..];
        let energy = dot(recent, recent);
        if energy < GATE * GATE * win as f32 {
            return unvoiced;
        }

        // Difference function d(tau) = e(0) + e(tau) - 2 r(tau), over a window
        // starting at the oldest sample.
        let head = &self.frame[..win];
        let e0 = dot(head, head);
        let mut e_tau = e0;
        self.diff[0] = 0.0;
        for tau in 1..=self.tau_max {
            let a = self.frame[tau - 1];
            let b = self.frame[tau - 1 + win];
            e_tau += b * b - a * a;
            let r = dot(head, &self.frame[tau..tau + win]);
            self.diff[tau] = (e0 + e_tau - 2.0 * r).max(0.0);
        }
        // Cumulative mean normalisation.
        let mut sum = 0.0;
        for tau in 1..=self.tau_max {
            sum += self.diff[tau];
            self.diff[tau] = if sum > 0.0 { self.diff[tau] * tau as f32 / sum } else { 1.0 };
        }
        // First dip below the threshold, else the global minimum.
        let mut tau = 0;
        let mut t = self.tau_min;
        while t < self.tau_max {
            if self.diff[t] < THRESHOLD {
                while t + 1 < self.tau_max && self.diff[t + 1] < self.diff[t] {
                    t += 1;
                }
                tau = t;
                break;
            }
            t += 1;
        }
        if tau == 0 {
            let mut best = self.tau_min;
            for t in self.tau_min..self.tau_max {
                if self.diff[t] < self.diff[best] {
                    best = t;
                }
            }
            tau = best;
        }
        if self.diff[tau] > VOICED || tau <= self.tau_min || tau >= self.tau_max - 1 {
            return unvoiced;
        }
        let coarse = tau as f32 + parabola(self.diff[tau - 1], self.diff[tau], self.diff[tau + 1]);

        // Refine on the full-rate signal around the coarse lag.
        let dec = self.dec;
        if dec == 1 {
            return Detection { voiced: true, hz: self.sr / coarse };
        }
        let centre = (coarse * dec as f32).round() as usize;
        let span = dec + 1;
        let lo_lag = centre.saturating_sub(span).max(2);
        let hi_lag = centre + span;
        let fwin = win * dec;
        let need = fwin + hi_lag + 2;
        let nf = need.min(self.ffull.len());
        let fmask = self.full.len() - 1;
        for i in 0..nf {
            self.ffull[i] = self.full[(self.full_pos + fmask + 1 - (nf - 1 - i)) & fmask];
        }
        if need > nf {
            return Detection { voiced: true, hz: self.sr / (coarse * dec as f32) };
        }
        let fhead = &self.ffull[..fwin];
        let fe0 = dot(fhead, fhead);
        let mut best = (centre, f32::MAX);
        let mut vals = [0.0f32; 64];
        let count = (hi_lag - lo_lag + 1).min(vals.len());
        for (k, v) in vals.iter_mut().enumerate().take(count) {
            let lag = lo_lag + k;
            let cand = &self.ffull[lag..lag + fwin];
            let d = fe0 + dot(cand, cand) - 2.0 * dot(fhead, cand);
            *v = d;
            if d < best.1 {
                best = (lag, d);
            }
        }
        let k = best.0 - lo_lag;
        let lag =
            if k > 0 && k + 1 < count { best.0 as f32 + parabola(vals[k - 1], vals[k], vals[k + 1]) } else { best.0 as f32 };
        Detection { voiced: true, hz: self.sr / lag }
    }
}

/// Offset (-0.5..0.5) of a parabola's extremum through three equally spaced points.
#[inline]
fn parabola(a: f32, b: f32, c: f32) -> f32 {
    let den = a - 2.0 * b + c;
    if den.abs() < 1e-12 {
        0.0
    } else {
        (0.5 * (a - c) / den).clamp(-0.5, 0.5)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::f32::consts::TAU;

    fn detect(sr: f32, hz: f32, harmonics: bool) -> Option<f32> {
        let mut d = PitchDetector::new(sr);
        let mut last = None;
        for i in 0..(sr * 0.3) as usize {
            let t = i as f32 / sr;
            let mut x = (TAU * hz * t).sin();
            if harmonics {
                x += 0.6 * (TAU * 2.0 * hz * t).sin() + 0.4 * (TAU * 3.0 * hz * t + 1.0).sin();
            }
            if let Some(det) = d.push(0.3 * x) {
                last = Some(det);
            }
        }
        last.filter(|d| d.voiced).map(|d| d.hz)
    }

    #[test]
    fn finds_the_fundamental() {
        for sr in [44_100.0, 48_000.0, 96_000.0] {
            for hz in [82.4, 196.0, 440.0, 880.0] {
                for harm in [false, true] {
                    let got = detect(sr, hz, harm).unwrap_or(0.0);
                    let cents = 1200.0 * (got / hz).log2();
                    assert!(cents.abs() < 2.0, "{sr} {hz} {harm}: {got}");
                }
            }
        }
    }

    #[test]
    fn noise_and_silence_are_unvoiced() {
        let mut d = PitchDetector::new(48_000.0);
        let mut n = crate::wing::fx::blocks::Noise::default();
        let mut voiced = 0;
        for i in 0..48_000 {
            let x = if i < 24_000 { 0.0 } else { n.sample() * 0.3 };
            if let Some(det) = d.push(x) {
                voiced += det.voiced as usize;
            }
        }
        assert!(voiced < 5, "{voiced}");
    }
}
