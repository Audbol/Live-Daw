//! BodyRez, after the tc electronic acoustic pickup enhancer: an undersaddle
//! piezo sounds thin and "quacky"; BodyRez restores the body and air of a
//! miked acoustic with one control.
//!
//! BODY (0..100) scales a fixed curve: a resonance lift around 200 Hz (up to
//! +6 dB), a notch of the piezo quack around 1.25 kHz (up to -9 dB) and an air
//! shelf above 7 kHz (up to +5 dB). BODY 0 is flat.

use super::{sub_blocks, Cascade, Design};
use crate::wing::fx::blocks::{BiquadCoeffs, Smoothed};
use crate::wing::fx::FxAlgo;
use crate::wing::Params;

pub(crate) struct BodyRez {
    sr: f32,
    filters: Cascade<3>,
    amount: Smoothed,
    designed: f32,
    primed: bool,
}

impl BodyRez {
    pub(crate) fn new(sr: f32) -> Self {
        let mut b = Self { sr, filters: Cascade::new(), amount: Smoothed::new(0.0, 15.0, sr), designed: -1.0, primed: false };
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
            self.filters.process(&mut left[a..b], &mut right[a..b]);
        });
    }

    fn reset(&mut self) {
        self.filters.reset();
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
        assert!(rig.sine_gain_db(200.0, 0.1) > 4.0);
        assert!(rig.sine_gain_db(1250.0, 0.1) < -6.0);
        let mut rig = Rig::new("BODY");
        rig.set("body", 0.0);
        for f in [100.0, 1250.0, 10_000.0] {
            assert!(rig.sine_gain_db(f, 0.1).abs() < 0.05);
        }
    }
}
