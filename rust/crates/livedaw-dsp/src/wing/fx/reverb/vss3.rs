//! VSS3 (tc electronic VSS3): separate early reflections and reverb, as on
//! the original.
//! - Early reflections: one of 24 patterns (ER TYPE: halls, rooms, small
//!   spaces, outdoors; first reflection, span, density, level fall and tone
//!   per type) on their own pre-delay (ER PDLY), scaled by ER SIZE; ER POS
//!   NEAR makes the first reflections stronger, DIST flatter, later and
//!   darker. DIFFUSE smears them (few loud vs many quiet reflections) through
//!   a short allpass chain; ER BAL, ER LCUT, ER COL (a tilt around 1 kHz) and
//!   ER LVL follow.
//! - Reverb: a 16-line tank on its own pre-delay (REV PDLY) whose size grows
//!   with DECAY. LO / LOMID / HIMID / HI DCY multiply DECAY in four bands
//!   split at LO / MID / HI X/O; REV TYPE sets the build-up, diffusion and
//!   modulation character (SMOOTH, NATURAL, ALIVE, FAST, X-WIDE, ALIVE2) and
//!   DIFFUSE offsets the diffusion. HI SOFT softens (or hardens) the high band
//!   with a +-6 dB shelf at HI X/O, LO SHV / LO DAMP is a low shelf cut, REV
//!   WID the width, REV BAL / REV LVL the output.
//!   MOD TYPE picks a modulation rate/depth pair that MOD RATE (x0.25..x4)
//!   and MOD WID (depth, 100 = nominal) scale.
//! - HI CUT low-passes both. PRESET, LOAD and VIEW are console UI only.

use super::parts::{fader, hash01, pole, rate, run, sign, spread, Decay, Diffuser, Param, Taps, Voice};
use super::space::{Config, Settings, Space};
use crate::wing::fx::blocks::{db_to_gain, Svf};
use crate::wing::fx::{FxAlgo, FxContext};
use crate::wing::Params;

/// Early reflections per side.
const ER: usize = 16;

/// ER TYPE patterns: first reflection and span (ms, at MED size), number of
/// reflections, how far their level falls over the span (negative: rises)
/// and tone (-1 dark .. 1 bright).
const ER_TYPES: [(f32, f32, usize, f32, f32); 24] = [
    (12.0, 80.0, 16, 0.7, 0.0),   // ROYAL
    (10.0, 60.0, 14, 0.7, 0.0),   // THEATRE
    (18.0, 120.0, 16, 0.6, -0.4), // CHURCH
    (25.0, 150.0, 12, 0.4, 0.3),  // GAS
    (15.0, 90.0, 16, 0.65, 0.0),  // CONCERT
    (14.0, 100.0, 16, 0.6, 0.1),  // ROYAL2
    (3.0, 30.0, 12, 0.8, 0.2),    // V1-NEAR
    (5.0, 40.0, 10, 0.5, 0.5),    // V2-HARD
    (8.0, 70.0, 16, 0.6, 0.0),    // V3-SPREAD
    (4.0, 60.0, 16, -0.6, 0.0),   // V4-BUILD
    (2.0, 50.0, 16, 0.5, 0.0),    // V5-RANDOM
    (60.0, 20.0, 3, 0.9, 0.2),    // SLAP
    (1.5, 8.0, 10, 0.8, -0.2),    // CAR
    (1.0, 6.0, 10, 0.7, 0.3),     // PHONEBTH
    (2.0, 15.0, 12, 0.5, 0.6),    // BATHROOM
    (4.0, 25.0, 12, 0.7, 0.0),    // CONFRM9
    (8.0, 50.0, 14, 0.7, 0.0),    // CONFRM30
    (6.0, 45.0, 14, 0.5, 0.2),    // GARAGE
    (20.0, 120.0, 16, 0.5, 0.4),  // SWIMSTDM
    (25.0, 140.0, 14, 0.5, 0.1),  // AIRPORT
    (8.0, 80.0, 6, 0.6, 0.1),     // STREET
    (4.0, 50.0, 10, 0.5, 0.3),    // ALLEY
    (15.0, 100.0, 8, 0.6, 0.0),   // PIAZA
    (10.0, 120.0, 12, 0.7, -0.5), // FOREST
];

/// REV TYPE: base diffusion, build-up shape, build-up span (x size),
/// modulation depth scale, extra width.
const REV_TYPES: [(f32, f32, f32, f32, f32); 6] = [
    (0.8, 0.45, 0.8, 1.0, 0.0),  // SMOOTH
    (0.65, 0.35, 0.6, 0.7, 0.0), // NATURAL
    (0.45, 0.25, 0.5, 1.3, 0.0), // ALIVE
    (0.8, 0.05, 0.25, 0.8, 0.0), // FAST
    (0.7, 0.4, 0.7, 1.0, 0.3),   // X-WIDE
    (0.5, 0.3, 0.9, 1.6, 0.0),   // ALIVE2
];

/// MOD TYPE: rate (Hz) and depth (ms).
const MOD_TYPES: [(f32, f32); 6] = [(0.35, 0.25), (0.6, 0.35), (0.9, 0.5), (1.4, 0.3), (0.2, 0.9), (2.2, 0.2)];

/// REV WID: stereo width.
const WIDTHS: [f32; 4] = [0.35, 0.8, 1.1, 1.4];

pub(super) struct Vss3 {
    sr: f32,
    space: Space<16>,
    er: Taps<ER>,
    er_diff: [Diffuser; 2],
    er_diffusion: Param,
    er_hp: [Svf; 2],
    er_lp: [Svf; 2],
    er_lc: Param,
    hcut: Param,
    /// ER tilt (dB at the extremes) and its shelf gains.
    er_tilt: Param,
    er_gains: (f32, f32),
    er_bal: Param,
    er_lvl: Param,
    /// Reverb shelves: LO DAMP (gain, corner) and HI SOFT (gain).
    lo_gain: Param,
    lo_hz: Param,
    hi_gain: Param,
    hi_hz: Param,
    a_lo: f32,
    z: [[f32; 3]; 2],
    a_tilt: f32,
    a_soft: f32,
    a: f32,
    force: bool,
    clock: usize,
    primed: bool,
}

impl Vss3 {
    pub fn new(ctx: FxContext) -> Self {
        let sr = ctx.sample_rate;
        let cfg = Config {
            rel: spread(0.55, 1.45, 71),
            max_size_ms: 95.0,
            max_pre_ms: 200.0,
            loop_ap: Some([0.15, 0.3]),
            diff_ms: [4.3, 3.1, 11.3, 8.7],
            diff_ref_ms: 60.0,
            seed: 0x0553,
        };
        let mut space = Space::new(sr, &cfg);
        space.cal = 1.0;
        Self {
            sr,
            space,
            // ER PDLY + the largest pattern at LRG size, moved later by DIST.
            er: Taps::new(sr, 100.0 + 1.6 * 175.0 + 30.0, ER),
            er_diff: [Diffuser::new(sr, [1.7, 2.9, 4.3, 6.1], 1.0), Diffuser::new(sr, [1.9, 2.6, 4.7, 5.7], 1.0)],
            er_diffusion: Param::default(),
            er_hp: [Svf::default(); 2],
            er_lp: [Svf::default(); 2],
            er_lc: Param::default(),
            hcut: Param::default(),
            er_tilt: Param::default(),
            er_gains: (1.0, 1.0),
            er_bal: Param::default(),
            er_lvl: Param::default(),
            lo_gain: Param::default(),
            lo_hz: Param::default(),
            hi_gain: Param::default(),
            hi_hz: Param::default(),
            a_lo: 1.0,
            z: [[0.0; 3]; 2],
            a_tilt: pole(1000.0, sr),
            a_soft: pole(4000.0, sr),
            a: rate(40.0, sr),
            force: true,
            clock: 0,
            primed: false,
        }
    }

    fn set_reflections(&mut self, p: &Params) -> f32 {
        let ms = 0.001 * self.sr;
        let (first, span, n, fall, tone) = ER_TYPES[p.choice("ertype").min(ER_TYPES.len() - 1)];
        let scale = [0.6, 1.0, 1.6][p.choice("ersize").min(2)];
        let dist = p.choice("erpos") == 1;
        let pre = p.f("erpdly");
        let slap = n <= 3;
        for ch in 0..2 {
            let seed = 0x0E5 + 97 * p.choice("ertype") as u32 + ch as u32;
            let mut taps = [(0.0f32, 0.0f32); ER];
            let mut norm = 0.0;
            for (k, t) in taps.iter_mut().enumerate().take(n) {
                let jit = hash01(k as u32, seed);
                let u = if slap { k as f32 * 0.5 + 0.1 * jit } else { ((k as f32 + 0.85 * jit) / n as f32).powf(1.2) };
                let fall = if dist { 0.5 * fall } else { fall };
                let mut g = if fall >= 0.0 { 1.0 - fall * u } else { 1.0 + fall * (1.0 - u) };
                if slap && k > 0 {
                    g *= 0.3;
                }
                if !dist && k < 2 {
                    g *= 1.5;
                }
                let time = first * if dist { 1.3 } else { 1.0 } + span * u;
                *t = ((pre + scale * time) * ms, g * sign(k as u32, seed ^ 0x2B));
                norm += g * g;
            }
            let norm = 1.0 / norm.sqrt().max(1e-3);
            for (k, (pos, g)) in taps.into_iter().enumerate() {
                self.er.set(ch, k, pos.max(1.0), g * norm);
            }
        }
        // Tone: the type's, darker when distant.
        8.0 * (tone - if dist { 0.3 } else { 0.0 })
    }
}

impl FxAlgo for Vss3 {
    fn set_params(&mut self, p: &Params) {
        let tone = self.set_reflections(p);
        self.er_tilt.set((tone + 8.0 * p.f("ercol") / 40.0).clamp(-12.0, 12.0));
        self.er_lc.set(p.f("erlc"));
        self.er_bal.set(p.f("erbal") / 100.0);
        self.er_lvl.set(fader(p.f("erlvl")));
        self.hcut.set(p.f("hcut"));

        let dcy = p.f("dcy");
        let (diff, shape, build, mod_scale, extra_w) = REV_TYPES[p.choice("rvtype").min(REV_TYPES.len() - 1)];
        let (mod_hz, mod_ms) = MOD_TYPES[p.choice("mtype").min(MOD_TYPES.len() - 1)];
        let size_ms = 22.0 + 18.0 * (1.0 + 2.0 * dcy).ln();
        let mults = [p.f("ldcy"), p.f("lmdcy"), p.f("hmdcy"), p.f("hdcy")];
        let s = Settings {
            pre_ms: p.f("rvpdly"),
            size_ms,
            decay: Decay { bands: mults.map(|m| 1.0 / (dcy * m).max(0.01)), xo: [p.f("lxo"), p.f("mxo"), p.f("hxo")] },
            diffusion: (diff + p.f("diff") / 100.0).clamp(0.0, 1.0),
            shape,
            build_ms: build * size_ms,
            mod_hz: mod_hz * 2.0f32.powf(p.f("mrate") / 50.0),
            mod_ms: mod_ms * mod_scale * p.f("mwid") / 100.0,
            lc: 20.0,
            hc: p.f("hcut"),
            width: WIDTHS[p.choice("rvwid").min(3)] + extra_w,
            gain: fader(p.f("rvlvl")),
            input: 1.0,
            ..Settings::default()
        };
        self.space.set(&s);
        self.space.out.bal.set(p.f("rvbal") / 100.0);
        self.lo_gain.set(db_to_gain(p.f("lsdmp")));
        self.lo_hz.set(p.f("lshv"));
        self.hi_gain.set(db_to_gain(-6.0 * p.f("hsoft") / 50.0));
        self.hi_hz.set(p.f("hxo"));
        self.er_diffusion.set(0.8 * (p.f("diff") + 50.0) / 100.0);
        if !self.primed {
            self.primed = true;
            self.space.snap();
            self.er.snap();
            for q in [
                &mut self.er_tilt,
                &mut self.er_lc,
                &mut self.er_bal,
                &mut self.er_lvl,
                &mut self.hcut,
                &mut self.lo_gain,
                &mut self.lo_hz,
                &mut self.hi_gain,
                &mut self.hi_hz,
                &mut self.er_diffusion,
            ] {
                q.snap();
            }
            self.force = true;
        }
    }

    fn process(&mut self, left: &mut [f32], right: &mut [f32]) {
        run(self, left, right);
    }

    fn reset(&mut self) {
        self.space.clear();
        self.er.clear();
        for d in &mut self.er_diff {
            d.clear();
        }
        for f in self.er_hp.iter_mut().chain(&mut self.er_lp) {
            f.reset();
        }
        self.z = [[0.0; 3]; 2];
    }
}

impl Voice for Vss3 {
    fn clock(&mut self) -> &mut usize {
        &mut self.clock
    }

    fn control(&mut self) {
        let force = std::mem::take(&mut self.force);
        let a = self.a;
        self.space.control();
        self.er.control(a, force);
        if self.er_lc.step(a) || force {
            for f in &mut self.er_hp {
                f.set(self.er_lc.cur, 0.707, self.sr);
            }
        }
        if self.hcut.step(a) || force {
            for f in &mut self.er_lp {
                f.set(self.hcut.cur, 0.707, self.sr);
            }
        }
        if self.er_tilt.step(a) || force {
            let h = 0.5 * self.er_tilt.cur;
            self.er_gains = (db_to_gain(-h), db_to_gain(h));
        }
        if self.lo_hz.step(a) || force {
            self.a_lo = pole(self.lo_hz.cur, self.sr);
        }
        if self.hi_hz.step(a) || force {
            self.a_soft = pole(self.hi_hz.cur, self.sr);
        }
        if self.er_diffusion.step(a) || force {
            for d in &mut self.er_diff {
                d.set(1.0, self.er_diffusion.cur, false);
            }
        }
        for q in [&mut self.er_bal, &mut self.er_lvl, &mut self.lo_gain, &mut self.hi_gain] {
            q.step(a);
        }
    }

    #[inline]
    fn tick(&mut self, xl: f32, xr: f32) -> (f32, f32) {
        let (el, er) = self.er.tick(xl, xr);
        let (tl, tr) = self.space.tank(xl, xr);
        let (lo_g, hi_g) = (self.lo_gain.cur, self.hi_gain.cur);
        let (gl, gh) = self.er_gains;
        let mut rev = [tl, tr];
        let mut early = [el, er];
        for c in 0..2 {
            let z = &mut self.z[c];
            // LO DAMP: low shelf cut; HI SOFT: high shelf.
            let x = rev[c];
            z[0] += self.a_lo * (x - z[0]);
            let x = x + (lo_g - 1.0) * z[0];
            z[1] += self.a_soft * (x - z[1]);
            rev[c] = z[1] + hi_g * (x - z[1]);
            // DIFFUSE, ER COL tilt, then ER LCUT and HI CUT.
            let e = self.er_diff[c].tick(early[c]);
            z[2] += self.a_tilt * (e - z[2]);
            let e = gl * z[2] + gh * (e - z[2]);
            early[c] = self.er_lp[c].tick(self.er_hp[c].tick(e).hp).lp;
        }
        let (rl, rr) = self.space.out.tick(rev[0], rev[1]);
        let b = self.er_bal.cur;
        let lvl = self.er_lvl.cur;
        (rl + early[0] * lvl * (1.0 - b).min(1.0), rr + early[1] * lvl * (1.0 + b).min(1.0))
    }
}
