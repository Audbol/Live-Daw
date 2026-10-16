//! Velvet Imager: enhances or reduces the stereo content of a signal (WING
//! manual 11.6), keeping it mono compatible.
//!
//! Everything happens on the side (L-R) signal, so the mono sum is never
//! touched. MS-WIDTH (-1..1) changes the ratio of side to mid, from mono
//! through unchanged to double width. STEREOIZE (the STEREO control) adds an
//! artificial stereo impression: in K-STEREO mode (after a mastering
//! engineer's technique) subtle early reflections of the signal, band-limited,
//! go into the side signal in alternating polarity; in VELVET mode a sparse
//! velvet-noise filter randomly shifts the phase between the channels. Both
//! widen mono sources too. DEEP makes STEREOIZE more pronounced: twice the
//! amount, reaching down to 100 Hz instead of 250 Hz. GAIN is the output
//! level.

use crate::wing::fx::blocks::{db_to_gain, DelayLine, Noise, OnePole, Smoothed};
use crate::wing::fx::FxAlgo;
use crate::wing::Params;

/// Velvet filter: length and pulse density.
const VELVET_MS: f32 = 30.0;
const VELVET_DENSITY: f32 = 1200.0;
/// K-Stereo early reflections: (ms, gain) taps of the mid signal ...
const REFLECTIONS: [(f32, f32); 4] = [(7.3, 0.55), (11.9, -0.45), (17.1, 0.35), (23.7, -0.28)];
/// ... and of the side signal (the ambience already there).
const AMBIENCE_TAPS: [(f32, f32); 2] = [(11.0, 0.5), (17.0, -0.35)];

pub(crate) struct VelvetImager {
    mid_line: DelayLine,
    side_line: DelayLine,
    /// Velvet-noise taps: (delay in samples, gain).
    taps: Vec<(usize, f32)>,
    reflections: [(usize, f32); 4],
    ambience: [(usize, f32); 2],
    /// Band-limits of the synthesised side: high-passes (via low-pass
    /// subtraction) at 250 Hz and, with DEEP, 100 Hz; low-pass at 7 kHz.
    synth_hp: OnePole,
    deep_hp: OnePole,
    synth_lp: OnePole,
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
        let sm = |v: f32| Smoothed::new(v, 30.0, sr);
        Self {
            mid_line: DelayLine::new(len.max(ms(25.0)) + 2),
            side_line: DelayLine::new(ms(20.0) + 2),
            taps,
            reflections: REFLECTIONS.map(|(m, g)| (ms(m), g)),
            ambience: AMBIENCE_TAPS.map(|(m, g)| (ms(m), g)),
            synth_hp: OnePole::new(250.0, sr),
            deep_hp: OnePole::new(100.0, sr),
            synth_lp: OnePole::new(7000.0, sr),
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

            // Artificial stereo: early reflections or velvet-noise decorrelation.
            let mut synth = 0.0;
            if velvet < 1.0 {
                let refl: f32 = self.reflections.iter().map(|&(d, g)| g * self.mid_line.tap(d)).sum();
                let amb: f32 = self.ambience.iter().map(|&(d, g)| g * self.side_line.tap(d)).sum();
                synth += (1.0 - velvet) * (refl + amb);
            }
            if velvet > 0.0 {
                let dec: f32 = self.taps.iter().map(|&(d, g)| g * self.mid_line.tap(d)).sum();
                synth += velvet * 0.5 * dec;
            }
            let normal = synth - self.synth_hp.lp(synth);
            let deeper = synth - self.deep_hp.lp(synth);
            let synth = self.synth_lp.lp(normal + deep * (2.0 * deeper - normal));

            let s = side * width + stereo * synth;

            *l = (mid + s) * gain;
            *r = (mid - s) * gain;
        }
    }

    fn reset(&mut self) {
        self.mid_line.clear();
        self.side_line.clear();
        self.synth_hp.reset();
        self.deep_hp.reset();
        self.synth_lp.reset();
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
    fn both_modes_widen_mono_and_deep_widens_more() {
        let side_energy = |mode: f32, deep: f32| {
            let mut rig = Rig::new("V-IMG");
            rig.set("st", 100.0).set("mode", mode).set("deep", deep);
            let n = 9600;
            let x: Vec<f32> = (0..n).map(|i| ((i * 7919) % 997) as f32 / 997.0 - 0.5).collect();
            let (mut l, mut r) = (x.clone(), x.clone());
            rig.run(&mut l, &mut r);
            l.iter().zip(&r).map(|(a, b)| (a - b) * (a - b)).sum::<f32>()
        };
        for mode in [0.0, 1.0] {
            let plain = side_energy(mode, 0.0);
            assert!(plain > 1.0, "mode {mode}: {plain}");
            assert!(side_energy(mode, 1.0) > 2.0 * plain, "mode {mode}");
        }
    }
}
