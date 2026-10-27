//! The vintage reverbs:
//! - V-ROOM (Quantec QRS/L room simulator, "warm and dense"): a 16-line room
//!   with an early-reflection pattern per side (ER DELAY L / R start times,
//!   spread by ROOM SIZE, ER LEVEL). DENSITY sets the in-loop and input
//!   diffusion (sparse vs a tight, continuous tail), LOW / HIGH MULTIPLY the
//!   decay below 300 Hz / above 4 kHz, LEVEL the output. FREEZE cuts the input
//!   and makes the tank lossless, holding the tail indefinitely; with ADD the
//!   input keeps adding to a very long (60 s) frozen tail.
//! - V-REV (EMT 250, "clear, open and lush"): a 16-line tank whose size
//!   grows a little with DECAY, highs always shorter (HI MULTI 0.25..0.67
//!   above 3 kHz), MODULATE setting the modulation rate and depth. OUTPUT REAR
//!   takes the rear output pair: later (+17 ms), darker and wider; XFORMER adds
//!   the output transformer's soft saturation, low-end weight and top roll-off.
//! - V-PLATE (EMT 140): a plate tank (allpasses in every line), fixed plate
//!   dimensions, highs and lows decaying faster than the mids; FILTER is the
//!   low cut, COLOUR a dark-bright tilt around 1 kHz (+-6 dB at the extremes).

use super::lexicon::mod_rate;
use super::parts::{fader, hash01, pole, rate, run, sign, spread, Decay, Param, Pos, Taps, Voice};
use super::space::{Config, Settings, Space};
use crate::wing::fx::blocks::{db_to_gain, DelayLine};
use crate::wing::fx::meters::VuMeter;
use crate::wing::fx::{FxAlgo, FxContext, FX_READOUTS};
use crate::wing::{colour, Params};

/// Early reflections per side in V-ROOM.
const VROOM_ER: usize = 12;

pub(super) struct VintageRoom {
    sr: f32,
    space: Space<16>,
    er: Taps<VROOM_ER>,
    /// ER input gain (cut with the tank input when frozen).
    er_in: Param,
    a: f32,
    clock: usize,
    primed: bool,
}

impl VintageRoom {
    pub fn new(ctx: FxContext) -> Self {
        let sr = ctx.sample_rate;
        let cfg = Config {
            rel: spread(0.62, 1.38, 21),
            max_size_ms: 120.0,
            max_pre_ms: 200.0,
            loop_ap: Some([0.1, 0.5]),
            diff_ms: [4.1, 3.3, 9.7, 7.6],
            diff_ref_ms: 50.0,
            seed: 0x0A27,
        };
        let mut space = Space::new(sr, &cfg);
        space.cal = 1.34;
        Self {
            sr,
            space,
            er: Taps::new(sr, 200.0 + 90.0, VROOM_ER),
            er_in: Param::default(),
            a: rate(40.0, sr),
            clock: 0,
            primed: false,
        }
    }
}

impl FxAlgo for VintageRoom {
    fn set_params(&mut self, p: &Params) {
        let size = p.f("size");
        let size_ms = 6.0 + 2.2 * size;
        let frz = p.on("frz");
        let add = p.on("add");
        let mut decay = Decay::three_band(p.f("dcy"), p.f("lmult"), p.f("hmult"), 300.0, 4000.0);
        let mut input = 1.0;
        if frz {
            if add {
                decay = Decay { bands: [1.0 / 60.0; 4], ..decay };
                input = 0.5;
            } else {
                decay = decay.frozen();
                input = 0.0;
            }
        }
        let s = Settings {
            pre_ms: p.f("pdel"),
            size_ms,
            decay,
            diffusion: 0.25 + 0.7 * (p.f("dens") - 1.0) / 29.0,
            shape: 0.35,
            build_ms: 0.5 * size_ms,
            mod_hz: 0.3,
            mod_ms: 0.08,
            lc: p.f("lc"),
            hc: p.f("hc"),
            width: 1.0,
            gain: db_to_gain(p.f("lvl")),
            input,
            ..Settings::default()
        };
        self.space.set(&s);
        self.er_in.set(input);
        // The reflection pattern: denser towards its start, falling in level.
        let ms = 0.001 * self.sr;
        let span = (4.0 + 1.6 * size) * ms;
        let lvl = 1.1 * p.f("erlvl") / 100.0;
        for (ch, start) in [p.f("erl"), p.f("err")].into_iter().enumerate() {
            let seed = 0x0E70 + ch as u32;
            let mut norm = 0.0;
            let mut taps = [(0.0f32, 0.0f32); VROOM_ER];
            for (k, t) in taps.iter_mut().enumerate() {
                let u = ((k as f32 + 0.8 * hash01(k as u32, seed)) / VROOM_ER as f32).powf(1.3);
                let g = (1.0 - 0.75 * u) * sign(k as u32, seed ^ 0x55);
                norm += g * g;
                *t = (start * ms + span * u, g);
            }
            let norm = lvl / norm.sqrt();
            for (k, (pos, g)) in taps.into_iter().enumerate() {
                self.er.set(ch, k, pos, g * norm);
            }
        }
        if !self.primed {
            self.primed = true;
            self.space.snap();
            self.er.snap();
            self.er_in.snap();
        }
    }

    fn process(&mut self, left: &mut [f32], right: &mut [f32]) {
        run(self, left, right);
    }

    fn reset(&mut self) {
        self.space.clear();
        self.er.clear();
    }
}

impl Voice for VintageRoom {
    fn clock(&mut self) -> &mut usize {
        &mut self.clock
    }

    fn control(&mut self) {
        self.space.control();
        self.er.control(self.a, false);
        self.er_in.step(self.a);
    }

    #[inline]
    fn tick(&mut self, xl: f32, xr: f32) -> (f32, f32) {
        let g = self.er_in.cur;
        let (el, er) = self.er.tick(xl * g, xr * g);
        let (tl, tr) = self.space.tank(xl, xr);
        self.space.out.tick(tl + el, tr + er)
    }
}

pub(super) struct VintageReverb {
    space: Space<16>,
    /// The rear outputs' extra delay.
    rear_line: Vec<DelayLine>,
    rear_pos: Pos,
    rear: Param,
    xformer: Param,
    /// Rear darkening and transformer low-end / top states.
    z: [[f32; 3]; 2],
    a_rear: f32,
    a_low: f32,
    a_top: f32,
    a: f32,
    clock: usize,
    primed: bool,
}

impl VintageReverb {
    pub fn new(ctx: FxContext) -> Self {
        let sr = ctx.sample_rate;
        let cfg = Config {
            rel: spread(0.6, 1.4, 31),
            max_size_ms: 80.0,
            max_pre_ms: 120.0,
            loop_ap: Some([0.15, 0.3]),
            diff_ms: [3.7, 5.3, 8.9, 12.1],
            diff_ref_ms: 60.0,
            seed: 0xE250,
        };
        let mut space = Space::new(sr, &cfg);
        space.cal = 1.39;
        let mut rear_pos = Pos::default();
        rear_pos.set(0.017 * sr, false);
        Self {
            space,
            rear_line: vec![DelayLine::new((0.02 * sr) as usize), DelayLine::new((0.02 * sr) as usize)],
            rear_pos,
            rear: Param::default(),
            xformer: Param::default(),
            z: [[0.0; 3]; 2],
            a_rear: pole(6500.0, sr),
            a_low: pole(120.0, sr),
            a_top: pole(14000.0, sr),
            a: rate(40.0, sr),
            clock: 0,
            primed: false,
        }
    }
}

impl FxAlgo for VintageReverb {
    fn set_params(&mut self, p: &Params) {
        let dcy = p.f("dcy");
        let m = p.f("mod");
        let s = Settings {
            pre_ms: p.f("pdel"),
            size_ms: 40.0 + 8.0 * dcy,
            decay: Decay::three_band(dcy, p.f("lmult"), p.f("hmult"), 400.0, 3000.0),
            diffusion: 0.6,
            shape: 0.4,
            build_ms: 25.0,
            mod_hz: mod_rate(m),
            mod_ms: 0.08 + 0.5 * m / 100.0,
            lc: p.f("lc"),
            hc: p.f("hc"),
            width: 1.0,
            gain: 1.0,
            input: 1.0,
            ..Settings::default()
        };
        self.space.set(&s);
        self.rear.set(p.choice("out") as f32);
        self.xformer.set(if p.on("trans") { 1.0 } else { 0.0 });
        if !self.primed {
            self.primed = true;
            self.space.snap();
            self.rear.snap();
            self.xformer.snap();
        }
    }

    fn process(&mut self, left: &mut [f32], right: &mut [f32]) {
        run(self, left, right);
    }

    fn reset(&mut self) {
        self.space.clear();
        for l in &mut self.rear_line {
            l.clear();
        }
        self.z = [[0.0; 3]; 2];
    }
}

impl Voice for VintageReverb {
    fn clock(&mut self) -> &mut usize {
        &mut self.clock
    }

    fn control(&mut self) {
        self.space.control();
        self.rear.step(self.a);
        self.xformer.step(self.a);
    }

    #[inline]
    fn tick(&mut self, xl: f32, xr: f32) -> (f32, f32) {
        let (tl, tr) = self.space.tank(xl, xr);
        let rear = self.rear.cur;
        let xf = self.xformer.cur;
        let mut out = [tl, tr];
        for ((o, line), z) in out.iter_mut().zip(&mut self.rear_line).zip(&mut self.z) {
            line.push(*o);
            if rear > 0.0 {
                let d = self.rear_pos.read(line);
                z[0] += self.a_rear * (d - z[0]);
                *o += rear * (z[0] - *o);
            }
            if xf > 0.0 {
                // Low-end weight (+~2 dB below 120 Hz), soft saturation, top roll-off.
                z[1] += self.a_low * (*o - z[1]);
                let c = colour(*o + 0.25 * z[1], 0.5);
                z[2] += self.a_top * (c - z[2]);
                *o += xf * (z[2] - *o);
            }
        }
        // The rear pair is the more diffuse one: wider.
        let (m, s) = (0.5 * (out[0] + out[1]), 0.5 * (out[0] - out[1]) * (1.0 + 0.3 * rear));
        self.space.out.tick(m + s, m - s)
    }
}

pub(super) struct VintagePlate {
    space: Space<16>,
    tilt: Param,
    gains: (f32, f32),
    z: [f32; 2],
    a_tilt: f32,
    a: f32,
    /// The drive into the plate (its VU, readout 0: a VU level, 0 VU at -18 dBFS).
    vu: VuMeter,
    clock: usize,
    primed: bool,
}

impl VintagePlate {
    pub fn new(ctx: FxContext) -> Self {
        let sr = ctx.sample_rate;
        let cfg = Config {
            rel: spread(0.5, 1.5, 41),
            max_size_ms: 50.0,
            max_pre_ms: 250.0,
            loop_ap: Some([0.2, 0.25]),
            diff_ms: [1.1, 1.7, 2.6, 3.9],
            diff_ref_ms: 28.0,
            seed: 0xE140,
        };
        let mut space = Space::new(sr, &cfg);
        space.cal = 0.85;
        Self {
            space,
            tilt: Param::default(),
            gains: (1.0, 1.0),
            z: [0.0; 2],
            a_tilt: pole(1000.0, sr),
            a: rate(40.0, sr),
            vu: VuMeter::new(sr, -18.0),
            clock: 0,
            primed: false,
        }
    }
}

impl FxAlgo for VintagePlate {
    fn set_params(&mut self, p: &Params) {
        let s = Settings {
            pre_ms: p.f("pdel"),
            size_ms: 45.0,
            decay: Decay::three_band(p.f("dcy"), 0.75, 0.6, 200.0, 5000.0),
            diffusion: 0.6,
            shape: 0.15,
            build_ms: 3.0,
            mod_hz: 0.2,
            mod_ms: 0.03,
            lc: p.f("lc"),
            hc: 16000.0,
            width: 1.0,
            gain: 1.0,
            input: 1.0,
            ..Settings::default()
        };
        self.space.set(&s);
        self.tilt.set(p.f("col") / 20.0);
        if !self.primed {
            self.primed = true;
            self.space.snap();
            self.tilt.snap();
            self.update_tilt();
        }
    }

    fn process(&mut self, left: &mut [f32], right: &mut [f32]) {
        run(self, left, right);
    }

    fn reset(&mut self) {
        self.space.clear();
        self.z = [0.0; 2];
        self.vu.reset();
    }

    fn readouts(&self, out: &mut [f32; FX_READOUTS]) {
        out[0] = self.vu.level();
    }
}

impl VintagePlate {
    /// COLOR: +-6 dB shelves either side of 1 kHz, in opposite directions.
    fn update_tilt(&mut self) {
        let db = 6.0 * self.tilt.cur;
        self.gains = (fader(-db), fader(db));
    }
}

impl Voice for VintagePlate {
    fn clock(&mut self) -> &mut usize {
        &mut self.clock
    }

    fn control(&mut self) {
        self.space.control();
        if self.tilt.step(self.a) {
            self.update_tilt();
        }
    }

    #[inline]
    fn tick(&mut self, xl: f32, xr: f32) -> (f32, f32) {
        self.vu.tick(0.5 * (xl + xr));
        let (tl, tr) = self.space.tank(xl, xr);
        let (gl, gh) = self.gains;
        let mut out = [tl, tr];
        for (o, z) in out.iter_mut().zip(&mut self.z) {
            *z += self.a_tilt * (*o - *z);
            *o = gl * *z + gh * (*o - *z);
        }
        self.space.out.tick(out[0], out[1])
    }
}
