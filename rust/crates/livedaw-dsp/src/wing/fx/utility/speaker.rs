//! Speaker Manager: a loudspeaker processor.
//!
//! Signal flow: crossover high- and low-pass (HP/LP FREQ and TYPE: FLAT or
//! Butterworth/Bessel/Linkwitz-Riley at 6..48 dB/oct) -> TILT (shelves of
//! +-TILT/2 around TILT F) -> PHASE -> INVERT -> dynamic EQ band -> LIMITER ->
//! alignment delay.
//!
//! PHASE (0..180 degrees) is a first-order all-pass that shifts the phase by
//! exactly that much at the crossover frequency (the HP frequency when the HP
//! is on, else the LP frequency when the LP is on, else 1 kHz): 0 is no
//! filter, 180 approaches a polarity flip. DIST (0..5 m) delays both channels
//! by the distance at 343 m/s; POS (-5..5 m) adds a delay to one side
//! (positive delays the left, negative the right) to centre the image.
//! The dynamic EQ band works like a Triple DEQ band. The limiter is a
//! feed-forward stereo-linked limiter at LIM THR, on peaks or RMS (LIMRMS),
//! with RELEASE. `meter()` shows the deeper of the limiter's and the dynamic
//! EQ's reductions.

use super::dyneq::{DynEqBand, DynEqSettings};
use super::{db_gain, level_db, power_db, push_cut, sub_blocks, Cascade, Design, Glide};
use crate::wing::fx::blocks::{coef, BiquadCoeffs, DelayLine, Smoothed};
use crate::wing::fx::FxAlgo;
use crate::wing::Params;
use std::f32::consts::PI;

const SPEED_OF_SOUND: f32 = 343.0;
/// Longest alignment delay: DIST + |POS|.
const MAX_DIST_M: f32 = 10.0;
const DEQ_IDS: [&str; 9] = ["deqthr", "deqratio", "deqatt", "deqrel", "deqfilt", "deqg", "deqf", "deqq", "deqmode"];

/// First-order all-pass on two channels, `a` = coefficient.
#[derive(Clone, Copy, Default)]
struct PhaseShift {
    a: f32,
    x1: [f32; 2],
    y1: [f32; 2],
}

impl PhaseShift {
    #[inline]
    fn tick(&mut self, x: f32, ch: usize) -> f32 {
        let y = self.a * x + self.x1[ch] - self.a * self.y1[ch];
        self.x1[ch] = x;
        self.y1[ch] = y;
        y
    }

    fn reset(&mut self) {
        self.x1 = [0.0; 2];
        self.y1 = [0.0; 2];
    }
}

pub(crate) struct SpeakerManager {
    sr: f32,
    filters: Cascade<10>,
    hp_kind: &'static str,
    lp_kind: &'static str,
    /// ln(HP Hz), ln(LP Hz), ln(TILT F), TILT dB, PHASE degrees.
    glide: Glide<5>,
    phase: PhaseShift,
    phase_on: bool,
    polarity: Smoothed,
    delay: [DelayLine; 2],
    delay_samples: [Smoothed; 2],
    dyn_eq: DynEqBand,
    dyn_on: Smoothed,
    lim_on: Smoothed,
    lim_thr: f32,
    lim_rms: bool,
    lim_rel: f32,
    lim_att: [f32; 2],
    lim_ms: f32,
    lim_ms_coef: f32,
    lim_gr: f32,
    meter: f32,
    primed: bool,
}

impl SpeakerManager {
    pub(crate) fn new(sr: f32) -> Self {
        let max_delay = (MAX_DIST_M / SPEED_OF_SOUND * sr) as usize + 4;
        let sm = |v: f32, ms: f32| Smoothed::new(v, ms, sr);
        let mut s = Self {
            sr,
            filters: Cascade::new(),
            hp_kind: "FLAT",
            lp_kind: "FLAT",
            glide: Glide::new(0.0),
            phase: PhaseShift::default(),
            phase_on: false,
            polarity: sm(1.0, 5.0),
            delay: [DelayLine::new(max_delay), DelayLine::new(max_delay)],
            delay_samples: [sm(0.0, 50.0), sm(0.0, 50.0)],
            dyn_eq: DynEqBand::new(sr),
            dyn_on: sm(0.0, 10.0),
            lim_on: sm(0.0, 10.0),
            lim_thr: -6.0,
            lim_rms: false,
            lim_rel: coef(200.0, sr),
            lim_att: [coef(0.3, sr), coef(5.0, sr)],
            lim_ms: 0.0,
            lim_ms_coef: coef(10.0, sr),
            lim_gr: 0.0,
            meter: 0.0,
            primed: false,
        };
        s.glide.tgt = [20f32.ln(), 20_000f32.ln(), 1000f32.ln(), 0.0, 0.0];
        s.glide.snap();
        s.redesign();
        s
    }

    fn redesign(&mut self) {
        let sr = self.sr as f64;
        let [hp, lp, tf, tilt, deg] = self.glide.cur;
        let (hp, lp, tf) = (hp.exp() as f64, lp.exp() as f64, tf.exp() as f64);
        let mut d = Design::<10>::new();
        push_cut(&mut d, sr, self.hp_kind, hp, true);
        push_cut(&mut d, sr, self.lp_kind, lp, false);
        if tilt != 0.0 {
            d.push(BiquadCoeffs::low_shelf(sr, tf, 0.5, -0.5 * tilt as f64));
            d.push(BiquadCoeffs::high_shelf(sr, tf, 0.5, 0.5 * tilt as f64));
        }
        self.filters.set(d.sections());

        // Phase: a first-order all-pass with `deg` of lag at the crossover frequency.
        let on = deg >= 0.25;
        if on != self.phase_on {
            self.phase.reset();
            self.phase_on = on;
        }
        if on {
            let fref = if self.hp_kind != "FLAT" {
                hp
            } else if self.lp_kind != "FLAT" {
                lp
            } else {
                1000.0
            } as f32;
            let wref = (PI * fref.min(self.sr * 0.45) / self.sr).tan();
            let t = (0.5 * deg.min(179.0).to_radians()).tan() / wref;
            self.phase.a = (1.0 - t) / (1.0 + t);
        }
    }

    /// Feed-forward limiter gain for one stereo sample.
    #[inline]
    fn limiter(&mut self, l: f32, r: f32) -> f32 {
        let lvl = if self.lim_rms {
            let x = 0.5 * (l * l + r * r);
            self.lim_ms += (x - self.lim_ms) * self.lim_ms_coef;
            power_db(self.lim_ms) + 3.0
        } else {
            level_db(l.abs().max(r.abs()))
        };
        let target = (self.lim_thr - lvl).min(0.0);
        let c = if target < self.lim_gr { self.lim_att[self.lim_rms as usize] } else { self.lim_rel };
        self.lim_gr += (target - self.lim_gr) * c;
        self.lim_gr
    }
}

impl FxAlgo for SpeakerManager {
    fn set_params(&mut self, p: &Params) {
        let (hp_kind, lp_kind) = (p.text("hptype"), p.text("lptype"));
        let structure = hp_kind != self.hp_kind || lp_kind != self.lp_kind;
        self.hp_kind = hp_kind;
        self.lp_kind = lp_kind;
        self.glide.tgt = [p.f("hpf").ln(), p.f("lpf").ln(), p.f("tiltf").ln(), p.f("tiltg"), p.f("phase")];
        self.polarity.target = if p.on("invert") { -1.0 } else { 1.0 };
        let pos = p.f("pos");
        let to_samples = |m: f32| m / SPEED_OF_SOUND * self.sr;
        self.delay_samples[0].target = to_samples(p.f("dist") + pos.max(0.0));
        self.delay_samples[1].target = to_samples(p.f("dist") + (-pos).max(0.0));
        self.dyn_eq.set(&DynEqSettings::read(p, &DEQ_IDS));
        self.dyn_on.target = if p.on("dyneq") { 1.0 } else { 0.0 };
        self.lim_on.target = if p.on("lim") { 1.0 } else { 0.0 };
        self.lim_thr = p.f("limthr");
        self.lim_rms = p.on("limrms");
        self.lim_rel = coef(p.f("limrel"), self.sr);
        if !self.primed {
            self.glide.snap();
            self.dyn_eq.snap();
            for s in [&mut self.polarity, &mut self.dyn_on, &mut self.lim_on] {
                s.snap(s.target);
            }
            for s in &mut self.delay_samples {
                s.snap(s.target);
            }
            self.primed = true;
            self.redesign();
        } else if structure {
            self.redesign();
        }
    }

    fn process(&mut self, left: &mut [f32], right: &mut [f32]) {
        let n = left.len().min(right.len());
        let mut deepest = 0.0f32;
        sub_blocks(n, |a, b| {
            if self.glide.step() != 0 {
                self.redesign();
            }
            let (l, r) = (&mut left[a..b], &mut right[a..b]);
            self.filters.process(l, r);
            if self.phase_on {
                for (x, y) in l.iter_mut().zip(r.iter_mut()) {
                    *x = self.phase.tick(*x, 0);
                    *y = self.phase.tick(*y, 1);
                }
            }
            for (x, y) in l.iter_mut().zip(r.iter_mut()) {
                let pol = self.polarity.tick();
                *x *= pol;
                *y *= pol;
            }
            // Dynamic EQ, faded in and out with its switch.
            if self.dyn_on.value > 0.0 || self.dyn_on.target > 0.0 {
                let mut wet_l = [0.0f32; super::SUB_BLOCK];
                let mut wet_r = [0.0f32; super::SUB_BLOCK];
                let m = l.len();
                wet_l[..m].copy_from_slice(l);
                wet_r[..m].copy_from_slice(r);
                deepest = deepest.min(self.dyn_eq.process(&mut wet_l[..m], &mut wet_r[..m]));
                for i in 0..m {
                    let w = self.dyn_on.tick();
                    l[i] += w * (wet_l[i] - l[i]);
                    r[i] += w * (wet_r[i] - r[i]);
                }
            }
            if self.lim_on.value > 0.0 || self.lim_on.target > 0.0 {
                for (x, y) in l.iter_mut().zip(r.iter_mut()) {
                    let gr = self.limiter(*x, *y);
                    deepest = deepest.min(gr);
                    let g = 1.0 + self.lim_on.tick() * (db_gain(gr) - 1.0);
                    *x *= g;
                    *y *= g;
                }
            }
            for (x, y) in l.iter_mut().zip(r.iter_mut()) {
                self.delay[0].push(*x);
                self.delay[1].push(*y);
                *x = self.delay[0].tap_linear(self.delay_samples[0].tick());
                *y = self.delay[1].tap_linear(self.delay_samples[1].tick());
            }
        });
        self.meter = deepest;
    }

    fn reset(&mut self) {
        self.filters.reset();
        self.phase.reset();
        for d in &mut self.delay {
            d.clear();
        }
        self.dyn_eq.reset();
        self.lim_ms = 0.0;
        self.lim_gr = 0.0;
        self.meter = 0.0;
        self.glide.snap();
        for s in [&mut self.polarity, &mut self.dyn_on, &mut self.lim_on] {
            s.snap(s.target);
        }
        for s in &mut self.delay_samples {
            s.snap(s.target);
        }
        self.redesign();
    }

    fn meter(&self) -> f32 {
        self.meter
    }
}

#[cfg(test)]
mod tests {
    use super::super::tests::Rig;

    #[test]
    fn defaults_are_transparent() {
        let mut rig = Rig::new("SPKMAN");
        let x: Vec<f32> = (0..4800).map(|i| ((i * 7919) % 997) as f32 / 997.0 - 0.5).collect();
        let (mut l, mut r) = (x.clone(), x.clone());
        rig.run(&mut l, &mut r);
        assert_eq!(l, x);
    }

    #[test]
    fn crossover_and_alignment() {
        let mut rig = Rig::new("SPKMAN");
        rig.set("hpf", 200.0).set("hptype", 8.0); // LR24
        assert!(rig.sine_gain_db(50.0, 0.1) < -20.0);
        assert!(rig.sine_gain_db(2000.0, 0.1).abs() < 0.2);

        // 3.43 m = 10 ms.
        let mut rig = Rig::new("SPKMAN");
        rig.set("dist", 3.43);
        let mut l = vec![0.0f32; 1024];
        let mut r = vec![0.0f32; 1024];
        l[0] = 1.0;
        r[0] = 1.0;
        rig.run(&mut l, &mut r);
        let at = l.iter().position(|x| x.abs() > 0.5).unwrap();
        assert_eq!(at, 480);
    }

    #[test]
    fn phase_and_limiter() {
        // 180 degrees at 1 kHz (no crossover) is close to a polarity flip there.
        let mut rig = Rig::new("SPKMAN");
        rig.set("phase", 180.0);
        assert!(rig.sine_gain_db(1000.0, 0.1).abs() < 0.01);

        let mut rig = Rig::new("SPKMAN");
        rig.set("lim", 1.0).set("limthr", -12.0);
        let g = rig.sine_gain_db(1000.0, 0.9);
        assert!(g < -8.0, "{g}");
        assert!(rig.dev.meter() < -8.0);
    }
}
