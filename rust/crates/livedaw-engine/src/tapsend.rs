//! TAP WIDTH and TAP EQ (WING `/ch/n/tapwid`, `/ch/n/peq`): an input
//! channel's TAP-mode sends take its tap signal through a 3-band EQ (low
//! shelf, peak, high shelf) and a stereo width. Parameter changes glide: the
//! band settings are smoothed per 32-sample sub-block, the width per sample.

use crate::params::{StripParams, TapEqParams, TAP_EQ_BANDS, TAP_EQ_GAIN_RANGE, TAP_EQ_Q_RANGE, TAP_WIDTH_RANGE};
use livedaw_dsp::biquad::{Biquad, BiquadCoeffs};
use std::sync::atomic::Ordering::Relaxed;

const SUB_BLOCK: usize = 32;
const GLIDE_MS: f64 = 20.0;

#[derive(Clone, Copy, Debug, PartialEq)]
struct Band {
    gain: f32,
    freq: f32,
    q: f32,
}

impl Band {
    const UNSET: Band = Band { gain: f32::NAN, freq: 0.0, q: 0.0 };

    fn coefficients(&self, index: usize, sr: f64) -> BiquadCoeffs {
        let (f, q, g) = (self.freq as f64, self.q as f64, self.gain as f64);
        match index {
            0 => BiquadCoeffs::low_shelf(sr, f, q, g),
            1 => BiquadCoeffs::peak(sr, f, q, g),
            _ => BiquadCoeffs::high_shelf(sr, f, q, g),
        }
    }
}

/// The TAP EQ's target settings (gain 0 when the EQ is off).
fn targets(eq: &TapEqParams) -> [Band; TAP_EQ_BANDS] {
    let on = eq.on.load(Relaxed);
    std::array::from_fn(|i| {
        let b = &eq.bands[i];
        let finite = |v: f32, d: f32| if v.is_finite() { v } else { d };
        Band {
            gain: if on { finite(b.gain_db.load(), 0.0).clamp(-TAP_EQ_GAIN_RANGE, TAP_EQ_GAIN_RANGE) } else { 0.0 },
            freq: finite(b.freq.load(), 1000.0).clamp(20.0, 20000.0),
            q: finite(b.q.load(), 1.0).clamp(TAP_EQ_Q_RANGE.0, TAP_EQ_Q_RANGE.1),
        }
    })
}

pub(crate) struct TapShaper {
    sr: f64,
    filters: [Biquad; TAP_EQ_BANDS],
    current: [Band; TAP_EQ_BANDS],
    designed: [Band; TAP_EQ_BANDS],
    /// The EQ is running (some band has gain, or is gliding to or from it).
    running: bool,
    /// Per-sub-block glide coefficient.
    glide: f32,
    /// The width applied last (1 = as is).
    width: f32,
}

impl Default for TapShaper {
    fn default() -> Self {
        let mut t = Self {
            sr: 48000.0,
            filters: Default::default(),
            current: [Band::UNSET; TAP_EQ_BANDS],
            designed: [Band::UNSET; TAP_EQ_BANDS],
            running: false,
            glide: 0.0,
            width: 1.0,
        };
        t.prepare(48000.0);
        t
    }
}

impl TapShaper {
    pub fn prepare(&mut self, sample_rate: f64) {
        self.sr = sample_rate;
        self.glide = (1.0 - (-(SUB_BLOCK as f64) / (sample_rate * GLIDE_MS * 0.001)).exp()) as f32;
        self.reset();
    }

    pub fn reset(&mut self) {
        for f in &mut self.filters {
            f.reset();
        }
        self.running = false;
        self.designed = [Band::UNSET; TAP_EQ_BANDS];
        self.width = 1.0;
    }

    /// Shapes the tap signal (1 or 2 channels) in place.
    pub fn process(&mut self, chans: &mut [&mut [f32]], p: &StripParams) {
        self.process_eq(chans, &p.tap_eq);
        if let [l, r] = chans {
            let w = p.tap_width.load();
            let w = if w.is_finite() { (w / 100.0).clamp(-TAP_WIDTH_RANGE / 100.0, TAP_WIDTH_RANGE / 100.0) } else { 1.0 };
            self.apply_width(l, r, w);
        }
    }

    fn apply_width(&mut self, l: &mut [f32], r: &mut [f32], target: f32) {
        let last = self.width;
        self.width = target;
        if last == 1.0 && target == 1.0 {
            return;
        }
        let n = l.len().min(r.len());
        let step = (target - last) / n.max(1) as f32;
        let mut w = last;
        for (a, b) in l.iter_mut().zip(r.iter_mut()) {
            w += step;
            let (same, cross) = ((1.0 + w) * 0.5, (1.0 - w) * 0.5);
            let (x, y) = (*a, *b);
            *a = same * x + cross * y;
            *b = cross * x + same * y;
        }
    }

    fn process_eq(&mut self, chans: &mut [&mut [f32]], eq: &TapEqParams) {
        let target = targets(eq);
        if !self.running {
            if target.iter().all(|b| b.gain == 0.0) {
                return;
            }
            // Start flat at the target frequencies, then glide the gains in.
            self.current = target.map(|b| Band { gain: 0.0, ..b });
            self.designed = [Band::UNSET; TAP_EQ_BANDS];
            for f in &mut self.filters {
                f.reset();
            }
            self.running = true;
        }
        let n = chans.iter().map(|c| c.len()).min().unwrap_or(0);
        let k = self.glide;
        let mut start = 0;
        while start < n {
            let end = (start + SUB_BLOCK).min(n);
            let bands = self.current.iter_mut().zip(target).zip(self.filters.iter_mut().zip(self.designed.iter_mut()));
            for (i, ((c, t), (filter, designed))) in bands.enumerate() {
                c.gain += (t.gain - c.gain) * k;
                if (t.gain - c.gain).abs() < 0.01 {
                    c.gain = t.gain;
                }
                // Frequency and Q glide on a log scale.
                c.freq *= (t.freq / c.freq).powf(k);
                if (t.freq / c.freq - 1.0).abs() < 1e-3 {
                    c.freq = t.freq;
                }
                c.q *= (t.q / c.q).powf(k);
                if (t.q / c.q - 1.0).abs() < 1e-3 {
                    c.q = t.q;
                }
                if c.gain == 0.0 {
                    // Flat: skip the band (a fresh state when it comes back).
                    if designed.gain != 0.0 {
                        filter.reset();
                        *designed = Band::UNSET;
                    }
                    continue;
                }
                if *c != *designed {
                    filter.set_coefficients(c.coefficients(i, self.sr));
                    *designed = *c;
                }
                for (ch, data) in chans.iter_mut().enumerate() {
                    filter.process(&mut data[start..end], ch);
                }
            }
            start = end;
        }
        if self.current.iter().zip(&target).all(|(c, t)| c.gain == 0.0 && t.gain == 0.0) {
            self.running = false;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::params::{StripKind, StripParams};

    fn rms(x: &[f32]) -> f32 {
        (x.iter().map(|v| v * v).sum::<f32>() / x.len() as f32).sqrt()
    }

    /// The steady-state gain of the shaper for a sine at `f` (mono).
    fn gain_at(p: &StripParams, f: f32) -> f32 {
        let mut t = TapShaper::default();
        let sr = 48000.0;
        let mut out = Vec::new();
        for block in 0..80 {
            let mut x: Vec<f32> =
                (0..512).map(|i| (2.0 * std::f32::consts::PI * f * (block * 512 + i) as f32 / sr).sin()).collect();
            t.process(&mut [&mut x[..]], p);
            if block >= 60 {
                out.extend(x);
            }
        }
        20.0 * (rms(&out) * std::f32::consts::SQRT_2).log10()
    }

    #[test]
    fn tap_eq_shapes_low_mid_and_high() {
        let p = StripParams::new(StripKind::Channel);
        let eq = &p.tap_eq;
        // Off (or flat): untouched.
        assert!(gain_at(&p, 1000.0).abs() < 0.05);
        eq.bands[0].gain_db.store(6.0);
        assert!(gain_at(&p, 1000.0).abs() < 0.05, "off ignores the gains");
        eq.on.store(true, Relaxed);
        // Low shelf +6 dB at 100 Hz.
        assert!((gain_at(&p, 30.0) - 6.0).abs() < 0.5);
        assert!(gain_at(&p, 3000.0).abs() < 0.3);
        // Peak -9 dB at 1 kHz.
        eq.bands[0].gain_db.store(0.0);
        eq.bands[1].gain_db.store(-9.0);
        assert!((gain_at(&p, 1000.0) + 9.0).abs() < 0.3);
        assert!(gain_at(&p, 100.0).abs() < 0.5 && gain_at(&p, 15000.0).abs() < 0.5);
        // High shelf +4 dB at 10 kHz.
        eq.bands[1].gain_db.store(0.0);
        eq.bands[2].gain_db.store(4.0);
        assert!((gain_at(&p, 18000.0) - 4.0).abs() < 0.6);
        assert!(gain_at(&p, 200.0).abs() < 0.1);
    }

    #[test]
    fn tap_width_mixes_and_swaps() {
        let p = StripParams::new(StripKind::Channel);
        let mut t = TapShaper::default();
        let run = |t: &mut TapShaper| {
            let (mut l, mut r) = (vec![1.0f32; 64], vec![0.0f32; 64]);
            t.process(&mut [&mut l[..], &mut r[..]], &p);
            (l[63], r[63])
        };
        assert_eq!(run(&mut t), (1.0, 0.0));
        p.tap_width.store(0.0); // mono
        run(&mut t);
        let (l, r) = run(&mut t);
        assert!((l - 0.5).abs() < 1e-6 && (r - 0.5).abs() < 1e-6);
        p.tap_width.store(-100.0); // swapped
        run(&mut t);
        let (l, r) = run(&mut t);
        assert!(l.abs() < 1e-6 && (r - 1.0).abs() < 1e-6);
        p.tap_width.store(150.0);
        run(&mut t);
        let (l, r) = run(&mut t);
        assert!((l - 1.25).abs() < 1e-6 && (r + 0.25).abs() < 1e-6);
    }

    #[test]
    fn changes_glide_without_jumps() {
        let p = StripParams::new(StripKind::Channel);
        p.tap_eq.on.store(true, Relaxed);
        let mut t = TapShaper::default();
        let mut prev = 0.0f32;
        let mut worst = 0.0f32;
        for block in 0..40 {
            if block == 10 {
                p.tap_eq.bands[1].gain_db.store(15.0);
                p.tap_eq.bands[1].freq.store(100.0);
            }
            if block == 25 {
                p.tap_eq.on.store(false, Relaxed);
            }
            // A slow sine: any coefficient jump shows as a step.
            let mut x: Vec<f32> =
                (0..256).map(|i| (2.0 * std::f32::consts::PI * 50.0 * (block * 256 + i) as f32 / 48000.0).sin() * 0.5).collect();
            t.process(&mut [&mut x[..]], &p);
            for v in x {
                worst = worst.max((v - prev).abs());
                prev = v;
            }
        }
        assert!(worst < 0.05, "largest step {worst}");
    }
}
