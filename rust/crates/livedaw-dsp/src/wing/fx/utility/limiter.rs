//! Precision Limiter: precise peak control against digital clipping (WING
//! manual 11.2): a brickwall limiter at 0 dBFS with look-ahead.
//!
//! IN GAIN drives the limiter; OUT GAIN is applied after limiting, so it sets
//! the output peak level. SQUEEZE raises the perceived loudness by blending in
//! a soft saturator ahead of the limiter, which rounds peaks so the limiter
//! works less (high settings distort audibly, as on the unit). KNEE goes from
//! a hard knee (0) to a wide soft knee (10: the onset starts 5 dB under full
//! scale). ATTACK (50 us .. 1 ms) is the length of the gain ramp ahead of each
//! peak; RELEASE (20 ms .. 2 s) the recovery time. AUTO GAIN adds a long-term
//! gain detection ahead of the limiter: a slow automatic gain control that
//! brings the program's long-term level (RMS over seconds, ignoring silence)
//! toward -18 dBFS, within +-12 dB, so material with a wide dynamic range hits
//! the limiter evenly and IN GAIN sets how hard.
//!
//! How 0 dBFS is held: the gain each sample needs is held over the whole
//! look-ahead window (a sliding minimum), released exponentially, and then
//! averaged over the attack length. Every averaged value is at most the gain
//! the delayed sample needs, so the output never exceeds full scale, and the
//! averaging turns each reduction into a smooth ramp that ends on the peak.
//! The audio is delayed by the longest attack (1 ms), a fixed latency the
//! device reports (the slot compensates it).
//!
//! Readouts: 0 the gain reduction (dB, <= 0: the gain the limiter applies
//! now), 1 AUTO GAIN's correction (dB, signed).

use super::{db_gain, power_db};
use crate::wing::fx::blocks::{coef, db_to_gain, fast_tanh, gain_to_db, DelayLine, Smoothed};
use crate::wing::fx::{FxAlgo, FX_READOUTS};
use crate::wing::Params;

pub(crate) struct PrecisionLimiter {
    sr: f32,
    /// Look-ahead (and latency) in samples: the longest ATTACK.
    look: usize,
    delay: [DelayLine; 2],
    // Sliding minimum of the required gain over `look + 1` samples (monotonic queue).
    min_val: Vec<f32>,
    min_at: Vec<usize>,
    min_mask: usize,
    head: usize,
    tail: usize,
    count: usize,
    // History of the released gain and the moving average over `ramp` samples.
    hist: Vec<f32>,
    hist_mask: usize,
    hist_pos: usize,
    ramp: usize,
    sum: f64,
    released: f32,
    rel_coef: f32,
    in_gain: Smoothed,
    out_gain: Smoothed,
    squeeze: Smoothed,
    // AUTO GAIN: long-term level (mean square) and the current correction in dB.
    agc_on: bool,
    agc_ms: f32,
    agc_ms_coef: f32,
    agc_slow: f32,
    agc_db: f32,
    agc_rise: f32,
    agc_fall: f32,
    agc_move: f32,
    agc_off: f32,
    knee_db: f32,
    reduction_db: f32,
    primed: bool,
}

impl PrecisionLimiter {
    pub(crate) fn new(sr: f32) -> Self {
        let look = ((sr / 1000.0).ceil() as usize).max(1);
        let min_len = (look + 2).next_power_of_two();
        let mut s = Self {
            sr,
            look,
            delay: [DelayLine::new(look + 1), DelayLine::new(look + 1)],
            min_val: vec![1.0; min_len],
            min_at: vec![0; min_len],
            min_mask: min_len - 1,
            head: 0,
            tail: 0,
            count: 0,
            hist: vec![1.0; min_len],
            hist_mask: min_len - 1,
            hist_pos: 0,
            ramp: 1,
            sum: 1.0,
            released: 1.0,
            rel_coef: coef(200.0, sr),
            in_gain: Smoothed::new(1.0, 20.0, sr),
            out_gain: Smoothed::new(1.0, 20.0, sr),
            squeeze: Smoothed::new(0.0, 20.0, sr),
            agc_on: false,
            agc_ms: 0.0,
            agc_ms_coef: coef(200.0, sr),
            agc_slow: 0.0,
            agc_db: 0.0,
            agc_rise: coef(300.0, sr),
            agc_fall: coef(3000.0, sr),
            agc_move: coef(1000.0, sr),
            agc_off: coef(100.0, sr),
            knee_db: 2.0,
            reduction_db: 0.0,
            primed: false,
        };
        s.reset();
        s
    }

    /// Gain (<= 1) that brings a peak of `peak` under full scale with the soft knee.
    #[inline]
    fn required_gain(&self, peak: f32) -> f32 {
        let k = self.knee_db;
        if k <= 0.0 {
            return if peak > 1.0 { 1.0 / peak } else { 1.0 };
        }
        // The knee spans -k/2 .. +k/2 dBFS; the output never passes 0 dBFS.
        if peak <= db_to_gain(-0.5 * k) {
            return 1.0;
        }
        let x = gain_to_db(peak);
        let over = x + 0.5 * k;
        let y = if over >= k { 0.0 } else { x - over * over / (2.0 * k) };
        db_to_gain(y - x).min(1.000_001 / peak.max(1e-9)).min(1.0)
    }

    /// AUTO GAIN: the long-term correction (dB) for one input sample.
    #[inline]
    fn auto_gain(&mut self, l: f32, r: f32) -> f32 {
        /// Long-term level the correction aims for, its range and the level below which it holds.
        const REF_DB: f32 = -18.0;
        const RANGE_DB: f32 = 12.0;
        const GATE_MS: f32 = 1e-5; // -50 dBFS
        if self.agc_on {
            let x = 0.5 * (l * l + r * r);
            // Mean square, then a long-term level that rises in ~0.3 s and falls in ~3 s.
            self.agc_ms += (x - self.agc_ms) * self.agc_ms_coef;
            let ms = self.agc_ms;
            self.agc_slow += (ms - self.agc_slow) * if ms > self.agc_slow { self.agc_rise } else { self.agc_fall };
            if self.agc_slow > GATE_MS {
                let want = (REF_DB - power_db(self.agc_slow)).clamp(-RANGE_DB, RANGE_DB);
                self.agc_db += (want - self.agc_db) * self.agc_move;
            }
        } else if self.agc_db != 0.0 {
            self.agc_db -= self.agc_db * self.agc_off;
            if self.agc_db.abs() < 1e-4 {
                self.agc_db = 0.0;
            }
        }
        self.agc_db
    }

    /// Sum of the last `ramp` released gains.
    fn resum(&mut self) {
        self.sum = (0..self.ramp).map(|i| self.hist[self.hist_pos.wrapping_sub(i) & self.hist_mask] as f64).sum();
    }
}

impl FxAlgo for PrecisionLimiter {
    fn set_params(&mut self, p: &Params) {
        self.in_gain.target = db_to_gain(p.f("gin"));
        self.out_gain.target = db_to_gain(p.f("gout"));
        self.agc_on = p.on("again");
        self.squeeze.target = p.f("sqz") / 100.0;
        self.knee_db = p.f("knee");
        self.rel_coef = coef(p.f("rel"), self.sr);
        let ramp = ((p.f("att") * 0.001 * self.sr).round() as usize).clamp(1, self.look);
        if ramp != self.ramp {
            self.ramp = ramp;
            self.resum();
        }
        if !self.primed {
            self.in_gain.snap(self.in_gain.target);
            self.out_gain.snap(self.out_gain.target);
            self.squeeze.snap(self.squeeze.target);
            self.primed = true;
        }
    }

    fn process(&mut self, left: &mut [f32], right: &mut [f32]) {
        let look = self.look;
        let mut lowest = 1.0f32;
        for (l, r) in left.iter_mut().zip(right.iter_mut()) {
            let agc = self.auto_gain(*l, *r);
            let gi = self.in_gain.tick() * if agc != 0.0 { db_gain(agc) } else { 1.0 };
            let go = self.out_gain.tick();
            let (mut xl, mut xr) = (*l * gi, *r * gi);
            let s = self.squeeze.tick();
            if s > 1e-6 {
                xl += s * (fast_tanh(xl) - xl);
                xr += s * (fast_tanh(xr) - xr);
            }
            let need = self.required_gain(xl.abs().max(xr.abs()));

            // Sliding minimum over the window [count - look, count].
            let n = self.count;
            while self.tail != self.head && self.min_val[self.tail.wrapping_sub(1) & self.min_mask] >= need {
                self.tail = self.tail.wrapping_sub(1);
            }
            self.min_val[self.tail & self.min_mask] = need;
            self.min_at[self.tail & self.min_mask] = n;
            self.tail = self.tail.wrapping_add(1);
            while n.wrapping_sub(self.min_at[self.head & self.min_mask]) > look {
                self.head = self.head.wrapping_add(1);
            }
            let held = self.min_val[self.head & self.min_mask];
            self.count = n.wrapping_add(1);

            // Instant attack, exponential release (never above the held gain).
            self.released = if held < self.released { held } else { self.released + (held - self.released) * self.rel_coef };

            // Moving average over the attack ramp.
            self.hist_pos = self.hist_pos.wrapping_add(1) & self.hist_mask;
            self.hist[self.hist_pos] = self.released;
            let leaving = self.hist[self.hist_pos.wrapping_sub(self.ramp) & self.hist_mask];
            self.sum += (self.released - leaving) as f64;
            let g = ((self.sum / self.ramp as f64) as f32).min(1.0);
            lowest = lowest.min(g);

            self.delay[0].push(xl);
            self.delay[1].push(xr);
            *l = self.delay[0].tap(look) * g * go;
            *r = self.delay[1].tap(look) * g * go;
        }
        self.reduction_db = gain_to_db(lowest).min(0.0);
    }

    fn reset(&mut self) {
        for d in &mut self.delay {
            d.clear();
        }
        self.head = 0;
        self.tail = 0;
        self.count = 0;
        self.hist.fill(1.0);
        self.hist_pos = 0;
        self.sum = self.ramp as f64;
        self.released = 1.0;
        self.reduction_db = 0.0;
        self.agc_ms = 0.0;
        self.agc_slow = 0.0;
        self.agc_db = 0.0;
        self.in_gain.snap(self.in_gain.target);
        self.out_gain.snap(self.out_gain.target);
        self.squeeze.snap(self.squeeze.target);
    }

    fn meter(&self) -> f32 {
        self.reduction_db
    }

    fn readouts(&self, out: &mut [f32; FX_READOUTS]) {
        out[0] = self.reduction_db;
        out[1] = self.agc_db;
    }

    fn latency(&self) -> u32 {
        self.look as u32
    }
}

#[cfg(test)]
mod tests {
    use super::super::tests::Rig;
    use std::f32::consts::TAU;

    #[test]
    fn holds_its_ceiling() {
        for (knee, sqz, att) in [(0.0, 0.0, 0.05), (2.0, 0.0, 0.3), (10.0, 50.0, 1.0)] {
            let mut rig = Rig::new("LIMITER");
            rig.set("gin", 18.0).set("gout", -3.0).set("knee", knee).set("sqz", sqz).set("att", att);
            let ceiling = 10f32.powf(-3.0 / 20.0);
            let n = 48_000;
            // A sine with sharp bursts on top.
            let mut l: Vec<f32> =
                (0..n).map(|i| (TAU * 220.0 * i as f32 / 48_000.0).sin() * 0.5 + if i % 4000 < 3 { 0.9 } else { 0.0 }).collect();
            let mut r: Vec<f32> = l.iter().map(|x| -x * 0.7).collect();
            rig.run(&mut l, &mut r);
            let peak = l.iter().chain(&r).fold(0.0f32, |a, x| a.max(x.abs()));
            assert!(peak <= ceiling * 1.0001, "knee {knee}: peak {peak} over ceiling {ceiling}");
            assert!(peak > ceiling * 0.8, "knee {knee}: limited far too much ({peak})");
            assert!(rig.dev.meter() < -3.0);
        }
    }

    #[test]
    fn auto_gain_levels_the_program() {
        // A quiet program (-43 dBFS RMS) comes up by the full 12 dB, a hot one (-9) goes down.
        for (amp, expect) in [(0.01f32, 12.0f32), (0.5, -9.0)] {
            let mut rig = Rig::new("LIMITER");
            rig.set("again", 1.0);
            let n = 48_000 * 8;
            let x: Vec<f32> = (0..n).map(|i| (TAU * 440.0 * i as f32 / 48_000.0).sin() * amp).collect();
            let (mut l, mut r) = (x.clone(), x.clone());
            rig.run(&mut l, &mut r);
            let rms = |v: &[f32]| (v.iter().map(|s| s * s).sum::<f32>() / v.len() as f32).sqrt();
            let g = 20.0 * (rms(&l[n - 48_000..]) / rms(&x[n - 48_000..])).log10();
            assert!((g - expect).abs() < 1.0, "{amp}: {g} dB");
        }
    }

    #[test]
    fn release_sets_the_recovery_and_readouts_follow() {
        // A loud burst, then a quiet tone: the gain comes back in about RELEASE.
        let recovery = |rel: f32| {
            let mut rig = Rig::new("LIMITER");
            rig.set("gin", 12.0).set("gout", 0.0).set("rel", rel).set("again", 0.0).set("knee", 0.0);
            let n = 48_000 * 6;
            let mut l: Vec<f32> =
                (0..n).map(|i| (TAU * 200.0 * i as f32 / 48_000.0).sin() * if i < 9600 { 0.9 } else { 0.05 }).collect();
            let mut r = l.clone();
            rig.run(&mut l[..9600], &mut r[..9600]);
            let mut ro = [0.0; crate::wing::fx::FX_READOUTS];
            rig.dev.readouts(&mut ro);
            assert!(ro[0] < -6.0, "readout {}", ro[0]);
            assert_eq!(ro[0], rig.dev.meter());
            // Blocks until the reduction is under 1 dB.
            let mut t = 0;
            for (a, b) in l[9600..].chunks_mut(256).zip(r[9600..].chunks_mut(256)) {
                rig.dev.process(&rig.values, a, b);
                t += 256;
                if rig.dev.meter() > -1.0 {
                    break;
                }
            }
            t as f32 / 48.0
        };
        let (fast, slow) = (recovery(20.0), recovery(2000.0));
        // RELEASE is the recovery's time constant: from 11 dB to 1 dB takes about 1.9 of them.
        assert!((30.0..60.0).contains(&fast) && (3200.0..4400.0).contains(&slow), "{fast} ms vs {slow} ms");
    }

    #[test]
    fn reports_its_look_ahead_as_latency() {
        let rig = Rig::new("LIMITER");
        assert_eq!(rig.dev.latency(), 48);
        let mut rig = Rig::new("LIMITER");
        // The AUTO GAIN readout shows the correction.
        rig.set("again", 1.0);
        let n = 48_000 * 4;
        let mut l: Vec<f32> = (0..n).map(|i| (TAU * 440.0 * i as f32 / 48_000.0).sin() * 0.01).collect();
        let mut r = l.clone();
        rig.run(&mut l, &mut r);
        let mut ro = [0.0; crate::wing::fx::FX_READOUTS];
        rig.dev.readouts(&mut ro);
        assert!(ro[1] > 6.0, "{}", ro[1]);
    }

    #[test]
    fn out_gain_and_squeeze_do_what_they_say() {
        // OUT GAIN sets the output peak level; SQUEEZE rounds peaks (less reduction for the same input).
        for gout in [-12.0f32, -3.0] {
            let mut rig = Rig::new("LIMITER");
            rig.set("gin", 12.0).set("gout", gout).set("knee", 0.0).set("again", 0.0);
            let n = 24_000;
            let mut l: Vec<f32> = (0..n).map(|i| (TAU * 200.0 * i as f32 / 48_000.0).sin() * 0.9).collect();
            let mut r = l.clone();
            rig.run(&mut l, &mut r);
            let peak = l[n / 2..].iter().fold(0.0f32, |a, x| a.max(x.abs()));
            assert!((20.0 * peak.log10() - gout).abs() < 0.3, "{gout}: {peak}");
        }
        let gr = |sqz: f32| {
            let mut rig = Rig::new("LIMITER");
            rig.set("gin", 12.0).set("sqz", sqz).set("knee", 0.0).set("again", 0.0);
            let n = 24_000;
            let mut l: Vec<f32> = (0..n).map(|i| (TAU * 200.0 * i as f32 / 48_000.0).sin() * 0.9).collect();
            let mut r = l.clone();
            rig.run(&mut l, &mut r);
            rig.dev.meter()
        };
        assert!(gr(100.0) > gr(0.0) + 1.0, "{} vs {}", gr(100.0), gr(0.0));
    }

    #[test]
    fn quiet_signals_pass_delayed() {
        let mut rig = Rig::new("LIMITER");
        rig.set("gout", 0.0).set("again", 0.0);
        let n = 4800;
        let x: Vec<f32> = (0..n).map(|i| (TAU * 440.0 * i as f32 / 48_000.0).sin() * 0.1).collect();
        let (mut l, mut r) = (x.clone(), x.clone());
        rig.run(&mut l, &mut r);
        // 1 ms look-ahead at 48 kHz.
        for i in 100..n {
            assert!((l[i] - x[i - 48]).abs() < 1e-6);
        }
        assert_eq!(rig.dev.meter(), 0.0);
    }
}
