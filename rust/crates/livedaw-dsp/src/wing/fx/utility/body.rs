//! BODYREZ, after the tc electronic acoustic pickup enhancer pedal (WING
//! manual 11.10): it restores the natural acoustic resonance an acoustic
//! guitar loses through a sub-optimal (undersaddle piezo) pickup, with
//! pre-defined filters and compression under one knob.
//!
//! BODY (0..100) scales both. The filters: a resonance lift around 200 Hz (up
//! to +6 dB), a notch of the piezo "quack" around 1.25 kHz (up to -9 dB) and
//! an air shelf above 7 kHz (up to +5 dB). The compression: a gentle,
//! stereo-linked RMS compressor (soft knee around -24 dBFS, up to 2.5:1,
//! 10 ms / 150 ms) that evens out the pickup's spiky dynamics, with half its
//! reduction at -18 dBFS made up. BODY 0 is flat and uncompressed. The curve
//! and compressor values are this emulation's choice; the manual gives none.

use super::{db_gain, power_db, sub_blocks, Cascade, Design};
use crate::wing::fx::blocks::{coef, BiquadCoeffs, Smoothed};
use crate::wing::fx::FxAlgo;
use crate::wing::Params;

const COMP_THR_DB: f32 = -24.0;
const COMP_KNEE_DB: f32 = 10.0;
const COMP_MAX_RATIO: f32 = 2.5;

/// Gain change (dB, <= 0) of the compressor with slope `slope` (1/ratio - 1) at `lvl` dB.
#[inline]
fn curve(lvl: f32, slope: f32) -> f32 {
    let over = lvl - COMP_THR_DB;
    if over >= 0.5 * COMP_KNEE_DB {
        slope * over
    } else if over > -0.5 * COMP_KNEE_DB {
        slope * (over + 0.5 * COMP_KNEE_DB).powi(2) / (2.0 * COMP_KNEE_DB)
    } else {
        0.0
    }
}

pub(crate) struct BodyRez {
    sr: f32,
    filters: Cascade<3>,
    amount: Smoothed,
    designed: f32,
    rms: f32,
    rms_coef: f32,
    gr: f32,
    att: f32,
    rel: f32,
    primed: bool,
}

impl BodyRez {
    pub(crate) fn new(sr: f32) -> Self {
        let mut b = Self {
            sr,
            filters: Cascade::new(),
            amount: Smoothed::new(0.0, 15.0, sr),
            designed: -1.0,
            rms: 0.0,
            rms_coef: coef(20.0, sr),
            gr: 0.0,
            att: coef(10.0, sr),
            rel: coef(150.0, sr),
            primed: false,
        };
        b.redesign(0.0);
        b
    }

    fn redesign(&mut self, k: f32) {
        let sr = self.sr as f64;
        let k = k as f64;
        let mut d = Design::<3>::new();
        d.push(BiquadCoeffs::peak(sr, 200.0, 1.1, 6.0 * k));
        d.push(BiquadCoeffs::peak(sr, 1250.0, 1.6, -9.0 * k));
        d.push(BiquadCoeffs::high_shelf(sr, 7000.0_f64.min(sr * 0.4), 0.7, 5.0 * k));
        self.filters.set(d.sections());
        self.designed = k as f32;
    }
}

impl FxAlgo for BodyRez {
    fn set_params(&mut self, p: &Params) {
        self.amount.target = p.f("body") / 100.0;
        if !self.primed {
            self.amount.snap(self.amount.target);
            self.redesign(self.amount.target);
            self.primed = true;
        }
    }

    fn process(&mut self, left: &mut [f32], right: &mut [f32]) {
        let n = left.len().min(right.len());
        sub_blocks(n, |a, b| {
            // Glide once per sub-block, redesigning only while it moves.
            for _ in a..b {
                self.amount.tick();
            }
            let k = if (self.amount.value - self.amount.target).abs() < 1e-4 { self.amount.target } else { self.amount.value };
            if k != self.designed {
                self.redesign(k);
            }
            let (l, r) = (&mut left[a..b], &mut right[a..b]);
            self.filters.process(l, r);
            if k <= 0.0 && self.gr == 0.0 {
                return;
            }
            let slope = 1.0 / (1.0 + (COMP_MAX_RATIO - 1.0) * k) - 1.0;
            let makeup = -0.5 * curve(-18.0, slope);
            for (x, y) in l.iter_mut().zip(r.iter_mut()) {
                self.rms += (0.5 * (*x * *x + *y * *y) - self.rms) * self.rms_coef;
                let target = curve(power_db(self.rms) + 3.0, slope);
                self.gr += (target - self.gr) * if target < self.gr { self.att } else { self.rel };
                if k <= 0.0 && self.gr.abs() < 1e-4 {
                    self.gr = 0.0;
                }
                let g = db_gain(self.gr + makeup);
                *x *= g;
                *y *= g;
            }
        });
    }

    fn reset(&mut self) {
        self.filters.reset();
        self.rms = 0.0;
        self.gr = 0.0;
        self.amount.snap(self.amount.target);
        self.redesign(self.amount.target);
    }
}

#[cfg(test)]
mod tests {
    use super::super::tests::Rig;

    #[test]
    fn body_shapes_and_zero_is_flat() {
        let mut rig = Rig::new("BODY");
        rig.set("body", 100.0);
        assert!(rig.sine_gain_db(200.0, 0.05) > 4.0);
        assert!(rig.sine_gain_db(1250.0, 0.05) < -6.0);
        let mut rig = Rig::new("BODY");
        rig.set("body", 0.0);
        for f in [100.0, 1250.0, 10_000.0] {
            assert!(rig.sine_gain_db(f, 0.1).abs() < 0.05);
        }
    }

    #[test]
    fn body_compresses_loud_playing() {
        // 20 dB more input comes out with clearly less than 20 dB more level.
        let level = |amp: f32| {
            let mut rig = Rig::new("BODY");
            rig.set("body", 100.0);
            rig.sine_gain_db(3000.0, amp) + 20.0 * amp.log10()
        };
        let span = level(0.5) - level(0.05);
        assert!(span < 16.0, "{span}");
    }
}
