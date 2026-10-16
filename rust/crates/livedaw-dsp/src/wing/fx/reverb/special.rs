//! Reverbs with an extra stage around the tank:
//! - DEL/REV (Lexicon PCM70 delay + reverb): a stereo delay (TIME, FEED with
//!   FEED HC damping the repeats) in parallel with a hall-style tank. DLY LVL
//!   is the delay's own level in the output, DLY->REV how much of the delay
//!   feeds the reverb, IN->REV how much dry input does; the reverb has
//!   PRE DLY, SIZE, DECAY, DAMPING (a gentle 6 dB/oct high cut on its input,
//!   the highs also dying a little faster) and REV LCUT. A new TIME is
//!   reached with a short crossfade, as on a digital delay.
//! - SHIMMER (a shimmer pedal into a reverb pedal into a filter pedal): the
//!   tank's output is pitch-shifted an octave up and fed back into its input,
//!   so each pass adds the next octave (harmonic). SHIMMER is that loop's
//!   gain, i.e. how many harmonics build up (normalised to the decay so the
//!   regeneration stays even); SHINE sets how gently each successive
//!   harmonic is attenuated (the shifted signal's brightness). DAMPING (%) is
//!   a gentle high cut ahead of the tank, LO / HI CUT the filter pedal.

use super::parts::{pole, rate, run, spread, Decay, Param, Voice, XHead, K60, LIM, TINY};
use super::space::{Config, Settings, Space};
use crate::wing::fx::blocks::{fast_tanh, DelayLine, PitchShifter};
use crate::wing::fx::{FxAlgo, FxContext};
use crate::wing::Params;

pub(super) struct DelayReverb {
    sr: f32,
    space: Space<16>,
    lines: Vec<DelayLine>,
    heads: [XHead; 2],
    feed: Param,
    fhc: Param,
    a_fhc: f32,
    dly: Param,
    d2r: Param,
    i2r: Param,
    z: [f32; 2],
    a: f32,
    clock: usize,
    primed: bool,
}

impl DelayReverb {
    pub fn new(ctx: FxContext) -> Self {
        let sr = ctx.sample_rate;
        let cfg = Config {
            rel: spread(0.55, 1.45, 51),
            max_size_ms: 130.0,
            max_pre_ms: 200.0,
            loop_ap: Some([0.15, 0.3]),
            diff_ms: [4.8, 3.6, 12.7, 9.3],
            diff_ref_ms: 80.0,
            seed: 0x70DE,
        };
        let mut space = Space::new(sr, &cfg);
        space.cal = 1.5;
        let cap = (3.01 * sr) as usize + 8;
        Self {
            sr,
            space,
            lines: vec![DelayLine::new(cap), DelayLine::new(cap)],
            heads: [XHead::new(sr, 40.0); 2],
            feed: Param::default(),
            fhc: Param::default(),
            a_fhc: 1.0,
            dly: Param::default(),
            d2r: Param::default(),
            i2r: Param::default(),
            z: [0.0; 2],
            a: rate(30.0, sr),
            clock: 0,
            primed: false,
        }
    }
}

impl FxAlgo for DelayReverb {
    fn set_params(&mut self, p: &Params) {
        let size_ms = 10.0 + 1.2 * p.f("size");
        let s = Settings {
            pre_ms: p.f("pdel"),
            size_ms,
            decay: Decay::lexicon(p.f("dcy"), 1.0, 500.0, p.f("damp")),
            damp_hz: p.f("damp"),
            diffusion: 0.75,
            shape: 0.4,
            build_ms: 0.6 * size_ms,
            mod_hz: 0.45,
            mod_ms: 0.3,
            lc: p.f("rlc"),
            hc: 16000.0,
            width: 1.0,
            gain: 1.0,
            input: 1.0,
            ..Settings::default()
        };
        self.space.set(&s);
        let max = self.lines[0].max_delay() as f32 - 3.0;
        for (k, h) in self.heads.iter_mut().enumerate() {
            // The right repeat sits a sample later: a hint of width.
            h.set((p.f("time") * 0.001 * self.sr + k as f32).min(max));
        }
        self.feed.set(0.95 * p.f("feed") / 100.0);
        self.fhc.set(p.f("fhc"));
        self.dly.set(p.f("dly") / 100.0);
        self.d2r.set(p.f("d2r") / 100.0);
        self.i2r.set(p.f("i2r") / 100.0);
        if !self.primed {
            self.primed = true;
            self.space.snap();
            for q in [&mut self.feed, &mut self.fhc, &mut self.dly, &mut self.d2r, &mut self.i2r] {
                q.snap();
            }
            self.a_fhc = pole(self.fhc.cur, self.sr);
            for h in &mut self.heads {
                h.snap();
            }
        }
    }

    fn process(&mut self, left: &mut [f32], right: &mut [f32]) {
        run(self, left, right);
    }

    fn reset(&mut self) {
        self.space.clear();
        for l in &mut self.lines {
            l.clear();
        }
        self.z = [0.0; 2];
    }
}

impl Voice for DelayReverb {
    fn clock(&mut self) -> &mut usize {
        &mut self.clock
    }

    fn control(&mut self) {
        self.space.control();
        for h in &mut self.heads {
            h.control();
        }
        if self.fhc.step(self.a) {
            self.a_fhc = pole(self.fhc.cur, self.sr);
        }
        for q in [&mut self.feed, &mut self.dly, &mut self.d2r, &mut self.i2r] {
            q.step(self.a);
        }
    }

    #[inline]
    fn tick(&mut self, xl: f32, xr: f32) -> (f32, f32) {
        let mut d = [0.0f32; 2];
        for (c, x) in [xl, xr].into_iter().enumerate() {
            let line = &mut self.lines[c];
            d[c] = self.heads[c].read(line);
            self.z[c] += self.a_fhc * (d[c] - self.z[c]);
            line.push((x + self.feed.cur * self.z[c] + TINY).clamp(-LIM, LIM));
        }
        let (i2r, d2r, dly) = (self.i2r.cur, self.d2r.cur, self.dly.cur);
        let (il, ir) = self.space.damp(i2r * xl + d2r * d[0], i2r * xr + d2r * d[1]);
        let (rl, rr) = self.space.tank(il, ir);
        self.space.out.tick(rl + dly * d[0], rr + dly * d[1])
    }
}

pub(super) struct Shimmer {
    sr: f32,
    space: Space<16>,
    shifter: PitchShifter,
    /// Last raw tank output (the feedback source).
    last: (f32, f32),
    gain: Param,
    shine: Param,
    a_shine: f32,
    z: [f32; 2],
    a_hp: f32,
    a: f32,
    clock: usize,
    primed: bool,
}

impl Shimmer {
    pub fn new(ctx: FxContext) -> Self {
        let sr = ctx.sample_rate;
        let cfg = Config {
            rel: spread(0.55, 1.45, 61),
            max_size_ms: 140.0,
            max_pre_ms: 250.0,
            loop_ap: Some([0.15, 0.3]),
            diff_ms: [4.8, 3.6, 12.7, 9.3],
            diff_ref_ms: 80.0,
            seed: 0x5A1E,
        };
        let mut space = Space::new(sr, &cfg);
        space.cal = 2.05;
        let mut shifter = PitchShifter::new(60.0, sr);
        shifter.set_ratio(2.0);
        Self {
            sr,
            space,
            shifter,
            last: (0.0, 0.0),
            gain: Param::default(),
            shine: Param::default(),
            a_shine: 1.0,
            z: [0.0; 2],
            a_hp: pole(180.0, sr),
            a: rate(40.0, sr),
            clock: 0,
            primed: false,
        }
    }
}

impl FxAlgo for Shimmer {
    fn set_params(&mut self, p: &Params) {
        let size_ms = 15.0 + 2.4 * p.f("size");
        let dcy = p.f("dcy");
        let damp = 18000.0 * (1200.0f32 / 18000.0).powf(p.f("damp") / 100.0);
        let s = Settings {
            pre_ms: p.f("pdel"),
            size_ms,
            decay: Decay::lexicon(dcy, 1.0, 500.0, damp),
            damp_hz: damp,
            diffusion: 0.8,
            shape: 0.65,
            build_ms: 0.8 * size_ms,
            mod_hz: 0.4,
            mod_ms: 0.6,
            lc: p.f("lc"),
            hc: p.f("hc"),
            width: 1.2,
            gain: 1.0,
            input: 1.0,
            ..Settings::default()
        };
        self.space.set(&s);
        // The tank's own recirculation gain at its mean line length: dividing
        // by its RMS gain keeps the octave loop's gain at SHIMMER whatever the
        // decay.
        let g = (K60 * size_ms * 0.001 / dcy).exp2();
        let norm = (1.0 - g * g).sqrt() / self.space.cal;
        self.gain.set(0.9 * norm * p.f("shim") / 100.0);
        self.shine.set(1500.0 * 8.0f32.powf(p.f("shine") / 100.0));
        if !self.primed {
            self.primed = true;
            self.space.snap();
            self.gain.snap();
            self.shine.snap();
            self.a_shine = pole(self.shine.cur, self.sr);
        }
    }

    fn process(&mut self, left: &mut [f32], right: &mut [f32]) {
        run(self, left, right);
    }

    fn reset(&mut self) {
        self.space.clear();
        self.shifter.clear();
        self.last = (0.0, 0.0);
        self.z = [0.0; 2];
    }
}

impl Voice for Shimmer {
    fn clock(&mut self) -> &mut usize {
        &mut self.clock
    }

    fn control(&mut self) {
        self.space.control();
        self.gain.step(self.a);
        if self.shine.step(self.a) {
            self.a_shine = pole(self.shine.cur, self.sr);
        }
    }

    #[inline]
    fn tick(&mut self, xl: f32, xr: f32) -> (f32, f32) {
        let up = self.shifter.tick(0.5 * (self.last.0 + self.last.1));
        // Keep the shifted signal out of the mud and as bright as SHINE asks.
        self.z[0] += self.a_hp * (up - self.z[0]);
        let hp = up - self.z[0];
        self.z[1] += self.a_shine * (hp - self.z[1]);
        let s = fast_tanh(self.gain.cur * self.z[1]);
        let (il, ir) = self.space.damp(xl + s, xr + s);
        let (tl, tr) = self.space.tank(il, ir);
        self.last = (tl, tr);
        self.space.out.tick(tl, tr)
    }
}
