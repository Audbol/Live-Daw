//! Pitch and sub: stereo and dual pitch, double vocal, pitch fix, sub octaver,
//! sub monster and psycho bass.
//!
//! The pitch devices share a splicing delay-line shifter ([`shifter`]) whose
//! splice points are chosen by correlation, so harmonic material shifts
//! without flanging. The pitch fix adds a YIN pitch detector ([`yin`]); the
//! sub devices ([`sub`]) use analog-style octave dividers and waveshaping.

mod shifter;
mod sub;
mod yin;

use super::blocks::{coef, db_to_gain, equal_power, Biquad, BiquadCoeffs, Noise, Smoothed};
use super::{FxAlgo, FxContext};
use crate::wing::Params;
use shifter::{Shifter, ShifterSpec};
use std::f32::consts::{FRAC_PI_4, LN_2};

/// The devices this module implements.
pub(super) const TOKENS: &[&str] = &["PITCH", "D-PITCH", "DOUBLE", "PCORR", "SUB", "SUB-M", "P-BASS"];

pub(super) fn create(token: &str, ctx: FxContext) -> Option<Box<dyn FxAlgo>> {
    if !TOKENS.contains(&token) {
        return None;
    }
    Some(match token {
        "PITCH" => Primed::boxed(StereoPitch::new(ctx)),
        "D-PITCH" => Primed::boxed(DualPitch::new(ctx)),
        "DOUBLE" => Primed::boxed(DoubleVocal::new(ctx)),
        "PCORR" => Primed::boxed(PitchFix::new(ctx)),
        "SUB" => Primed::boxed(sub::SubOctaver::new(ctx)),
        "SUB-M" => Primed::boxed(sub::SubMonster::new(ctx)),
        "P-BASS" => Primed::boxed(sub::PsychoBass::new(ctx)),
        _ => return None,
    })
}

/// Starts a new instance at its first parameter set (smoothed controls snap
/// to it instead of gliding there from zero).
struct Primed<T> {
    algo: T,
    primed: bool,
}

impl<T: FxAlgo + 'static> Primed<T> {
    fn boxed(algo: T) -> Box<dyn FxAlgo> {
        Box::new(Self { algo, primed: false })
    }
}

impl<T: FxAlgo> FxAlgo for Primed<T> {
    fn set_params(&mut self, p: &Params) {
        self.algo.set_params(p);
        if !self.primed {
            self.primed = true;
            self.algo.reset();
        }
    }

    fn process(&mut self, left: &mut [f32], right: &mut [f32]) {
        self.algo.process(left, right);
    }

    fn reset(&mut self) {
        self.algo.reset();
    }

    fn meter(&self) -> f32 {
        self.algo.meter()
    }

    fn readouts(&self, out: &mut [f32; super::FX_READOUTS]) {
        self.algo.readouts(out);
    }

    fn latency(&self) -> u32 {
        self.algo.latency()
    }
}

/// Parameter smoothing time (ms).
const SMOOTH_MS: f32 = 20.0;

/// Longest DELAY of the pitch devices (ms).
const MAX_DELAY_MS: f32 = 500.0;

/// The shifter settings of the PITCH and D-PITCH voices.
const PITCH_SHIFTER: ShifterSpec = ShifterSpec {
    max_offset_ms: MAX_DELAY_MS + 5.0,
    min_ms: 1.0,
    window_ms: 40.0,
    search_ms: 16.0,
    corr_ms: 12.0,
    fade_ms: 10.0,
};

/// Semitones to a frequency ratio.
#[inline]
fn semis_to_ratio(semis: f32) -> f32 {
    (semis * (LN_2 / 12.0)).exp()
}

/// A console level (dB, -60 and below = off) as a gain.
#[inline]
fn fader_gain(db: f32) -> f32 {
    if db <= -60.0 {
        0.0
    } else {
        db_to_gain(db)
    }
}

/// MIX for devices that add to the dry signal: (dry, wet) gains for 0..100 %,
/// both at full level at 50 %.
#[inline]
fn add_mix(mix_pct: f32) -> (f32, f32) {
    let m = (mix_pct * 0.01).clamp(0.0, 1.0);
    ((2.0 * (1.0 - m)).min(1.0), (2.0 * m).min(1.0))
}

/// Runs `x` through a cascade of biquads (channel `ch`).
#[inline]
fn chain(filters: &mut [Biquad], x: f32, ch: usize) -> f32 {
    filters.iter_mut().fold(x, |v, f| f.tick(v, ch))
}

/// Constant-power pan, -1..1 to (left, right).
#[inline]
fn pan_gains(pan: f32) -> (f32, f32) {
    let a = (pan.clamp(-1.0, 1.0) + 1.0) * FRAC_PI_4;
    (a.cos(), a.sin())
}

/// LO CUT / HI CUT (12 dB/oct each) with cutoffs that glide per block.
struct CutFilters {
    sr: f32,
    hp: Biquad,
    lp: Biquad,
    lc: f32,
    hc: f32,
    lc_target: f32,
    hc_target: f32,
}

impl CutFilters {
    fn new(sr: f32) -> Self {
        let mut f = Self {
            sr,
            hp: Biquad::default(),
            lp: Biquad::default(),
            lc: 80.0,
            hc: 12_000.0,
            lc_target: 80.0,
            hc_target: 12_000.0,
        };
        f.design();
        f
    }

    fn set(&mut self, lc: f32, hc: f32) {
        self.lc_target = lc.clamp(10.0, 1000.0);
        self.hc_target = hc.clamp(100.0, 22_000.0);
    }

    fn design(&mut self) {
        let sr = self.sr as f64;
        self.hp.set_coefficients(BiquadCoeffs::high_pass(sr, self.lc as f64, std::f64::consts::FRAC_1_SQRT_2));
        self.lp.set_coefficients(BiquadCoeffs::low_pass(sr, (self.hc as f64).min(sr * 0.45), std::f64::consts::FRAC_1_SQRT_2));
    }

    /// Moves the cutoffs towards their targets (once per block).
    fn update(&mut self) {
        if self.lc == self.lc_target && self.hc == self.hc_target {
            return;
        }
        let glide = |v: f32, t: f32| {
            let r = t / v;
            if (r - 1.0).abs() < 0.005 {
                t
            } else {
                v * r.powf(0.35)
            }
        };
        self.lc = glide(self.lc, self.lc_target);
        self.hc = glide(self.hc, self.hc_target);
        self.design();
    }

    fn snap(&mut self) {
        self.lc = self.lc_target;
        self.hc = self.hc_target;
        self.design();
        self.hp.reset();
        self.lp.reset();
    }

    #[inline]
    fn tick(&mut self, x: f32, ch: usize) -> f32 {
        self.lp.tick(self.hp.tick(x, ch), ch)
    }
}

// ---- PITCH: Stereo Pitch --------------------------------------------------------------

/// Lexicon PCM70 style stereo pitch shifter: SEMITONES and CENT set the
/// interval (+-1 octave), DELAY delays the shifted signal (up to 500 ms), LO
/// CUT / HI CUT band-limit it and MIX crossfades dry and shifted (equal
/// power). Both channels share one shifter's splice points.
struct StereoPitch {
    sr: f32,
    shifter: Shifter,
    cuts: CutFilters,
    ratio: Smoothed,
    delay: Smoothed,
    dry: Smoothed,
    wet: Smoothed,
}

impl StereoPitch {
    fn new(ctx: FxContext) -> Self {
        let sr = ctx.sample_rate;
        Self {
            sr,
            shifter: Shifter::new(PITCH_SHIFTER, sr, true),
            cuts: CutFilters::new(sr),
            ratio: Smoothed::new(1.0, SMOOTH_MS, sr),
            delay: Smoothed::new(0.0, 80.0, sr),
            dry: Smoothed::new(0.0, SMOOTH_MS, sr),
            wet: Smoothed::new(1.0, SMOOTH_MS, sr),
        }
    }
}

impl FxAlgo for StereoPitch {
    fn set_params(&mut self, p: &Params) {
        self.ratio.target = semis_to_ratio(p.f("semi") + p.f("cent") * 0.01);
        self.delay.target = p.f("dly").clamp(0.0, MAX_DELAY_MS) * 0.001 * self.sr;
        self.cuts.set(p.f("lc"), p.f("hc"));
        let (dry, wet) = equal_power(p.f("mix") * 0.01);
        self.dry.target = dry;
        self.wet.target = wet;
    }

    fn process(&mut self, left: &mut [f32], right: &mut [f32]) {
        self.cuts.update();
        for (l, r) in left.iter_mut().zip(right.iter_mut()) {
            let (xl, xr) = (self.cuts.tick(*l, 0), self.cuts.tick(*r, 1));
            let (yl, yr) = self.shifter.tick_stereo(xl, xr, self.ratio.tick(), self.delay.tick());
            let (dry, wet) = (self.dry.tick(), self.wet.tick());
            *l = *l * dry + yl * wet;
            *r = *r * dry + yr * wet;
        }
    }

    fn reset(&mut self) {
        self.shifter.clear();
        self.cuts.snap();
        for s in [&mut self.ratio, &mut self.delay, &mut self.dry, &mut self.wet] {
            s.snap(s.target);
        }
    }
}

// ---- D-PITCH: Dual Pitch --------------------------------------------------------------

/// One voice of the dual pitch.
struct PitchVoice {
    shifter: Shifter,
    ratio: Smoothed,
    delay: Smoothed,
    gl: Smoothed,
    gr: Smoothed,
}

/// Two independent pitch voices on the (mono) input, each with SEMITONES,
/// CENT, DELAY, PAN and LEVEL; LO CUT / HI CUT band-limit the input. The
/// output is the two voices only (the FX slot mixes in the dry signal).
struct DualPitch {
    sr: f32,
    cuts: CutFilters,
    voices: [PitchVoice; 2],
}

impl DualPitch {
    fn new(ctx: FxContext) -> Self {
        let sr = ctx.sample_rate;
        let voice = || PitchVoice {
            shifter: Shifter::new(PITCH_SHIFTER, sr, false),
            ratio: Smoothed::new(1.0, SMOOTH_MS, sr),
            delay: Smoothed::new(0.0, 80.0, sr),
            gl: Smoothed::new(0.0, SMOOTH_MS, sr),
            gr: Smoothed::new(0.0, SMOOTH_MS, sr),
        };
        Self { sr, cuts: CutFilters::new(sr), voices: [voice(), voice()] }
    }
}

impl FxAlgo for DualPitch {
    fn set_params(&mut self, p: &Params) {
        const IDS: [[&str; 5]; 2] = [["semi1", "cent1", "dly1", "pan1", "lvl1"], ["semi2", "cent2", "dly2", "pan2", "lvl2"]];
        for (v, ids) in self.voices.iter_mut().zip(IDS) {
            v.ratio.target = semis_to_ratio(p.f(ids[0]) + p.f(ids[1]) * 0.01);
            v.delay.target = p.f(ids[2]).clamp(0.0, MAX_DELAY_MS) * 0.001 * self.sr;
            let (gl, gr) = pan_gains(p.f(ids[3]) * 0.01);
            let g = fader_gain(p.f(ids[4]));
            v.gl.target = gl * g;
            v.gr.target = gr * g;
        }
        self.cuts.set(p.f("lc"), p.f("hc"));
    }

    fn process(&mut self, left: &mut [f32], right: &mut [f32]) {
        self.cuts.update();
        for (l, r) in left.iter_mut().zip(right.iter_mut()) {
            let x = self.cuts.tick(0.5 * (*l + *r), 0);
            let (mut ol, mut or) = (0.0, 0.0);
            for v in &mut self.voices {
                let y = v.shifter.tick_mono(x, v.ratio.tick(), v.delay.tick());
                ol += y * v.gl.tick();
                or += y * v.gr.tick();
            }
            *l = ol;
            *r = or;
        }
    }

    fn reset(&mut self) {
        self.cuts.snap();
        for v in &mut self.voices {
            v.shifter.clear();
            for s in [&mut v.ratio, &mut v.delay, &mut v.gl, &mut v.gr] {
                s.snap(s.target);
            }
        }
    }
}

// ---- DOUBLE: Double Vocal -------------------------------------------------------------

/// Smooth random wander, about -1..1: a sample-and-hold source through two
/// one-pole low-passes (continuous slope, so the pitch it implies never jumps).
#[derive(Clone, Copy)]
struct Wander {
    noise: Noise,
    seed: u32,
    period: u32,
    count: u32,
    target: f32,
    a: f32,
    s1: f32,
    s2: f32,
}

impl Wander {
    fn new(seed: u32, hz: f32, sr: f32) -> Self {
        let period = ((sr / hz.max(0.01)) as u32).max(1);
        let a = 1.0 - (-std::f32::consts::TAU * hz * 0.7 / sr).exp();
        Self { noise: Noise(seed), seed, period, count: 0, target: 0.0, a, s1: 0.0, s2: 0.0 }
    }

    fn reset(&mut self) {
        *self = Self { noise: Noise(self.seed), count: 0, target: 0.0, s1: 0.0, s2: 0.0, ..*self };
    }

    #[inline]
    fn next(&mut self) -> f32 {
        if self.count == 0 {
            self.count = self.period;
            self.target = self.noise.sample();
        }
        self.count -= 1;
        self.s1 += self.a * (self.target - self.s1);
        self.s2 += self.a * (self.s1 - self.s2);
        self.s2 * 1.6
    }
}

/// A doubler mode: voices, their delays (ms), timing wander (+-ms), pitch
/// wander (+-cents), fixed detune (cents) and pan positions.
struct DoubleMode {
    voices: usize,
    delay: [f32; 4],
    timing: f32,
    drift: f32,
    detune: [f32; 4],
    pan: [f32; 4],
}

/// TIGHT, LOOSE, GROUP, DETUNE, THICK.
const DOUBLE_MODES: [DoubleMode; 5] = [
    DoubleMode {
        voices: 2,
        delay: [17.0, 24.0, 0.0, 0.0],
        timing: 2.5,
        drift: 4.0,
        detune: [0.0; 4],
        pan: [-1.0, 1.0, 0.0, 0.0],
    },
    DoubleMode {
        voices: 2,
        delay: [32.0, 46.0, 0.0, 0.0],
        timing: 10.0,
        drift: 9.0,
        detune: [0.0; 4],
        pan: [-1.0, 1.0, 0.0, 0.0],
    },
    DoubleMode {
        voices: 4,
        delay: [22.0, 35.0, 49.0, 63.0],
        timing: 10.0,
        drift: 12.0,
        detune: [-5.0, 4.0, -9.0, 8.0],
        pan: [-1.0, 1.0, -0.45, 0.45],
    },
    DoubleMode {
        voices: 2,
        delay: [11.0, 15.0, 0.0, 0.0],
        timing: 1.0,
        drift: 2.0,
        detune: [-11.0, 11.0, 0.0, 0.0],
        pan: [-1.0, 1.0, 0.0, 0.0],
    },
    DoubleMode {
        voices: 4,
        delay: [13.0, 19.0, 27.0, 35.0],
        timing: 3.0,
        drift: 5.0,
        detune: [-7.0, 7.0, -15.0, 15.0],
        pan: [-1.0, 1.0, -0.6, 0.6],
    },
];

const MAX_DOUBLE_DELAY_MS: f32 = 90.0;

/// Samples per control update of the doubler's voices.
const DOUBLE_CTRL: usize = 16;

/// One double: a mono shifter whose delay and detune wander slowly. Its
/// controls run at the control rate and are interpolated per sample.
struct DoubleVoice {
    shifter: Shifter,
    timing: Wander,
    pitch: Wander,
    delay: Smoothed,
    timing_depth: Smoothed,
    detune: Smoothed,
    drift: Smoothed,
    gl: Smoothed,
    gr: Smoothed,
    /// Offset (samples) and pan gains at the start and end of the current
    /// control period, and the period's pitch ratio.
    offset: (f32, f32),
    left: (f32, f32),
    right: (f32, f32),
    ratio: f32,
}

impl DoubleVoice {
    fn control(&mut self, max_offset: f32) {
        let offset = (self.delay.tick() + self.timing_depth.tick() * self.timing.next()).clamp(0.0, max_offset);
        let cents = self.detune.tick() + self.drift.tick() * self.pitch.next();
        self.ratio = semis_to_ratio(cents * 0.01);
        self.offset = (self.offset.1, offset);
        self.left = (self.left.1, self.gl.tick());
        self.right = (self.right.1, self.gr.tick());
    }

    fn silent(&self) -> bool {
        self.left.0 + self.left.1 + self.right.0 + self.right.1 < 1e-6
    }

    fn snap(&mut self, max_offset: f32) {
        for s in [&mut self.delay, &mut self.timing_depth, &mut self.detune, &mut self.drift, &mut self.gl, &mut self.gr] {
            s.snap(s.target);
        }
        self.control(max_offset);
        self.offset.0 = self.offset.1;
        self.left.0 = self.left.1;
        self.right.0 = self.right.1;
    }
}

/// iZotope Vocal Doubler style doubler: up to four copies of the (mono)
/// input, each a little late and slightly out of tune, with slowly wandering
/// timing and pitch like a second take. MODE picks the number of voices and
/// how much they wander: TIGHT (two close, steady doubles), LOOSE (two
/// looser, later ones), GROUP (four voices, a small section), DETUNE (two
/// voices held about 11 cents sharp and flat) and THICK (four detuned
/// voices). SPREAD sets how far the doubles are panned apart, MIX balances
/// the original and the doubles (both at full level at 50 %).
struct DoubleVocal {
    sr: f32,
    voices: [DoubleVoice; 4],
    /// Position in the control period.
    phase: usize,
    dry: Smoothed,
    wet: Smoothed,
}

impl DoubleVocal {
    fn new(ctx: FxContext) -> Self {
        let sr = ctx.sample_rate;
        let cr = sr / DOUBLE_CTRL as f32;
        let spec = ShifterSpec {
            max_offset_ms: MAX_DOUBLE_DELAY_MS,
            min_ms: 1.0,
            window_ms: 30.0,
            search_ms: 15.0,
            corr_ms: 10.0,
            fade_ms: 12.0,
        };
        let voice = |k: u32| DoubleVoice {
            shifter: Shifter::new(spec, sr, false),
            timing: Wander::new(0x1357_9BDF ^ k.wrapping_mul(0x2F1F_3C27), 0.25 + 0.07 * k as f32, cr),
            pitch: Wander::new(0x2468_ACE1 ^ k.wrapping_mul(0x7A2B_9D13), 1.1 + 0.3 * k as f32, cr),
            delay: Smoothed::new(0.0, 150.0, cr),
            timing_depth: Smoothed::new(0.0, 150.0, cr),
            detune: Smoothed::new(0.0, 150.0, cr),
            drift: Smoothed::new(0.0, 150.0, cr),
            gl: Smoothed::new(0.0, 60.0, cr),
            gr: Smoothed::new(0.0, 60.0, cr),
            offset: (0.0, 0.0),
            left: (0.0, 0.0),
            right: (0.0, 0.0),
            ratio: 1.0,
        };
        Self {
            sr,
            voices: [voice(0), voice(1), voice(2), voice(3)],
            phase: 0,
            dry: Smoothed::new(1.0, SMOOTH_MS, sr),
            wet: Smoothed::new(1.0, SMOOTH_MS, sr),
        }
    }

    fn max_offset(&self) -> f32 {
        MAX_DOUBLE_DELAY_MS * 0.001 * self.sr
    }
}

impl FxAlgo for DoubleVocal {
    fn set_params(&mut self, p: &Params) {
        let mode = &DOUBLE_MODES[p.choice("mode").min(DOUBLE_MODES.len() - 1)];
        let spread = (p.f("sprd") * 0.01).clamp(0.0, 1.0);
        let gain = 1.0 / (mode.voices as f32).sqrt();
        let ms = self.sr * 0.001;
        for (k, v) in self.voices.iter_mut().enumerate() {
            let on = k < mode.voices;
            v.delay.target = mode.delay[k].max(5.0) * ms;
            v.timing_depth.target = mode.timing * ms;
            v.detune.target = mode.detune[k];
            v.drift.target = mode.drift;
            let (gl, gr) = pan_gains(mode.pan[k] * spread);
            let g = if on { gain } else { 0.0 };
            v.gl.target = gl * g;
            v.gr.target = gr * g;
        }
        let (dry, wet) = add_mix(p.f("mix"));
        self.dry.target = dry;
        self.wet.target = wet;
    }

    fn process(&mut self, left: &mut [f32], right: &mut [f32]) {
        let max_offset = self.max_offset();
        let step = 1.0 / DOUBLE_CTRL as f32;
        for (l, r) in left.iter_mut().zip(right.iter_mut()) {
            if self.phase == 0 {
                for v in &mut self.voices {
                    v.control(max_offset);
                }
            }
            self.phase += 1;
            let f = self.phase as f32 * step;
            if self.phase == DOUBLE_CTRL {
                self.phase = 0;
            }
            let x = 0.5 * (*l + *r);
            let (mut ol, mut or) = (0.0, 0.0);
            for v in &mut self.voices {
                if v.silent() {
                    // Keep the line filled so the voice can fade back in.
                    v.shifter.tick_mono(x, 1.0, v.offset.1);
                    continue;
                }
                let lerp = |(a, b): (f32, f32)| a + (b - a) * f;
                let y = v.shifter.tick_mono(x, v.ratio, lerp(v.offset));
                ol += y * lerp(v.left);
                or += y * lerp(v.right);
            }
            let (dry, wet) = (self.dry.tick(), self.wet.tick());
            *l = *l * dry + ol * wet;
            *r = *r * dry + or * wet;
        }
    }

    fn reset(&mut self) {
        let max_offset = self.max_offset();
        for v in &mut self.voices {
            v.shifter.clear();
            v.timing.reset();
            v.pitch.reset();
            v.snap(max_offset);
        }
        self.phase = 0;
        self.dry.snap(self.dry.target);
        self.wet.snap(self.wet.target);
    }
}

// ---- PCORR: Pitch Fix -----------------------------------------------------------------

/// Pitch class of each note switch (sw_c = C ... sw_b = B); on = the note is disabled.
const NOTE_IDS: [&str; 12] =
    ["sw_c", "sw_db", "sw_d", "sw_eb", "sw_e", "sw_f", "sw_gb", "sw_g", "sw_ab", "sw_a", "sw_bb", "sw_b"];

/// How long a correction is held through an unvoiced gap (ms).
const PCORR_HOLD_MS: f32 = 60.0;

/// Retune time (to ~90 % of the correction) at SPEED 1 and 100, ms.
const PCORR_SLOWEST_MS: f32 = 250.0;
const PCORR_FASTEST_MS: f32 = 1.0;

/// Auto-Tune style real-time pitch correction. A YIN detector follows the
/// (mono) input's pitch; each voiced analysis finds the nearest note of the
/// chromatic scale relative to A4 PITCH (with a little hysteresis, so a pitch
/// sitting between two notes doesn't flip). If that note's key is enabled
/// the input is pulled towards it, by AMOUNT; if it is disabled, input in
/// that note's range stays unaltered (as the console's manual describes; the
/// keys don't snap to a scale). SPEED sets how fast an out-of-tune note
/// reaches its target, from a slow glide that keeps vibrato (1, ~250 ms) to
/// an instant snap (100, the hard-tuned effect). Unvoiced or uncertain input
/// (and silence) glides back to no shift. The shifter runs on both channels
/// with shared splices.
struct PitchFix {
    sr: f32,
    detector: yin::PitchDetector,
    shifter: Shifter,
    allowed: [bool; 12],
    a4: f32,
    amount: f32,
    /// Glide coefficients towards a note and back to no shift.
    retune: f32,
    release: f32,
    /// Correction target and current correction, semitones.
    target: f32,
    shift: f32,
    /// Nearest note at the last voiced analysis (MIDI), and whether the
    /// input is being pulled towards it.
    note: Option<i32>,
    correcting: bool,
    unvoiced: usize,
}

impl PitchFix {
    fn new(ctx: FxContext) -> Self {
        let sr = ctx.sample_rate;
        let spec = ShifterSpec { max_offset_ms: 0.0, min_ms: 1.0, window_ms: 24.0, search_ms: 15.0, corr_ms: 10.0, fade_ms: 6.0 };
        Self {
            sr,
            detector: yin::PitchDetector::new(sr),
            shifter: Shifter::new(spec, sr, true),
            allowed: [true; 12],
            a4: 440.0,
            amount: 1.0,
            retune: 1.0,
            release: coef(30.0, sr),
            target: 0.0,
            shift: 0.0,
            note: None,
            correcting: false,
            unvoiced: usize::MAX / 2,
        }
    }

    /// The note nearest to `midi` (fractional MIDI note), keeping the current
    /// one until the pitch is clearly closer to another.
    fn nearest_note(&self, midi: f32) -> i32 {
        match self.note {
            Some(cur) if (midi - cur as f32).abs() < 0.65 => cur,
            _ => midi.round() as i32,
        }
    }

    fn analysed(&mut self, det: yin::Detection) {
        let hold = (PCORR_HOLD_MS * 0.001 * self.sr) as usize;
        if det.voiced && det.hz > 0.0 {
            self.unvoiced = 0;
            let midi = 69.0 + 12.0 * (det.hz / self.a4).log2();
            let n = self.nearest_note(midi);
            self.note = Some(n);
            self.correcting = self.allowed[n.rem_euclid(12) as usize];
            self.target = if self.correcting { (n as f32 - midi) * self.amount } else { 0.0 };
        } else if self.unvoiced > hold {
            self.note = None;
            self.correcting = false;
            self.target = 0.0;
        }
    }
}

impl FxAlgo for PitchFix {
    fn set_params(&mut self, p: &Params) {
        // SPEED 1..100: retune time from PCORR_SLOWEST_MS down to
        // PCORR_FASTEST_MS (log taper); 2.3 time constants reach 90 %.
        let x = (p.f("spd").clamp(1.0, 100.0) - 1.0) / 99.0;
        let retune_ms = PCORR_SLOWEST_MS * (PCORR_FASTEST_MS / PCORR_SLOWEST_MS).powf(x);
        self.retune = coef(retune_ms / 2.3, self.sr);
        self.amount = (p.f("amnt") * 0.01).clamp(0.0, 1.0);
        self.a4 = p.f("a4").clamp(400.0, 480.0);
        // A key switched on disables its note: the console starts with every
        // switch off and every note corrected (WING Edit's defaults).
        for (a, id) in self.allowed.iter_mut().zip(NOTE_IDS) {
            *a = !p.on(id);
        }
    }

    fn process(&mut self, left: &mut [f32], right: &mut [f32]) {
        for (l, r) in left.iter_mut().zip(right.iter_mut()) {
            if let Some(det) = self.detector.push(0.5 * (*l + *r)) {
                self.analysed(det);
            }
            self.unvoiced = self.unvoiced.saturating_add(1);
            let a = if self.correcting { self.retune } else { self.release };
            self.shift += a * (self.target - self.shift);
            let (yl, yr) = self.shifter.tick_stereo(*l, *r, semis_to_ratio(self.shift), 0.0);
            *l = yl;
            *r = yr;
        }
    }

    fn reset(&mut self) {
        self.detector.reset();
        self.shifter.clear();
        self.target = 0.0;
        self.shift = 0.0;
        self.note = None;
        self.correcting = false;
        self.unvoiced = usize::MAX / 2;
    }
}

#[cfg(test)]
mod tests {
    use super::super::{model_index, FxDevice, FX_MODELS, MAX_FX_PARAMS};
    use super::*;
    use std::f32::consts::TAU;

    #[test]
    fn devices() {
        for token in super::TOKENS {
            crate::wing::fx::tests::check_device(token);
        }
    }

    /// Runs a device on a mono signal (same on both channels) with some
    /// parameters changed from their defaults.
    fn render(token: &str, set: &[(&str, f32)], input: &[f32], sr: f32) -> (Vec<f32>, Vec<f32>) {
        let model = model_index(token).unwrap();
        let spec = &FX_MODELS[model];
        let mut dev = FxDevice::new(model, FxContext { sample_rate: sr, max_block: 256 }).unwrap();
        let mut values = [0.0; MAX_FX_PARAMS];
        spec.defaults_into(&mut values);
        for (id, v) in set {
            values[spec.param_index(id).unwrap()] = *v;
        }
        let mut l = input.to_vec();
        let mut r = input.to_vec();
        dev.process(&values, &mut l, &mut r);
        (l, r)
    }

    fn sine(hz: f32, secs: f32, amp: f32, sr: f32) -> Vec<f32> {
        (0..(secs * sr) as usize).map(|i| amp * (TAU * hz * i as f32 / sr).sin()).collect()
    }

    /// Frequency from interpolated rising zero crossings.
    fn frequency(x: &[f32], sr: f32) -> f32 {
        let mut first = None;
        let mut last = 0.0;
        let mut n = 0;
        for i in 1..x.len() {
            if x[i - 1] < 0.0 && x[i] >= 0.0 {
                let t = (i - 1) as f32 + x[i - 1] / (x[i - 1] - x[i]);
                if first.is_none() {
                    first = Some(t);
                } else {
                    n += 1;
                }
                last = t;
            }
        }
        n as f32 * sr / (last - first.unwrap_or(0.0))
    }

    /// Magnitude of the `hz` component (Goertzel), as a sine amplitude.
    fn level_at(x: &[f32], hz: f32, sr: f32) -> f32 {
        let w = TAU * hz / sr;
        let c = 2.0 * w.cos();
        let (mut s1, mut s2) = (0.0f64, 0.0f64);
        for &v in x {
            let s = v as f64 + c as f64 * s1 - s2;
            s2 = s1;
            s1 = s;
        }
        let p = s1 * s1 + s2 * s2 - c as f64 * s1 * s2;
        (2.0 * p.sqrt() / x.len() as f64) as f32
    }

    #[test]
    fn pitch_octave_up_doubles_frequency() {
        let sr = 48_000.0;
        let (l, r) =
            render("PITCH", &[("semi", 12.0), ("cent", 0.0), ("lc", 20.0), ("hc", 20_000.0)], &sine(300.0, 2.0, 0.5, sr), sr);
        for out in [&l, &r] {
            let hz = frequency(&out[48_000..], sr);
            assert!((hz - 600.0).abs() < 3.0, "{hz}");
            // Steady level: the splices don't cancel.
            let peak = out[48_000..].iter().fold(0.0f32, |a, x| a.max(x.abs()));
            assert!(peak > 0.4 && peak < 0.6, "{peak}");
        }
        let (l, _) = render("PITCH", &[("semi", -7.0), ("cent", 0.0), ("lc", 20.0)], &sine(440.0, 2.0, 0.5, sr), sr);
        let hz = frequency(&l[48_000..], sr);
        assert!((hz - 440.0 * 2f32.powf(-7.0 / 12.0)).abs() < 2.0, "{hz}");
    }

    #[test]
    fn dual_pitch_pans_its_voices() {
        let sr = 48_000.0;
        let set =
            [("semi1", 12.0), ("cent1", 0.0), ("pan1", -100.0), ("semi2", -12.0), ("cent2", 0.0), ("pan2", 100.0), ("lc", 20.0)];
        let (l, r) = render("D-PITCH", &set, &sine(400.0, 1.5, 0.5, sr), sr);
        assert!((frequency(&l[24_000..], sr) - 800.0).abs() < 4.0);
        assert!((frequency(&r[24_000..], sr) - 200.0).abs() < 2.0);
    }

    #[test]
    fn pitch_fix_pulls_a_flat_note_to_pitch() {
        let sr = 48_000.0;
        // 30 cents flat of A4: corrected to 440 Hz.
        let input = sine(440.0 * 2f32.powf(-0.3 / 12.0), 2.0, 0.3, sr);
        let full = [("amnt", 100.0), ("spd", 20.0)];
        let (l, _) = render("PCORR", &full, &input, sr);
        let hz = frequency(&l[48_000..], sr);
        let cents = 1200.0 * (hz / 440.0).log2();
        assert!(cents.abs() < 3.0, "{hz} Hz, {cents} cents");
        // AMOUNT 0 leaves it alone.
        let (l, _) = render("PCORR", &[("amnt", 0.0)], &input, sr);
        let cents = 1200.0 * (frequency(&l[48_000..], sr) / 440.0).log2();
        assert!((cents + 30.0).abs() < 3.0, "{cents}");
        // A disabled key leaves input in that note's range alone ...
        let (l, _) = render("PCORR", &[full[0], full[1], ("sw_a", 1.0)], &input, sr);
        let cents = 1200.0 * (frequency(&l[48_000..], sr) / 440.0).log2();
        assert!((cents + 30.0).abs() < 3.0, "{cents}");
        // ... while the other keys still correct.
        let (l, _) = render("PCORR", &[full[0], full[1], ("sw_g", 1.0)], &input, sr);
        let cents = 1200.0 * (frequency(&l[48_000..], sr) / 440.0).log2();
        assert!(cents.abs() < 3.0, "{cents}");
        // SPEED: higher is faster.
        let early = |spd: f32| {
            let (l, _) = render("PCORR", &[full[0], ("spd", spd)], &input, sr);
            (1200.0 * (frequency(&l[2400..4800], sr) / 440.0).log2()).abs()
        };
        let (fast, slow) = (early(100.0), early(1.0));
        assert!(fast < 5.0 && slow > 12.0, "fast {fast}, slow {slow}");
        // A4 PITCH moves the reference.
        let (l, _) = render("PCORR", &[full[0], full[1], ("a4", 432.0)], &sine(437.0, 2.0, 0.3, sr), sr);
        assert!((frequency(&l[48_000..], sr) - 432.0).abs() < 1.5);
    }

    #[test]
    fn sub_octaver_adds_octaves_below() {
        let sr = 48_000.0;
        let input = sine(220.0, 1.0, 0.5, sr);
        let (l, _) = render("SUB", &[("oct1", 100.0)], &input, sr);
        let tail = &l[24_000..];
        assert!(level_at(tail, 110.0, sr) > 0.15, "{}", level_at(tail, 110.0, sr));
        assert!((level_at(tail, 220.0, sr) - 0.5).abs() < 0.15);
        let (l, _) = render("SUB", &[("oct1", 0.0), ("oct2", 100.0)], &input, sr);
        assert!(level_at(&l[24_000..], 55.0, sr) > 0.1);
        assert!(level_at(&l[24_000..], 110.0, sr) < 0.05);
    }

    #[test]
    fn sub_monster_synthesises_a_sub() {
        let sr = 48_000.0;
        // Band 3 sits on FREQ (55 Hz) and follows content around 110 Hz.
        let (l, _) = render("SUB-M", &[("bd1", 0.0), ("bd3", 100.0)], &sine(110.0, 1.5, 0.5, sr), sr);
        let tail = &l[36_000..];
        assert!(level_at(tail, 55.0, sr) > 0.1, "{}", level_at(tail, 55.0, sr));
        assert!(level_at(tail, 110.0, sr) > 0.4);
        // Band 1 (default 50 %) is two fifths lower: 55 / 2.25 Hz.
        let f1 = 55.0 / 2.25;
        let (l, _) = render("SUB-M", &[], &sine(2.0 * f1, 2.0, 0.5, sr), sr);
        assert!(level_at(&l[48_000..], f1, sr) > 0.05, "{}", level_at(&l[48_000..], f1, sr));
    }

    #[test]
    fn psycho_bass_adds_harmonics() {
        let sr = 48_000.0;
        let input = sine(50.0, 1.5, 0.5, sr);
        let (l, _) = render("P-BASS", &[("int", -6.0), ("bass", -20.0), ("xf", 80.0)], &input, sr);
        let tail = &l[36_000..];
        // The fundamental is mostly removed (BASS GAIN -20 dB) and its
        // harmonics appear above the crossover.
        assert!(level_at(tail, 50.0, sr) < 0.15);
        assert!(level_at(tail, 100.0, sr) + level_at(tail, 150.0, sr) > 0.1);
        assert!(level_at(&input, 100.0, sr) < 1e-3);
        // SOLO passes only the band below the crossover.
        let (l, _) = render("P-BASS", &[("solo", 1.0)], &sine(1000.0, 0.5, 0.5, sr), sr);
        assert!(level_at(&l[12_000..], 1000.0, sr) < 0.01);
        let (l, _) = render("P-BASS", &[("solo", 1.0)], &sine(30.0, 1.0, 0.5, sr), sr);
        assert!(level_at(&l[24_000..], 30.0, sr) > 0.4);
    }

    #[test]
    fn psycho_bass_meters_show_the_low_band_and_the_harmonics() {
        let sr = 48_000.0;
        let model = model_index("P-BASS").unwrap();
        let spec = &FX_MODELS[model];
        let mut dev = FxDevice::new(model, FxContext { sample_rate: sr, max_block: 256 }).unwrap();
        let mut values = [0.0; MAX_FX_PARAMS];
        spec.defaults_into(&mut values);
        values[spec.param_index("int").unwrap()] = 0.0;
        values[spec.param_index("bass").unwrap()] = 0.0;
        let mut read = |input: &[f32], values: &[f32]| {
            let (mut l, mut r) = (input.to_vec(), input.to_vec());
            dev.process(values, &mut l, &mut r);
            let mut ro = [0.0; super::super::FX_READOUTS];
            dev.readouts(&mut ro);
            [ro[0], ro[1]]
        };
        // A 50 Hz tone: the low band reads its level, the harmonics show up.
        let low = read(&sine(50.0, 1.0, 0.5, sr), &values);
        assert!(low[0] > 0.25 && low[0] < 0.55 && low[1] > 0.05, "{low:?}");
        // A 2 kHz tone: nothing below X/O, no harmonics.
        let high = read(&sine(2000.0, 1.0, 0.5, sr), &values);
        assert!(high[0] < 0.02 && high[1] < 0.02, "{high:?}");
        // BASS GAIN -60 dB: the low band's meter drops.
        values[spec.param_index("bass").unwrap()] = -60.0;
        let cut = read(&sine(50.0, 1.0, 0.5, sr), &values);
        assert!(cut[0] < 0.01, "{cut:?}");
    }

    #[test]
    fn sub_octaver_range_and_sub_monster_tune_and_mix() {
        let sr = 48_000.0;
        // RANGE HIGH tracks a high guitar note, LOW (for bass) filters it out of its tracker.
        let input = sine(500.0, 1.5, 0.5, sr);
        let (low, _) = render("SUB", &[("oct1", 100.0), ("rng", 0.0)], &input, sr);
        let (high, _) = render("SUB", &[("oct1", 100.0), ("rng", 2.0)], &input, sr);
        let (a, b) = (level_at(&high[36_000..], 250.0, sr), level_at(&low[36_000..], 250.0, sr));
        assert!(a > 0.05 && a > 4.0 * b, "high {a}, low {b}");
        // TUNE moves band 3: at 67.5 Hz it follows 135 Hz content.
        let (l, _) =
            render("SUB-M", &[("bd1", 0.0), ("bd2", 0.0), ("bd3", 100.0), ("freq", 67.5)], &sine(135.0, 1.5, 0.5, sr), sr);
        assert!(level_at(&l[36_000..], 67.5, sr) > 0.1, "{}", level_at(&l[36_000..], 67.5, sr));
        // The band meters: band 3 tracks, band 5 (fader down) reads nothing.
        let model = model_index("SUB-M").unwrap();
        let spec = &FX_MODELS[model];
        let mut dev = FxDevice::new(model, FxContext { sample_rate: sr, max_block: 256 }).unwrap();
        let mut values = [0.0; MAX_FX_PARAMS];
        spec.defaults_into(&mut values);
        values[spec.param_index("bd3").unwrap()] = 100.0;
        values[spec.param_index("bd5").unwrap()] = 0.0;
        let (mut l, mut r) = (sine(110.0, 1.0, 0.5, sr), sine(110.0, 1.0, 0.5, sr));
        dev.process(&values, &mut l, &mut r);
        let mut ro = [0.0; super::super::FX_READOUTS];
        dev.readouts(&mut ro);
        assert!(ro[2] > 0.05 && ro[4] == 0.0, "{:?}", &ro[..5]);
        // MIX 0: the dry signal alone.
        let x = sine(110.0, 1.0, 0.5, sr);
        let (l, _) = render("SUB-M", &[("mix", 0.0)], &x, sr);
        let (sub, dry) = (level_at(&l[24_000..], 55.0, sr), level_at(&l[24_000..], 110.0, sr));
        assert!(sub < 0.02 && (dry - 0.5).abs() < 0.02, "{sub} {dry}"); // (the 55 Hz bin leaks a little of 110 Hz)
    }

    #[test]
    fn double_vocal_mix_and_modes() {
        let sr = 48_000.0;
        let mut input = vec![0.0; 24_000];
        input[100] = 1.0;
        // MIX 0: only the dry signal.
        let (l, _) = render("DOUBLE", &[("mix", 0.0)], &input, sr);
        let late: f32 = l[580..2500].iter().map(|v| v * v).sum();
        assert!(late < 1e-4, "{late}");
        // Every MODE adds doubles; SPREAD 0 keeps them centred (L = R).
        for mode in 0..5 {
            let (l, r) = render("DOUBLE", &[("mode", mode as f32), ("mix", 100.0), ("sprd", 0.0)], &input, sr);
            let late: f32 = l[580..4000].iter().map(|v| v * v).sum();
            assert!(late > 0.01, "mode {mode}: {late}");
            let diff: f32 = l.iter().zip(&r).map(|(a, b)| (a - b).abs()).sum();
            assert!(diff < 0.05, "mode {mode}: spread 0 differs by {diff}");
        }
    }

    #[test]
    fn double_vocal_adds_late_wide_copies() {
        let sr = 48_000.0;
        let mut input = vec![0.0; 24_000];
        input[100] = 1.0;
        let (l, r) = render("DOUBLE", &[("sprd", 100.0)], &input, sr);
        // Dry impulse at 100 on both sides, the doubles arrive 10-40 ms later,
        // one per side.
        let late = |x: &[f32]| x[100 + 480..100 + 2400].iter().map(|v| v * v).sum::<f32>();
        assert!(late(&l) > 0.05 && late(&r) > 0.05);
        assert!((l[100] - 1.0).abs() < 0.05);
        let diff: f32 = l.iter().zip(&r).map(|(a, b)| (a - b).abs()).sum();
        assert!(diff > 0.1, "doubles should differ between sides");
    }
}
