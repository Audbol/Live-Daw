//! SPRING (Pioneer SR-202 spring tank): two springs, each a feedback loop of
//! a dispersion chain and a delay (the wave's round trip along the spring).
//! The chain is 48 stretched first-order allpasses, `(-g + z^-K) / (1 - g
//! z^-K)` with `K = fs / 8.4 kHz` (after Parker / Välimäki / Abel): below
//! ~4 kHz the higher frequencies arrive later, so every reflection is the
//! spring's rising chirp, smeared further on each round trip. The output
//! transducer sits half way round the loop; the loop is band-limited like a
//! real tank and its gain gives the DECAY (RT60). DENSITY deepens the
//! dispersion and adds diffusion; BASS and TREBLE are the unit's tone
//! controls (+-12 dB shelves at 250 Hz / 2.5 kHz, flat at 25).
//!
//! The springs run in blocks of `K` samples (a `K`-sample latency, ~0.1 ms):
//! a stretched allpass only looks `K` samples back, so a block runs through
//! each stage at once instead of as one long serial chain per sample.

use super::parts::{pole, rate, run, Diffuser, Output, Param, Pos, Voice, K60, LIM, TINY};
use crate::wing::fx::blocks::{db_to_gain, fast_tanh, DelayLine};
use crate::wing::fx::{FxAlgo, FxContext};
use crate::wing::Params;

/// Allpass stages per spring.
const STAGES: usize = 48;

/// One spring.
struct Spring {
    /// Stage states: `STAGES` rows of the last `k` samples.
    disp: Vec<f32>,
    k: usize,
    line: DelayLine,
    /// Round trip and output tap (samples, before the dispersion).
    round: f32,
    out_pos: Pos,
    /// Sway of the spring (slow delay modulation).
    phase: f32,
    inc: f32,
    sway: f32,
    slope: f32,
    lp: f32,
    hp: f32,
}

impl Spring {
    fn new(sr: f32, k: usize, round_ms: f32, sway_hz: f32) -> Self {
        let round = round_ms * 0.001 * sr;
        let mut out_pos = Pos::default();
        out_pos.set(0.5 * round, false);
        Self {
            disp: vec![0.0; STAGES * k],
            k,
            line: DelayLine::new(round as usize + 64),
            round,
            out_pos,
            phase: 0.0,
            inc: sway_hz * super::parts::CTL as f32 / sr,
            sway: 0.0,
            slope: 0.0,
            lp: 0.0,
            hp: 0.0,
        }
    }

    fn clear(&mut self) {
        self.disp.fill(0.0);
        self.line.clear();
        self.lp = 0.0;
        self.hp = 0.0;
    }

    /// Mean loop delay in samples (an allpass delays by `k` on average).
    fn loop_len(&self) -> f32 {
        self.round + (STAGES * self.k) as f32
    }

    fn control(&mut self, depth: f32) {
        self.phase = (self.phase + self.inc).fract();
        let target = depth * (std::f32::consts::TAU * self.phase).sin();
        self.slope = (target - self.sway) / super::parts::CTL as f32;
    }

    /// Runs a block of `k` samples `x` into `y`; `v` is scratch.
    #[allow(clippy::too_many_arguments)]
    fn block(&mut self, x: &[f32], y: &mut [f32], v: &mut [f32], g: f32, fb: f32, a_lp: f32, a_hp: f32) {
        // The round trip is longer than a block: every feedback sample the
        // block needs is already in the line (`j` samples less far back).
        for (j, (v, &x)) in v.iter_mut().zip(x).enumerate() {
            self.sway += self.slope;
            *v = x + fb * self.line.tap_hermite(self.round + self.sway - j as f32);
        }
        for row in self.disp.chunks_exact_mut(self.k) {
            for (v, s) in v.iter_mut().zip(row.iter_mut()) {
                let d = *s;
                let w = *v + g * d;
                *s = w;
                *v = d - g * w;
            }
        }
        for (&v, y) in v.iter().zip(y.iter_mut()) {
            // The tank's band: ~4.5 kHz top, ~70 Hz bottom, per round trip.
            self.lp += a_lp * (v - self.lp);
            self.hp += a_hp * (self.lp - self.hp);
            self.line.push((self.lp - self.hp + TINY).clamp(-LIM, LIM));
            *y = self.out_pos.read(&self.line);
        }
    }
}

pub(super) struct SpringReverb {
    sr: f32,
    springs: [Spring; 2],
    /// Block buffers (inputs and outputs per spring, scratch) and the fill.
    ins: [Vec<f32>; 2],
    outs: [Vec<f32>; 2],
    scratch: Vec<f32>,
    fill: usize,
    diff: [Diffuser; 2],
    t60: Param,
    dens: Param,
    bass: Param,
    treble: Param,
    g: f32,
    fb: [f32; 2],
    gains: (f32, f32),
    tone: [[f32; 2]; 2],
    a_lp: f32,
    a_hp: f32,
    a_lo: f32,
    a_hi: f32,
    out: Output,
    a: f32,
    force: bool,
    clock: usize,
    primed: bool,
}

impl SpringReverb {
    pub fn new(ctx: FxContext) -> Self {
        let sr = ctx.sample_rate;
        let k = ((sr / 8400.0).round() as usize).max(2);
        let mut out = Output::new(sr);
        out.lc.set(60.0);
        out.hc.set(7000.0);
        out.gain.set(1.25);
        out.snap();
        Self {
            sr,
            springs: [Spring::new(sr, k, 31.0, 2.3), Spring::new(sr, k, 37.5, 3.1)],
            ins: [vec![0.0; k], vec![0.0; k]],
            outs: [vec![0.0; k], vec![0.0; k]],
            scratch: vec![0.0; k],
            fill: 0,
            diff: [Diffuser::new(sr, [2.3, 3.7, 5.9, 7.1], 1.0), Diffuser::new(sr, [2.6, 3.3, 6.1, 6.7], 1.0)],
            t60: Param::default(),
            dens: Param::default(),
            bass: Param::default(),
            treble: Param::default(),
            g: 0.0,
            fb: [0.0; 2],
            gains: (1.0, 1.0),
            tone: [[0.0; 2]; 2],
            a_lp: pole(4500.0, sr),
            a_hp: pole(70.0, sr),
            a_lo: pole(250.0, sr),
            a_hi: pole(2500.0, sr),
            out,
            a: rate(40.0, sr),
            force: true,
            clock: 0,
            primed: false,
        }
    }
}

impl FxAlgo for SpringReverb {
    fn set_params(&mut self, p: &Params) {
        self.t60.set(p.f("dcy"));
        self.dens.set((p.f("dens") - 1.0) / 29.0);
        self.bass.set(12.0 * (p.f("low") - 25.5) / 24.5);
        self.treble.set(12.0 * (p.f("high") - 25.5) / 24.5);
        if !self.primed {
            self.primed = true;
            for q in [&mut self.t60, &mut self.dens, &mut self.bass, &mut self.treble] {
                q.snap();
            }
            self.force = true;
        }
    }

    fn process(&mut self, left: &mut [f32], right: &mut [f32]) {
        run(self, left, right);
    }

    fn reset(&mut self) {
        for s in &mut self.springs {
            s.clear();
        }
        for b in self.ins.iter_mut().chain(&mut self.outs) {
            b.fill(0.0);
        }
        for d in &mut self.diff {
            d.clear();
        }
        self.tone = [[0.0; 2]; 2];
        self.out.clear();
    }
}

impl Voice for SpringReverb {
    fn clock(&mut self) -> &mut usize {
        &mut self.clock
    }

    fn control(&mut self) {
        let force = std::mem::take(&mut self.force);
        let a = self.a;
        if self.t60.step(a) || force {
            // The loop's band-limiting costs a little level per round trip even
            // at 1 kHz; make that up so DECAY holds in the middle of the band.
            let w = std::f32::consts::TAU * 1000.0 / self.sr;
            let lp = |a: f32| {
                let b = 1.0 - a;
                let (re, im) = (1.0 - b * w.cos(), b * w.sin());
                (a * re / (re * re + im * im), -a * im / (re * re + im * im))
            };
            let (lr, li) = lp(self.a_lp);
            let (hr, hi) = lp(self.a_hp);
            // The loop's filter is lp * (1 - hp-pole low-pass).
            let (br, bi) = (1.0 - hr, -hi);
            let loss = ((lr * br - li * bi).powi(2) + (lr * bi + li * br).powi(2)).sqrt();
            for (fb, s) in self.fb.iter_mut().zip(&self.springs) {
                *fb = ((K60 * s.loop_len() / self.sr / self.t60.cur).exp2() / loss.max(0.5)).min(0.995);
            }
        }
        if self.dens.step(a) || force {
            self.g = -(0.45 + 0.3 * self.dens.cur);
            for d in &mut self.diff {
                d.set(1.0, 0.15 + 0.6 * self.dens.cur, false);
            }
        }
        if self.bass.step(a) | self.treble.step(a) || force {
            self.gains = (db_to_gain(self.bass.cur), db_to_gain(self.treble.cur));
        }
        let depth = 0.0004 * self.sr;
        for s in &mut self.springs {
            s.control(depth);
        }
        self.out.control(a, force);
    }

    #[inline]
    fn tick(&mut self, xl: f32, xr: f32) -> (f32, f32) {
        // The drive coil saturates gently.
        let (il, ir) = (fast_tanh(0.7 * xl + 0.3 * xr), fast_tanh(0.3 * xl + 0.7 * xr));
        let j = self.fill;
        self.ins[0][j] = il;
        self.ins[1][j] = ir;
        let (s0, s1) = (self.outs[0][j], self.outs[1][j]);
        self.fill += 1;
        if self.fill == self.scratch.len() {
            self.fill = 0;
            let (g, a_lp, a_hp) = (self.g, self.a_lp, self.a_hp);
            for (((s, x), y), &fb) in self.springs.iter_mut().zip(&self.ins).zip(&mut self.outs).zip(&self.fb) {
                s.block(x, y, &mut self.scratch, g, fb, a_lp, a_hp);
            }
        }
        let mut out = [self.diff[0].tick(s0 + 0.25 * s1), self.diff[1].tick(s1 + 0.25 * s0)];
        let (gb, gt) = self.gains;
        for (o, z) in out.iter_mut().zip(&mut self.tone) {
            z[0] += self.a_lo * (*o - z[0]);
            z[1] += self.a_hi * (*o - z[1]);
            *o = gb * z[0] + (z[1] - z[0]) + gt * (*o - z[1]);
        }
        self.out.tick(out[0], out[1])
    }
}
