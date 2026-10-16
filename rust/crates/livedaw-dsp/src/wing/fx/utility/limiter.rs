//! Precision Limiter: a mastering brickwall limiter with look-ahead.
//!
//! IN GAIN drives the limiter; OUT GAIN is the ceiling no sample passes.
//! SQUEEZE blends in a soft saturator (referenced to the ceiling) ahead of the
//! limiter, rounding peaks so the limiter works less and the result gets
//! louder. KNEE (0..10 dB) softens the onset below the ceiling. ATTACK
//! (0.05..1 ms) is the length of the gain ramp ahead of each peak; RELEASE is
//! the recovery time. AUTO GAIN level-matches the output (takes IN GAIN back
//! off) so the limiting can be judged without the loudness gain.
//!
//! How the ceiling is held: the gain each sample needs is held over the whole
//! look-ahead window (a sliding minimum), released exponentially, and then
//! averaged over the attack length. Every averaged value is at most the gain
//! the delayed sample needs, so the output never exceeds the ceiling, and the
//! averaging turns each reduction into a smooth ramp that ends on the peak.
//! The audio is delayed by the longest attack (1 ms), a fixed latency.

use crate::wing::fx::blocks::{coef, db_to_gain, fast_tanh, gain_to_db, DelayLine, Smoothed};
use crate::wing::fx::FxAlgo;
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
    ceiling: Smoothed,
    makeup: Smoothed,
    squeeze: Smoothed,
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
            ceiling: Smoothed::new(1.0, 20.0, sr),
            makeup: Smoothed::new(1.0, 20.0, sr),
            squeeze: Smoothed::new(0.0, 20.0, sr),
            knee_db: 2.0,
            reduction_db: 0.0,
            primed: false,
        };
        s.reset();
        s
    }

    /// Gain (<= 1) that brings a peak of `peak` under the ceiling `c` with the soft knee.
    #[inline]
    fn required_gain(&self, peak: f32, c: f32) -> f32 {
        let k = self.knee_db;
        if k <= 0.0 {
            return if peak > c { c / peak } else { 1.0 };
        }
        // The knee spans c - k/2 .. c + k/2 (dB); the output never passes c.
        if peak <= c * db_to_gain(-0.5 * k) {
            return 1.0;
        }
        let x = gain_to_db(peak);
        let cd = gain_to_db(c);
        let over = x - cd + 0.5 * k;
        let y = if over >= k { cd } else { x - over * over / (2.0 * k) };
        db_to_gain(y - x).min(c / peak.max(1e-9) * 1.000_001).min(1.0)
    }

    /// Sum of the last `ramp` released gains.
    fn resum(&mut self) {
        self.sum = (0..self.ramp).map(|i| self.hist[self.hist_pos.wrapping_sub(i) & self.hist_mask] as f64).sum();
    }
}

impl FxAlgo for PrecisionLimiter {
    fn set_params(&mut self, p: &Params) {
        let gin = p.f("gin");
        self.in_gain.target = db_to_gain(gin);
        self.ceiling.target = db_to_gain(p.f("gout"));
        self.makeup.target = if p.on("again") { db_to_gain(-gin) } else { 1.0 };
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
            self.ceiling.snap(self.ceiling.target);
            self.makeup.snap(self.makeup.target);
            self.squeeze.snap(self.squeeze.target);
            self.primed = true;
        }
    }

    fn process(&mut self, left: &mut [f32], right: &mut [f32]) {
        let look = self.look;
        let mut lowest = 1.0f32;
        for (l, r) in left.iter_mut().zip(right.iter_mut()) {
            let gi = self.in_gain.tick();
            let c = self.ceiling.tick();
            let mk = self.makeup.tick();
            let (mut xl, mut xr) = (*l * gi, *r * gi);
            let s = self.squeeze.tick();
            if s > 1e-6 {
                xl += s * (c * fast_tanh(xl / c) - xl);
                xr += s * (c * fast_tanh(xr / c) - xr);
            }
            let need = self.required_gain(xl.abs().max(xr.abs()), c);

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
            *l = self.delay[0].tap(look) * g * mk;
            *r = self.delay[1].tap(look) * g * mk;
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
        self.in_gain.snap(self.in_gain.target);
        self.ceiling.snap(self.ceiling.target);
        self.makeup.snap(self.makeup.target);
        self.squeeze.snap(self.squeeze.target);
    }

    fn meter(&self) -> f32 {
        self.reduction_db
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
    fn quiet_signals_pass_delayed() {
        let mut rig = Rig::new("LIMITER");
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
