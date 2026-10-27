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
//!
//! Readouts (the screen's meters and scope; the slot meters the input and
//! output L/R): 0 the output's mid and 1 its side level (linear peaks), 2 the
//! output's correlation (-1..1 over about 300 ms; 0 in silence), then
//! [`SCOPE_POINTS`] points of the LF VECTOR SCOPE (side, mid pairs from 3 on:
//! the output below 500 Hz, one point every 1.5 ms, oldest first).

use crate::wing::fx::blocks::{coef, db_to_gain, DelayLine, Noise, OnePole, Smoothed};
use crate::wing::fx::meters::PeakMeter;
use crate::wing::fx::{FxAlgo, FX_READOUTS};
use crate::wing::Params;

/// Velvet filter: length and pulse density.
const VELVET_MS: f32 = 30.0;
const VELVET_DENSITY: f32 = 1200.0;
/// K-Stereo early reflections: (ms, gain) taps of the mid signal ...
const REFLECTIONS: [(f32, f32); 4] = [(7.3, 0.55), (11.9, -0.45), (17.1, 0.35), (23.7, -0.28)];
/// ... and of the side signal (the ambience already there).
const AMBIENCE_TAPS: [(f32, f32); 2] = [(11.0, 0.5), (17.0, -0.35)];

/// Points of the LF vector scope in the readouts.
pub(crate) const SCOPE_POINTS: usize = 20;
const SCOPE_MS: f32 = 1.5;

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
    // The screen: mid / side levels, correlation and the scope.
    mid_meter: PeakMeter,
    side_meter: PeakMeter,
    /// Running means of L*R, L^2 and R^2.
    corr: [f32; 3],
    corr_coef: f32,
    scope_lp: [OnePole; 2],
    scope: [(f32, f32); SCOPE_POINTS],
    scope_pos: usize,
    scope_every: usize,
    scope_count: usize,
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
            mid_meter: PeakMeter::new(sr),
            side_meter: PeakMeter::new(sr),
            corr: [0.0; 3],
            corr_coef: coef(300.0 / 4.6, sr),
            scope_lp: [OnePole::new(500.0, sr), OnePole::new(500.0, sr)],
            scope: [(0.0, 0.0); SCOPE_POINTS],
            scope_pos: 0,
            scope_every: ((SCOPE_MS * 0.001 * sr) as usize).max(1),
            scope_count: 0,
            primed: false,
        }
    }

    /// The meters, correlation and scope for one output sample.
    #[inline]
    fn measure(&mut self, l: f32, r: f32) {
        let (mid, side) = (0.5 * (l + r), 0.5 * (l - r));
        self.mid_meter.tick(mid);
        self.side_meter.tick(side);
        let k = self.corr_coef;
        for (c, x) in self.corr.iter_mut().zip([l * r, l * l, r * r]) {
            *c += (x - *c) * k;
        }
        let (sm, ss) = (self.scope_lp[0].lp(mid), self.scope_lp[1].lp(side));
        self.scope_count += 1;
        if self.scope_count >= self.scope_every {
            self.scope_count = 0;
            self.scope[self.scope_pos] = (ss, sm);
            self.scope_pos = (self.scope_pos + 1) % SCOPE_POINTS;
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
            self.measure(*l, *r);
        }
    }

    fn readouts(&self, out: &mut [f32; FX_READOUTS]) {
        out[0] = self.mid_meter.level();
        out[1] = self.side_meter.level();
        let [lr, ll, rr] = self.corr;
        let norm = (ll * rr).sqrt();
        out[2] = if norm > 1e-10 { (lr / norm).clamp(-1.0, 1.0) } else { 0.0 };
        for k in 0..SCOPE_POINTS {
            let (x, y) = self.scope[(self.scope_pos + k) % SCOPE_POINTS];
            out[3 + 2 * k] = x;
            out[4 + 2 * k] = y;
        }
    }

    fn reset(&mut self) {
        self.mid_line.clear();
        self.side_line.clear();
        self.synth_hp.reset();
        self.deep_hp.reset();
        self.synth_lp.reset();
        self.mid_meter.reset();
        self.side_meter.reset();
        self.corr = [0.0; 3];
        self.scope_lp.iter_mut().for_each(OnePole::reset);
        self.scope = [(0.0, 0.0); SCOPE_POINTS];
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

    fn readouts(rig: &Rig) -> [f32; crate::wing::fx::FX_READOUTS] {
        let mut r = [0.0; crate::wing::fx::FX_READOUTS];
        rig.dev.readouts(&mut r);
        r
    }

    #[test]
    fn width_gain_and_the_meters() {
        use std::f32::consts::TAU;
        let n = 24_000;
        let l0: Vec<f32> = (0..n).map(|i| (TAU * 200.0 * i as f32 / 48_000.0).sin() * 0.5).collect();
        let r0: Vec<f32> = (0..n).map(|i| (TAU * 200.0 * i as f32 / 48_000.0 + 1.0).sin() * 0.3).collect();
        let run = |set: &[(&str, f32)]| {
            let mut rig = Rig::new("V-IMG");
            rig.set("st", 0.0);
            for (id, v) in set {
                rig.set(id, *v);
            }
            let (mut l, mut r) = (l0.clone(), r0.clone());
            rig.run(&mut l, &mut r);
            (l, r, readouts(&rig))
        };
        // MS-WIDTH -1: mono (side gone, correlation +1); +1: double side.
        let (l, r, ro) = run(&[("wid", -1.0)]);
        assert!(l.iter().zip(&r).skip(4800).all(|(a, b)| (a - b).abs() < 1e-3));
        assert!(ro[1] < 1e-3 && ro[0] > 0.2 && ro[2] > 0.99, "{:?}", &ro[..3]);
        let (_, _, wide) = run(&[("wid", 1.0)]);
        let (_, _, same) = run(&[("wid", 0.0)]);
        assert!((wide[1] / same[1] - 2.0).abs() < 0.1 && (wide[0] - same[0]).abs() < 1e-3);
        assert!(wide[2] < same[2], "wider is less correlated");
        // Opposite polarity reads -1.
        let mut rig = Rig::new("V-IMG");
        rig.set("st", 0.0);
        let (mut l, mut r): (Vec<f32>, Vec<f32>) = (l0.clone(), l0.iter().map(|x| -x).collect());
        rig.run(&mut l, &mut r);
        assert!(readouts(&rig)[2] < -0.99);
        // GAIN.
        let (l, _, _) = run(&[("g", 6.0)]);
        let (l1, _, _) = run(&[]);
        let peak = |v: &[f32]| v[4800..].iter().fold(0.0f32, |a, x| a.max(x.abs()));
        assert!((20.0 * (peak(&l) / peak(&l1)).log10() - 6.0).abs() < 0.2);
        // The scope: a mono signal draws a vertical line (side 0), mid moving.
        let mut rig = Rig::new("V-IMG");
        rig.set("st", 0.0);
        let (mut l, mut r) = (l0.clone(), l0.clone());
        rig.run(&mut l, &mut r);
        let ro = readouts(&rig);
        let pts: Vec<(f32, f32)> = (0..super::SCOPE_POINTS).map(|k| (ro[3 + 2 * k], ro[4 + 2 * k])).collect();
        assert!(pts.iter().all(|p| p.0.abs() < 1e-4));
        assert!(pts.iter().map(|p| p.1.abs()).fold(0.0, f32::max) > 0.1);
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
