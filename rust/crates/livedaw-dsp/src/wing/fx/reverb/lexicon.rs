//! The algorithmic reverbs modelled on the Lexicon 480L banks (hall, room,
//! chamber, plate, concert hall, ambience) and the PCM70-style blue plate.
//!
//! All are a 16-line FDN voice ([`Space`]) with in-loop allpasses and the
//! controls the WING manual describes:
//! - DECAY is the mid-band RT60 and BASS MULT multiplies it below the
//!   crossover (500-600 Hz, or XOVER). On the 480L banks SIZE is the master
//!   of DECAY: the RT60 equals DECAY at the default SIZE and scales with the
//!   square root of the line lengths (a bigger room rings longer).
//! - DAMPING is a 6 dB/oct high cut ahead of everything (early reflections
//!   and tail alike); the tank also lets the highs above it die twice as fast.
//! - SIZE scales every line (the room's longest dimension), DIFFUSION the
//!   input allpasses (few loud vs many quiet early reflections).
//! - SHAPE and SPREAD set the build-up envelope: SHAPE its steepness (low =
//!   steep), SPREAD (with SIZE) how long it takes to unfold.
//! - MOD SPD / SPIN / CHORUS randomly vary the delay times (and so pitch).
//!
//! Variants:
//! - HALL: widely spread lines.
//! - ROOM: SIZE in metres, pre-echoes: a delay per side (ECHO) fed from after
//!   the diffusion back to before it (FEED, which is also the echoes' level).
//! - CHAMBER: evenly spread lines; ECHO L/R time and output level per side.
//! - PLATE / BPLATE: stronger in-loop allpasses, little pre-diffusion. PLATE's
//!   ATTACK sets how fast it builds (low = slower, over at most 50 ms) and it
//!   has ROOM's pre-echoes; BPLATE adds MOD depth / SPD and XOVER.
//! - CONCERT: a reflection cluster per side (REFL time and level) ahead of a
//!   large hall; DEPTH moves the listener back: a louder, later-building tail.
//! - AMBI: the build-up taps are the sound; TAIL GAIN adds the late reverb.

use super::parts::{fader, rate, run, spread, Decay, Echo, Param, Taps, Voice};
use super::space::{Config, Settings, Space};
use crate::wing::fx::{FxAlgo, FxContext};
use crate::wing::Params;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(super) enum Kind {
    Hall,
    Room,
    Chamber,
    Plate,
    Concert,
    Ambience,
    BluePlate,
}

/// MOD SPD / SPIN (0..100) to a modulation rate in Hz.
pub(super) fn mod_rate(v: f32) -> f32 {
    0.05 + 2.95 * (v / 100.0).clamp(0.0, 1.0).powf(1.5)
}

/// SIZE's hold on DECAY: the RT60 scale for a mean line length `size_ms`
/// against the default's `ref_ms`.
fn size_scale(size_ms: f32, ref_ms: f32) -> f32 {
    (size_ms / ref_ms).sqrt().clamp(0.3, 1.8)
}

/// SPREAD (0..50) with SIZE: the build-up's duration.
fn build_time(spread: f32, size_ms: f32) -> f32 {
    size_ms * (0.05 + 2.0 * spread / 50.0)
}

/// Reflection cluster shape: (delay after the first reflection in ms, gain).
const CLUSTER: [(f32, f32); 4] = [(0.0, 1.0), (6.3, -0.62), (13.9, 0.45), (23.1, -0.3)];

pub(super) struct Lexicon {
    kind: Kind,
    sr: f32,
    space: Space<16>,
    refl: Option<Taps<4>>,
    /// Tail level: AMBI's TAIL GAIN, CONCERT's DEPTH.
    tail: Param,
    a: f32,
    clock: usize,
    primed: bool,
}

impl Lexicon {
    pub fn new(kind: Kind, ctx: FxContext) -> Self {
        let sr = ctx.sample_rate;
        let lex_diff = [4.8, 3.6, 12.7, 9.3];
        let plate_diff = [1.6, 2.3, 3.7, 5.3];
        // In-loop allpasses everywhere (as in Lexicon's tanks): stronger in
        // the plates, lighter in the rooms and halls.
        let plate_ap = Some([0.2, 0.25]);
        let room_ap = Some([0.15, 0.3]);
        let (rel, max_size, loop_ap, diff_ms, diff_ref, seed, cal) = match kind {
            Kind::Hall => (spread(0.55, 1.45, 11), 130.0, room_ap, lex_diff, 80.0, 0x4A11, 1.75),
            Kind::Room => (spread(0.6, 1.4, 12), 150.0, room_ap, lex_diff, 40.0, 0x2004, 1.57),
            Kind::Chamber => (spread(0.72, 1.28, 13), 150.0, room_ap, lex_diff, 50.0, 0xC4A3, 1.74),
            Kind::Plate => (spread(0.5, 1.5, 14), 110.0, plate_ap, plate_diff, 30.0, 0x91A7, 1.12),
            Kind::Concert => (spread(0.55, 1.45, 15), 155.0, room_ap, lex_diff, 90.0, 0xC0C7, 1.5),
            Kind::Ambience => (spread(0.6, 1.4, 16), 75.0, Some([0.15, 0.25]), [3.1, 4.7, 7.9, 11.3], 30.0, 0xA3B1, 0.6),
            Kind::BluePlate => (spread(0.5, 1.5, 17), 130.0, plate_ap, plate_diff, 40.0, 0xB1E9, 1.48),
        };
        let cfg = Config { rel, max_size_ms: max_size, max_pre_ms: 200.0, loop_ap, diff_ms, diff_ref_ms: diff_ref, seed };
        let mut space = Space::new(sr, &cfg);
        space.cal = cal;
        space.echo = match kind {
            Kind::Room | Kind::Plate => Some(Echo::new(sr, 1200.0)),
            Kind::Chamber => Some(Echo::new(sr, 300.0)),
            _ => None,
        };
        let refl = if kind == Kind::Concert { Some(Taps::new(sr, 1230.0, 4)) } else { None };
        Self { kind, sr, space, refl, tail: Param { cur: 1.0, target: 1.0 }, a: rate(40.0, sr), clock: 0, primed: false }
    }
}

impl FxAlgo for Lexicon {
    fn set_params(&mut self, p: &Params) {
        let ms = 0.001 * self.sr;
        let mut s = Settings { pre_ms: p.f("pdel"), lc: p.f("lc"), hc: p.f("hc"), damp_hz: p.f("damp"), ..Settings::default() };
        let damp = p.f("damp");
        match self.kind {
            Kind::Hall => {
                s.size_ms = 10.0 + 1.2 * p.f("size");
                let t60 = p.f("dcy") * size_scale(s.size_ms, 82.0);
                s.decay = Decay::lexicon(t60, p.f("mult"), 500.0, damp);
                s.shape = p.f("shp") / 250.0;
                s.build_ms = build_time(p.f("sprd"), s.size_ms);
                s.diffusion = p.f("diff") / 30.0;
                s.mod_hz = mod_rate(p.f("mspd"));
                s.mod_ms = 0.2 + 0.004 * s.size_ms;
            }
            Kind::Room | Kind::Chamber | Kind::Plate => {
                let plate = self.kind == Kind::Plate;
                s.size_ms = p.f("size") * if plate { 1.4 } else { 1.94 };
                let reference = match self.kind {
                    Kind::Room => 38.8,
                    Kind::Chamber => 58.2,
                    _ => 42.0,
                };
                let t60 = p.f("dcy") * size_scale(s.size_ms, reference);
                s.decay = Decay::lexicon(t60, p.f("mult"), 600.0, damp);
                s.build_ms = build_time(p.f("sprd"), s.size_ms);
                if plate {
                    // ATTACK: high = immediate onset, low = a fade-in of up to 50 ms.
                    s.shape = 0.35;
                    s.ramp_ms = 50.0 * (1.0 - p.f("att") / 100.0);
                } else {
                    s.shape = p.f("shp") / 250.0;
                }
                s.diffusion = p.f("diff") / 100.0;
                s.mod_hz = mod_rate(p.f("spin"));
                s.mod_ms = 0.15 + 0.003 * s.size_ms;
                if let Some(e) = &mut self.space.echo {
                    if self.kind == Kind::Chamber {
                        e.set(0, p.f("ecl") * ms, 0.0, fader(p.f("ell")));
                        e.set(1, p.f("ecr") * ms, 0.0, fader(p.f("elr")));
                    } else {
                        let (fl, fr) = (p.f("efl") / 100.0, p.f("efr") / 100.0);
                        e.set(0, p.f("ecl") * ms, fl, fl);
                        e.set(1, p.f("ecr") * ms, fr, fr);
                    }
                }
            }
            Kind::Concert => {
                s.size_ms = 2.0 * p.f("size");
                let t60 = p.f("dcy") * size_scale(s.size_ms, 90.0);
                s.decay = Decay::lexicon(t60, p.f("mult"), 500.0, damp);
                let depth = (p.f("depth") - 1.0) / 15.0;
                s.shape = (0.8 * p.f("shp") / 50.0 + 0.3 * depth).min(1.0);
                s.build_ms = build_time(p.f("sprd"), s.size_ms) * (0.6 + 0.8 * depth);
                s.diffusion = p.f("diff") / 100.0;
                s.mod_hz = mod_rate(p.f("spin"));
                s.mod_ms = 0.1 + 2.4 * p.f("crs") / 100.0;
                self.tail.set(0.5 + depth);
                if let Some(t) = &mut self.refl {
                    for (ch, (time, lvl)) in [("rfl", "rfll"), ("rfr", "rflr")].into_iter().enumerate() {
                        let t0 = p.f(time) * ms;
                        let g = fader(p.f(lvl));
                        // Each side's cluster spreads a little wider than the other's.
                        let spread = if ch == 0 { 1.0 } else { 1.13 };
                        for (k, &(dt, kg)) in CLUSTER.iter().enumerate() {
                            t.set(ch, k, t0 + dt * spread * ms, g * kg);
                        }
                    }
                }
            }
            Kind::Ambience => {
                let size = p.f("size");
                s.size_ms = 14.0 + 0.6 * size;
                let t60 = p.f("dcy") * size_scale(s.size_ms, 32.0);
                s.decay = Decay::lexicon(t60, 1.0, 500.0, damp);
                s.shape = 0.2;
                s.build_ms = 4.0 + 0.85 * size;
                s.diffusion = p.f("diff") / 30.0;
                s.mod_hz = mod_rate(p.f("mod"));
                s.mod_ms = 0.15;
                // TAIL GAIN: the late reverb's level (100 = on a par with the early sound).
                self.tail.set(p.f("tail") / 100.0);
            }
            Kind::BluePlate => {
                s.size_ms = 10.0 + 1.2 * p.f("size");
                s.decay = Decay::lexicon(p.f("dcy"), p.f("mult"), p.f("xover"), damp);
                s.shape = 0.3;
                s.build_ms = 12.0;
                s.diffusion = p.f("diff") / 30.0;
                s.mod_hz = mod_rate(p.f("mspd"));
                s.mod_ms = 0.03 + 1.5 * (p.f("mdep") - 1.0) / 49.0;
            }
        }
        self.space.set(&s);
        if !self.primed {
            self.primed = true;
            self.space.snap();
            self.tail.snap();
            if let Some(t) = &mut self.refl {
                t.snap();
            }
        }
    }

    fn process(&mut self, left: &mut [f32], right: &mut [f32]) {
        run(self, left, right);
    }

    fn reset(&mut self) {
        self.space.clear();
        if let Some(t) = &mut self.refl {
            t.clear();
        }
    }
}

impl Voice for Lexicon {
    fn clock(&mut self) -> &mut usize {
        &mut self.clock
    }

    fn control(&mut self) {
        self.space.control();
        self.tail.step(self.a);
        if let Some(t) = &mut self.refl {
            t.control(self.a, false);
        }
    }

    #[inline]
    fn tick(&mut self, xl: f32, xr: f32) -> (f32, f32) {
        let (xl, xr) = self.space.damp(xl, xr);
        let g = self.tail.cur;
        if self.kind == Kind::Ambience {
            let ((el, er), (tl, tr)) = self.space.tank_split(xl, xr);
            return self.space.out.tick(0.8 * el + g * tl, 0.8 * er + g * tr);
        }
        // Pre-echoes / reflections are blended with the tail.
        let (el, er) = match &mut self.refl {
            Some(t) => t.tick(xl, xr),
            None => (0.0, 0.0),
        };
        let (tl, tr) = self.space.tank(xl, xr);
        let (pl, pr) = self.space.echo_out;
        self.space.out.tick(g * tl + el + pl, g * tr + er + pr)
    }
}
