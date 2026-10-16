//! Velvet Imager: a mono-compatible stereo imager.
//!
//! Everything happens on the side (L-R) signal, so the mono sum is never
//! touched. WIDTH (-1..1) scales the existing side signal from mono through
//! unchanged to double width. STEREO adds synthesised width: in K-STEREO mode
//! it recovers the ambience already in the recording (the side signal, delayed
//! and band-limited, added back, after Bob Katz's K-Stereo); in VELVET mode it
//! decorrelates the mid signal with a sparse velvet-noise filter, which widens
//! even mono sources. DEEP widens the bass: it lifts the low side signal and
//! adds an all-pass shifted copy of the low mid signal to the side. GAIN is the
//! output level.

use crate::wing::fx::blocks::{db_to_gain, Biquad, BiquadCoeffs, DelayLine, Noise, OnePole, Smoothed};
use crate::wing::fx::FxAlgo;
use crate::wing::Params;

/// Velvet filter: length and pulse density.
const VELVET_MS: f32 = 30.0;
const VELVET_DENSITY: f32 = 1200.0;
/// K-Stereo ambience taps (ms) and gains.
const AMBIENCE_TAPS: [(f32, f32); 2] = [(11.0, 0.7), (17.0, -0.5)];

pub(crate) struct VelvetImager {
    mid_line: DelayLine,
    side_line: DelayLine,
    /// Velvet-noise taps: (delay in samples, gain).
    taps: Vec<(usize, f32)>,
    ambience: [(usize, f32); 2],
    /// Band-limits of the synthesised side: high-pass (via low-pass subtraction) and low-pass.
    amb_hp: OnePole,
    amb_lp: OnePole,
    deep_lp_side: OnePole,
    deep_lp_mid: Biquad,
    deep_ap: Biquad,
    width: Smoothed,
    stereo: Smoothed,
    velvet: Smoothed,
    deep: Smoothed,
    gain: Smoothed,
    primed: bool,
}

impl VelvetImager {
    pub(crate) fn new(sr: f32) -> Self {
        let len = (VELVET_MS * 0.001 * sr) as usize;
        let spacing = (sr / VELVET_DENSITY).max(2.0);
        let mut noise = Noise(0x5EED_1234);
        let mut taps = Vec::new();
        let mut pos = 0.0f32;
        while (pos as usize) < len {
            let at = pos as usize + ((noise.sample() * 0.5 + 0.5) * (spacing - 1.0)) as usize;
            let sign = if noise.sample() >= 0.0 { 1.0 } else { -1.0 };
            // Decays across the filter so the decorrelated copy stays tight.
            let decay = (-2.5 * at as f32 / len as f32).exp();
            taps.push((at.max(1), sign * decay));
            pos += spacing;
        }
        let energy: f32 = taps.iter().map(|(_, g)| g * g).sum::<f32>().max(1e-9);
        for t in &mut taps {
            t.1 /= energy.sqrt();
        }
        let ms = |m: f32| (m * 0.001 * sr) as usize;
        let mut deep_lp_mid = Biquad::default();
        deep_lp_mid.set_coefficients(BiquadCoeffs::low_pass(sr as f64, 150.0, 0.7));
        let mut deep_ap = Biquad::default();
        deep_ap.set_coefficients(BiquadCoeffs::all_pass_2(sr as f64, 90.0, 0.7));
        let sm = |v: f32| Smoothed::new(v, 30.0, sr);
        Self {
            mid_line: DelayLine::new(len + 2),
            side_line: DelayLine::new(ms(20.0) + 2),
            taps,
            ambience: AMBIENCE_TAPS.map(|(m, g)| (ms(m), g)),
            amb_hp: OnePole::new(250.0, sr),
            amb_lp: OnePole::new(7000.0, sr),
            deep_lp_side: OnePole::new(180.0, sr),
            deep_lp_mid,
            deep_ap,
            width: sm(1.0),
            stereo: sm(0.0),
            velvet: sm(1.0),
            deep: sm(0.0),
            gain: sm(1.0),
            primed: false,
        }
    }

    fn smoothers(&mut self) -> [&mut Smoothed; 5] {
        [&mut self.width, &mut self.stereo, &mut self.velvet, &mut self.deep, &mut self.gain]
    }
}

impl FxAlgo for VelvetImager {
    fn set_params(&mut self, p: &Params) {
        self.width.target = 1.0 + p.f("wid");
        self.stereo.target = p.f("st") / 100.0;
        self.velvet.target = if p.choice("mode") == 1 { 1.0 } else { 0.0 };
        self.deep.target = if p.on("deep") { 1.0 } else { 0.0 };
        self.gain.target = db_to_gain(p.f("g"));
        if !self.primed {
            for s in self.smoothers() {
                s.snap(s.target);
            }
            self.primed = true;
        }
    }

    fn process(&mut self, left: &mut [f32], right: &mut [f32]) {
        for (l, r) in left.iter_mut().zip(right.iter_mut()) {
            let mid = 0.5 * (*l + *r);
            let side = 0.5 * (*l - *r);
            self.mid_line.push(mid);
            self.side_line.push(side);

            let width = self.width.tick();
            let stereo = self.stereo.tick();
            let velvet = self.velvet.tick();
            let deep = self.deep.tick();
            let gain = self.gain.tick();

            // Synthesised side: recovered ambience or decorrelated mid.
            let mut synth = 0.0;
            if velvet < 1.0 {
                let amb: f32 = self.ambience.iter().map(|&(d, g)| g * self.side_line.tap(d)).sum();
                synth += (1.0 - velvet) * 1.2 * amb;
            }
            if velvet > 0.0 {
                let dec: f32 = self.taps.iter().map(|&(d, g)| g * self.mid_line.tap(d)).sum();
                synth += velvet * 0.5 * dec;
            }
            let synth = self.amb_lp.lp(synth - self.amb_hp.lp(synth));

            let mut s = side * width + stereo * synth;
            // DEEP: wider low end, still mono compatible.
            let low_side = self.deep_lp_side.lp(side);
            let low_mid = self.deep_ap.tick(self.deep_lp_mid.tick(mid, 0), 0);
            s += deep * (low_side + 0.4 * low_mid);

            *l = (mid + s) * gain;
            *r = (mid - s) * gain;
        }
    }

    fn reset(&mut self) {
        self.mid_line.clear();
        self.side_line.clear();
        self.amb_hp.reset();
        self.amb_lp.reset();
        self.deep_lp_side.reset();
        self.deep_lp_mid.reset();
        self.deep_ap.reset();
        for s in self.smoothers() {
            s.snap(s.target);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::super::tests::Rig;

    #[test]
    fn mono_sum_is_untouched() {
        for (mode, deep, wid, st) in [(0.0, 0.0, 1.0, 100.0), (1.0, 1.0, -1.0, 50.0), (1.0, 1.0, 0.7, 100.0)] {
            let mut rig = Rig::new("V-IMG");
            rig.set("mode", mode).set("deep", deep).set("wid", wid).set("st", st);
            let n = 9600;
            let l0: Vec<f32> = (0..n).map(|i| ((i * 7919) % 997) as f32 / 997.0 - 0.5).collect();
            let r0: Vec<f32> = (0..n).map(|i| ((i * 104_729) % 991) as f32 / 991.0 - 0.5).collect();
            let (mut l, mut r) = (l0.clone(), r0.clone());
            rig.run(&mut l, &mut r);
            for i in 0..n {
                assert!(((l[i] + r[i]) - (l0[i] + r0[i])).abs() < 1e-5, "mode {mode}: sample {i}");
            }
        }
    }

    #[test]
    fn velvet_widens_mono() {
        let mut rig = Rig::new("V-IMG");
        rig.set("st", 100.0);
        let n = 9600;
        let x: Vec<f32> = (0..n).map(|i| ((i * 7919) % 997) as f32 / 997.0 - 0.5).collect();
        let (mut l, mut r) = (x.clone(), x.clone());
        rig.run(&mut l, &mut r);
        let side: f32 = l.iter().zip(&r).map(|(a, b)| (a - b) * (a - b)).sum();
        assert!(side > 1.0, "{side}");
    }
}
