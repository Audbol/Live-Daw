//! Gated and reverse reverbs, built the way the classic non-linear programs
//! were: a short, dense allpass diffuser feeding a delay line read by up to
//! 32 taps per side scattered over the DECAY window (after the pre-delay),
//! then a very short diffuser that fuses the taps. The taps' gains draw the
//! envelope, so the sound stops (within a few ms) at the end of the window
//! whatever the input.
//! - GATED (the 1980s reverb-into-noise-gate sound): the envelope rises
//!   (ATTACK: high = immediate, low = over up to 35 % of the window), then
//!   falls away within the window unless SPREAD lets it unfold to the end,
//!   where it cuts. DENSITY sets how many taps are active (few = sparse and
//!   grainy, many = a tight, continuous tail), DIFFUSE the allpasses.
//! - REVERSE: the envelope swells over the RISE length (20..100 % of the
//!   window) and cuts at its end; SPREAD sets how early the swell unfolds
//!   (low = a late, steep swell, high = a long, even one).
//!
//! Both end in a first-order high shelf (FREQ, GAIN) and LO CUT; the taps
//! are scattered differently per side, so the output is wide.

use super::parts::{hash01, pole, rate, run, sign, Diffuser, Output, Param, Pos, Voice, CTL};
use crate::wing::fx::blocks::DelayLine;
use crate::wing::fx::{FxAlgo, FxContext};
use crate::wing::{fx::blocks::db_to_gain, Params};

/// Taps per side.
const TAPS: usize = 32;

pub(super) struct NonLin {
    reverse: bool,
    sr: f32,
    diff: [Diffuser; 2],
    post: [Diffuser; 2],
    lines: Vec<DelayLine>,
    /// Each tap's place in the window (0..1); taps are in order of activation.
    u: [[f32; TAPS]; 2],
    sgn: [[f32; TAPS]; 2],
    pos: [[Pos; TAPS]; 2],
    gain: [[f32; TAPS]; 2],
    active: usize,
    pre: Param,
    win: Param,
    dens: Param,
    shape: Param,
    unfold: Param,
    diffusion: Param,
    hs_hz: Param,
    hs_gain: Param,
    hs_a: f32,
    z: [f32; 2],
    out: Output,
    a: f32,
    a_slow: f32,
    force: bool,
    clock: usize,
    primed: bool,
}

impl NonLin {
    pub fn new(reverse: bool, ctx: FxContext) -> Self {
        let sr = ctx.sample_rate;
        let cap = (1.25 * sr) as usize;
        let mut u = [[0.0; TAPS]; 2];
        let mut sgn = [[1.0; TAPS]; 2];
        for c in 0..2 {
            let seed = 0x6A7E + c as u32 * 0x101;
            for k in 0..TAPS {
                // A golden-ratio sequence (jittered): the taps active at any
                // density cover the whole window evenly.
                let j = (hash01(k as u32, seed) - 0.5) / TAPS as f32;
                u[c][k] = (0.31 * c as f32 + k as f32 * 0.618_034 + j).rem_euclid(1.0);
                sgn[c][k] = sign(k as u32, seed ^ 0x3F);
            }
        }
        Self {
            reverse,
            sr,
            diff: [Diffuser::new(sr, [1.3, 2.1, 3.4, 5.1], 1.0), Diffuser::new(sr, [1.5, 2.3, 3.1, 4.7], 1.0)],
            post: [Diffuser::new(sr, [0.7, 1.1, 1.9, 2.9], 1.0), Diffuser::new(sr, [0.8, 1.3, 1.7, 2.7], 1.0)],
            lines: vec![DelayLine::new(cap), DelayLine::new(cap)],
            u,
            sgn,
            pos: [[Pos::default(); TAPS]; 2],
            gain: [[0.0; TAPS]; 2],
            active: TAPS,
            pre: Param::default(),
            win: Param::default(),
            dens: Param::default(),
            shape: Param::default(),
            unfold: Param::default(),
            diffusion: Param::default(),
            hs_hz: Param::default(),
            hs_gain: Param::default(),
            hs_a: 1.0,
            z: [0.0; 2],
            out: Output::new(sr),
            a: rate(30.0, sr),
            a_slow: rate(150.0, sr),
            force: true,
            clock: 0,
            primed: false,
        }
    }

    /// Envelope at `u` (0..1) of the window for `shape` (the attack or rise
    /// fraction) and `unfold` (SPREAD, 0..1).
    fn envelope(&self, u: f32, shape: f32, unfold: f32) -> f32 {
        if self.reverse {
            let x = (u / shape.max(1e-3)).min(1.0);
            let c = 0.5 + 7.0 * (1.0 - unfold);
            ((c * x).exp() - 1.0) / (c.exp() - 1.0)
        } else if u < shape {
            (u / shape).powf(0.7)
        } else {
            let t = (u - shape) / (1.0 - shape).max(1e-3);
            (-3.5 * (1.0 - unfold) * t).exp()
        }
    }

    fn update_taps(&mut self, moving: bool) {
        let frac = self.pre.moving() || self.win.moving();
        let max = self.lines[0].max_delay() as f32 - 3.0;
        // DENSITY: the active fraction of the taps; the last one fades in.
        let n = (self.dens.cur * TAPS as f32).clamp(1.0, TAPS as f32);
        self.active = n.ceil() as usize;
        let (shape, unfold) = (self.shape.cur, self.unfold.cur);
        for c in 0..2 {
            if moving {
                for (p, &u) in self.pos[c].iter_mut().zip(&self.u[c]) {
                    p.set((self.pre.cur + self.win.cur * u).clamp(1.0, max), frac);
                }
            }
            let mut sum = 0.0;
            for k in 0..self.active {
                let u = self.u[c][k];
                let w = (n - k as f32).min(1.0);
                let g = self.envelope(u, shape, unfold) * w;
                self.gain[c][k] = g * self.sgn[c][k];
                sum += g * g;
            }
            let norm = 0.8 / sum.sqrt().max(1e-3);
            for g in &mut self.gain[c][..self.active] {
                *g *= norm;
            }
        }
    }
}

impl FxAlgo for NonLin {
    fn set_params(&mut self, p: &Params) {
        let ms = 0.001 * self.sr;
        self.pre.set(p.f("pdel") * ms);
        self.win.set(p.f("dcy") * 1000.0 * ms);
        self.diffusion.set(p.f("diff") / 30.0);
        if self.reverse {
            self.dens.set(1.0);
            self.shape.set(0.2 + 0.8 * p.f("rise") / 50.0);
        } else {
            self.dens.set(0.06 + 0.94 * (p.f("dens") - 1.0) / 49.0);
            self.shape.set(0.35 * (1.0 - p.f("att") / 30.0));
        }
        self.unfold.set(p.f("sprd") / 100.0);
        self.hs_hz.set(p.f("hsf"));
        self.hs_gain.set(db_to_gain(p.f("hsg")));
        self.out.lc.set(p.f("lc"));
        if !self.primed {
            self.primed = true;
            for q in [
                &mut self.pre,
                &mut self.win,
                &mut self.diffusion,
                &mut self.dens,
                &mut self.shape,
                &mut self.unfold,
                &mut self.hs_hz,
                &mut self.hs_gain,
            ] {
                q.snap();
            }
            self.out.snap();
            self.force = true;
        }
    }

    fn process(&mut self, left: &mut [f32], right: &mut [f32]) {
        run(self, left, right);
    }

    fn reset(&mut self) {
        for d in self.diff.iter_mut().chain(&mut self.post) {
            d.clear();
        }
        for l in &mut self.lines {
            l.clear();
        }
        self.z = [0.0; 2];
        self.out.clear();
    }
}

impl Voice for NonLin {
    fn clock(&mut self) -> &mut usize {
        &mut self.clock
    }

    fn control(&mut self) {
        let force = std::mem::take(&mut self.force);
        let a = self.a;
        // Tap times glide at most ~0.8 sample per sample (a bend, not a smear).
        let lim = 0.4 * CTL as f32;
        let moved = self.pre.step_limited(self.a_slow, lim) | self.win.step_limited(self.a_slow, lim);
        let shaped = self.dens.step(a) | self.shape.step(a) | self.unfold.step(a);
        if moved || shaped || force {
            self.update_taps(moved || force);
        }
        if self.diffusion.step(a) || force {
            for d in &mut self.diff {
                d.set(1.0, self.diffusion.cur, false);
            }
            for d in &mut self.post {
                d.set(1.0, 0.3 + 0.5 * self.diffusion.cur, false);
            }
        }
        if self.hs_hz.step(a) || force {
            self.hs_a = pole(self.hs_hz.cur, self.sr);
        }
        self.hs_gain.step(a);
        self.out.control(a, force);
    }

    #[inline]
    fn tick(&mut self, xl: f32, xr: f32) -> (f32, f32) {
        let mut out = [0.0f32; 2];
        let n = self.active;
        for (c, x) in [xl, xr].into_iter().enumerate() {
            let line = &mut self.lines[c];
            line.push(self.diff[c].tick(x));
            let mut s = 0.0;
            for (p, &g) in self.pos[c][..n].iter_mut().zip(&self.gain[c][..n]) {
                s += g * p.read(line);
            }
            let s = self.post[c].tick(s);
            // HI FREQ / HI GAIN: first-order high shelf.
            self.z[c] += self.hs_a * (s - self.z[c]);
            out[c] = self.z[c] + self.hs_gain.cur * (s - self.z[c]);
        }
        self.out.tick(out[0], out[1])
    }
}
