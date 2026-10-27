//! Guitar and bass amps: rack amp, UK rock, angel, jazz clean and deluxe.
//!
//! One amp engine with a voicing per model. The (mono-summed) input passes an
//! input high-pass and the model's pre-distortion EQ (bright caps, mid boost,
//! the SansAmp's BUZZ/PUNCH/CRUNCH), a chain of tube-like gain stages
//! (asymmetric soft clipping, Miller low-pass and coupling-cap high-pass per
//! stage), the tone section (the circuit-derived Fender/Marshall stack of
//! [`tonestack`], or active EQ), the power amp (softer push-pull saturation
//! whose headroom sags with the output level) and the post EQ (presence,
//! bottom, the SansAmp's LOW/HIGH). The preamp runs 4x and the power section 2x
//! oversampled ([`oversample`]). CAB SIM selects a speaker cabinet response
//! (low resonance, mid character, roll-off above ~5 kHz) instead of a gentle
//! line-out response, and OUT GAIN trims the output, calibrated so the
//! defaults come out at about the input level. The result goes to both
//! channels.

mod oversample;
mod tonestack;

use super::blocks::{db_to_gain, Biquad, BiquadCoeffs, OnePole, Smoothed};
use super::{FxAlgo, FxContext};
use crate::wing::Params;
use oversample::{Oversampler2, Oversampler4};
use tonestack::{ToneStack, FENDER_AB763, HIGH_GAIN, MARSHALL};

/// The devices this module implements.
pub(super) const TOKENS: &[&str] = &["RACKAMP", "UKROCK", "ANGEL", "JAZZC", "DELUXE"];

pub(super) fn create(token: &str, ctx: FxContext) -> Option<Box<dyn FxAlgo>> {
    let model = match token {
        "RACKAMP" => Model::Rack,
        "UKROCK" => Model::UkRock,
        "ANGEL" => Model::Angel,
        "JAZZC" => Model::JazzClean,
        "DELUXE" => Model::Deluxe,
        _ => return None,
    };
    debug_assert!(TOKENS.contains(&token));
    Some(Box::new(Amp::new(model, ctx)))
}

/// Oversampling of the preamp (heavy clipping) and of the power section.
const OS_PRE: f32 = 4.0;
const OS_POWER: f32 = 2.0;
/// Samples between tone-control coefficient updates.
const CTRL: usize = 32;
const SMOOTH_MS: f32 = 20.0;
/// The tone knobs glide a little slower: their filters are redesigned per
/// control step, and small steps keep a turned knob from crackling.
const TONE_SMOOTH_MS: f32 = 60.0;
/// Level of the anti-denormal signal (about -300 dBFS at the output). The
/// engine flushes denormals on the audio thread; this keeps the amps fast
/// where nothing does (tests, offline rendering).
const DENORMAL_GUARD: f32 = 1e-15;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Model {
    /// Tech 21 SansAmp: PREAMP, BUZZ, PUNCH, CRUNCH, DRIVE, LOW, HIGH.
    Rack,
    /// Marshall JCM800: GAIN, BASS, MIDDLE, TREBLE, PRESENCE, MASTER, SAG.
    UkRock,
    /// ENGL high gain: as UK rock plus MID BOOST, BRIGHT, BOTTOM.
    Angel,
    /// Roland JC-120: VOLUME, BASS, MIDDLE, TREBLE, BRIGHT.
    JazzClean,
    /// Fender Deluxe Reverb: VOLUME, BASS, TREBLE, SAG.
    Deluxe,
}

/// A gain stage: gain into it, Miller low-pass (Hz), coupling high-pass
/// (Hz), and the knees of its positive and negative swings (saturating at
/// 1/knee; unequal knees make the stage asymmetric, like a triode). `hard`
/// stages clip like solid-state circuits instead.
#[derive(Debug, Clone, Copy)]
struct Stage {
    gain: f32,
    lp: f32,
    hp: f32,
    pos: f32,
    neg: f32,
    hard: bool,
}

const fn tube(gain: f32, lp: f32, hp: f32, pos: f32, neg: f32) -> Stage {
    Stage { gain, lp, hp, pos, neg, hard: false }
}

/// A cabinet (or line-out) filter section.
#[derive(Debug, Clone, Copy)]
enum Section {
    Hp(f32, f32),
    Lp(f32, f32),
    Peak(f32, f32, f32),
}

/// Closed-back 4x12 (Celestion style): tight low resonance, a little
/// boxiness dip, upper-mid presence peak, steep roll-off above 5 kHz.
const CAB_4X12: &[Section] = &[
    Section::Hp(75.0, 1.1),
    Section::Peak(110.0, 1.3, 3.0),
    Section::Peak(450.0, 1.0, -3.5),
    Section::Peak(2300.0, 1.6, 4.0),
    Section::Lp(4800.0, 0.85),
    Section::Lp(6500.0, 0.6),
];
/// Open-back 2x12 (JC-120): lighter bass, brighter top.
const CAB_2X12_OPEN: &[Section] = &[
    Section::Hp(85.0, 0.75),
    Section::Peak(130.0, 1.0, 2.0),
    Section::Peak(700.0, 1.0, -2.0),
    Section::Peak(2800.0, 1.8, 3.5),
    Section::Lp(5500.0, 0.8),
    Section::Lp(8000.0, 0.6),
];
/// Open-back 1x12 (Deluxe): warm low end, smooth upper mids.
const CAB_1X12_OPEN: &[Section] = &[
    Section::Hp(90.0, 0.8),
    Section::Peak(120.0, 1.2, 2.5),
    Section::Peak(1200.0, 1.0, 1.5),
    Section::Peak(3000.0, 2.0, 3.0),
    Section::Lp(5000.0, 0.8),
    Section::Lp(7000.0, 0.6),
];
/// The SansAmp's speaker emulation (closed-back character).
const CAB_SANSAMP: &[Section] = &[
    Section::Hp(70.0, 0.9),
    Section::Peak(120.0, 1.2, 2.0),
    Section::Peak(500.0, 0.9, -2.5),
    Section::Peak(2000.0, 1.4, 3.0),
    Section::Lp(5000.0, 0.8),
    Section::Lp(7500.0, 0.6),
];
/// Line out (CAB SIM off): just the amp, with the extreme lows and the
/// fizz gently trimmed.
const LINE_OUT: &[Section] = &[Section::Hp(35.0, 0.707), Section::Lp(11_000.0, 0.5)];

/// Everything that makes a model sound like itself.
struct Voicing {
    in_hp: f32,
    /// Preamp drive (dB) over the gain dial, and the stage it feeds.
    pre_db: (f32, f32),
    drive_at: usize,
    stages: &'static [Stage],
    /// Power section drive (dB) over its dial (MASTER/DRIVE/VOLUME).
    power_db: (f32, f32),
    /// Extra stage driven from the power dial (the Fender's V2 after its
    /// VOLUME pot).
    post_stage: Option<Stage>,
    /// Power amp knees and output transformer band (high-pass, low-pass Hz).
    power: Stage,
    /// Headroom lost at SAG 10 and full output.
    sag: f32,
    cab: &'static [Section],
    /// Calibration: defaults give about the input level.
    trim_db: f32,
}

/// Rack amp (Tech 21 SansAmp). As the console's manual describes: BUZZ,
/// PUNCH and CRUNCH control the low-, mid- and high-frequency distortion
/// (each band is pushed +-12 dB into the preamp's two clipping stages and
/// partly pulled back after them, so they change the distortion more than
/// the EQ); PRE AMP (-18..+18 dB) sets the level from the preamp into the
/// power amp; DRIVE (-6..+24 dB into the power stage, level-compensated) sets
/// the power amp distortion; LO EQ and HI EQ are +-12 dB shelves after it;
/// the speaker emulation is closed-back.
const RACK: Voicing = Voicing {
    in_hp: 60.0,
    pre_db: (12.0, 12.0),
    drive_at: 0,
    stages: &[tube(1.0, 12_000.0, 40.0, 1.0, 0.7), tube(2.5, 9000.0, 60.0, 1.0, 0.8)],
    power_db: (-18.0, 18.0),
    post_stage: None,
    power: tube(1.0, 14_000.0, 40.0, 1.0, 0.9),
    sag: 0.3,
    cab: CAB_SANSAMP,
    trim_db: -12.5,
};

/// UK rock amp (Marshall JCM800 2203): a fixed first stage, then GAIN (the
/// pot between V1a and V1b, -10..+36 dB, with its bright cap fading out as it
/// turns up) into two more stages, the Marshall tone stack, MASTER into the
/// EL34 power amp (-24..+12 dB), PRESENCE as the feedback-loop treble shelf
/// (0..+9 dB) and a closed 4x12.
const UKROCK: Voicing = Voicing {
    in_hp: 90.0,
    pre_db: (-10.0, 36.0),
    drive_at: 1,
    stages: &[tube(2.0, 14_000.0, 30.0, 1.0, 0.7), tube(1.5, 10_000.0, 120.0, 1.0, 0.6), tube(2.5, 8000.0, 40.0, 1.0, 0.8)],
    power_db: (-24.0, 12.0),
    post_stage: None,
    power: tube(1.0, 14_000.0, 45.0, 1.0, 0.9),
    sag: 1.5,
    cab: CAB_4X12,
    trim_db: -10.0,
};

/// Angel amp (ENGL high gain): as the UK rock amp with a tighter input, four
/// cascaded stages (GAIN -16..+40 dB), a darker stack with more upper mids;
/// MID BOOST adds +8 dB at 800 Hz and BRIGHT +6 dB above 2.5 kHz before the
/// distortion, BOTTOM +6 dB below 90 Hz after the power amp.
const ANGEL: Voicing = Voicing {
    in_hp: 80.0,
    pre_db: (-16.0, 40.0),
    drive_at: 1,
    stages: &[
        tube(2.0, 14_000.0, 60.0, 1.0, 0.7),
        tube(2.0, 10_000.0, 100.0, 1.0, 0.6),
        tube(3.0, 8000.0, 60.0, 1.0, 0.75),
        tube(2.5, 7000.0, 60.0, 1.0, 0.85),
    ],
    power_db: (-24.0, 12.0),
    post_stage: None,
    power: tube(1.0, 14_000.0, 45.0, 1.0, 0.9),
    sag: 1.2,
    cab: CAB_4X12,
    trim_db: -10.7,
};

/// Jazz clean amp (Roland JC-120): a solid-state preamp that stays clean
/// until VOLUME (-16..+16 dB) is well up, active BASS/MIDDLE/TREBLE (+-12,
/// +-10, +-12 dB, flat at 5), BRIGHT (+6 dB above 3 kHz), no sag, and an
/// open-back 2x12.
const JAZZC: Voicing = Voicing {
    in_hp: 50.0,
    pre_db: (-16.0, 16.0),
    drive_at: 0,
    stages: &[Stage { gain: 1.5, lp: 18_000.0, hp: 20.0, pos: 1.0, neg: 1.0, hard: true }],
    power_db: (0.0, 0.0),
    post_stage: None,
    power: Stage { gain: 0.8, lp: 18_000.0, hp: 30.0, pos: 1.0, neg: 1.0, hard: true },
    sag: 0.0,
    cab: CAB_2X12_OPEN,
    trim_db: -1.1,
};

/// Deluxe amp (Fender Deluxe Reverb, AB763): a first stage, the Fender tone
/// stack (BASS, TREBLE; the mid pot is a fixed resistor), then VOLUME
/// (-20..+12 dB) into the second stage and the 6V6 power amp, which breaks up
/// and sags (SAG) past about half way; open-back 1x12. Dials run 1..10.
const DELUXE: Voicing = Voicing {
    in_hp: 40.0,
    pre_db: (0.0, 0.0),
    drive_at: 0,
    stages: &[tube(2.5, 15_000.0, 20.0, 1.0, 0.75)],
    power_db: (-20.0, 12.0),
    post_stage: Some(tube(3.0, 10_000.0, 30.0, 1.0, 0.7)),
    power: tube(1.5, 12_000.0, 50.0, 1.0, 0.85),
    sag: 3.0,
    cab: CAB_1X12_OPEN,
    trim_db: 1.9,
};

impl Model {
    fn voicing(self) -> &'static Voicing {
        match self {
            Model::Rack => &RACK,
            Model::UkRock => &UKROCK,
            Model::Angel => &ANGEL,
            Model::JazzClean => &JAZZC,
            Model::Deluxe => &DELUXE,
        }
    }
}

/// Coefficient of a one-pole filter with cutoff `hz`.
fn one_pole(hz: f32, sr: f32) -> f32 {
    1.0 - (-std::f32::consts::TAU * hz.clamp(1.0, sr * 0.49) / sr).exp()
}

/// Padé tanh (as `blocks::fast_tanh`), inlined into the oversampled loops.
#[inline(always)]
fn tanh_pade(x: f32) -> f32 {
    let x = x.clamp(-3.0, 3.0);
    let x2 = x * x;
    x * (27.0 + x2) / (27.0 + 9.0 * x2)
}

/// One running gain stage (at the oversampled rate): Miller low-pass,
/// clipper, coupling-cap high-pass.
#[derive(Clone, Copy, Default)]
struct GainStage {
    lp_a: f32,
    lp_z: f32,
    hp_a: f32,
    hp_z: f32,
    /// Knees of the positive and negative swings and their reciprocals.
    knee: [f32; 2],
    inv_knee: [f32; 2],
    hard: bool,
}

impl GainStage {
    fn new(s: &Stage, sr: f32) -> Self {
        let knee = [s.pos.max(0.05), s.neg.max(0.05)];
        Self {
            lp_a: one_pole(s.lp, sr),
            lp_z: 0.0,
            hp_a: one_pole(s.hp, sr),
            hp_z: 0.0,
            knee,
            inv_knee: [1.0 / knee[0], 1.0 / knee[1]],
            hard: s.hard,
        }
    }

    fn reset(&mut self) {
        self.lp_z = 0.0;
        self.hp_z = 0.0;
    }

    /// The clipper. Tube stages: slope 1 at 0, saturating at 1/knee on each
    /// side (unequal knees make them asymmetric, like a triode); solid-state
    /// stages: clean almost to the rail, then a firm knee.
    #[inline(always)]
    fn clip(&self, v: f32) -> f32 {
        if self.hard {
            let v = v.clamp(-1.5, 1.5);
            v - v * v * v * (4.0 / 27.0)
        } else {
            let side = (v < 0.0) as usize;
            tanh_pade(v * self.knee[side]) * self.inv_knee[side]
        }
    }

    /// Low-pass, clip (scaled to `headroom`), high-pass.
    #[inline(always)]
    fn run(&mut self, v: f32, headroom: f32, inv_headroom: f32) -> f32 {
        self.lp_z += self.lp_a * (v - self.lp_z);
        let y = self.clip(self.lp_z * inv_headroom) * headroom;
        self.hp_z += self.hp_a * (y - self.hp_z);
        y - self.hp_z
    }

    /// `x * gain` through the stage.
    #[inline(always)]
    fn tick(&mut self, x: f32, gain: f32) -> f32 {
        self.run(x * gain, 1.0, 1.0)
    }

    /// As `tick`, with the clipping scaled to a (sagging) `headroom`.
    #[inline(always)]
    fn tick_sag(&mut self, x: f32, gain: f32, headroom: f32, inv_headroom: f32) -> f32 {
        self.run(x * gain, headroom, inv_headroom)
    }
}

/// A fixed chain of biquads (cabinet or line out).
#[derive(Clone, Default)]
struct Chain {
    f: [Biquad; 6],
    n: usize,
}

impl Chain {
    fn new(sections: &[Section], sr: f32) -> Self {
        let sr = sr as f64;
        let mut c = Self::default();
        for (f, s) in c.f.iter_mut().zip(sections) {
            f.set_coefficients(match *s {
                Section::Hp(hz, q) => BiquadCoeffs::high_pass(sr, hz as f64, q as f64),
                Section::Lp(hz, q) => BiquadCoeffs::low_pass(sr, (hz as f64).min(sr * 0.45), q as f64),
                Section::Peak(hz, q, db) => BiquadCoeffs::peak(sr, hz as f64, q as f64, db as f64),
            });
        }
        c.n = sections.len().min(c.f.len());
        c
    }

    fn reset(&mut self) {
        for f in &mut self.f {
            f.reset();
        }
    }

    fn process(&mut self, buf: &mut [f32]) {
        for f in &mut self.f[..self.n] {
            f.process(buf, 0);
        }
    }
}

// Tone knob slots (meaning per model, see `Amp::design`; the rack amp uses
// BASS/MID/TREB for BUZZ/PUNCH/CRUNCH and PRES/BRIGHT for LO EQ/HI EQ).
const K_BASS: usize = 0;
const K_MID: usize = 1;
const K_TREB: usize = 2;
const K_PRES: usize = 3;
const K_BRIGHT: usize = 4;
const K_MIDB: usize = 5;
const K_BOTTOM: usize = 6;
const KNOBS: usize = 7;

/// An audio-taper (logarithmic) pot: resistance fraction at rotation `x`.
#[inline]
fn audio_taper(x: f32) -> f32 {
    let x = x.clamp(0.0, 1.0);
    (10f32.powf(2.0 * x) - 1.0) / 99.0
}

/// A 0..10 dial as 0..1.
#[inline]
fn dial(v: f32) -> f32 {
    (v * 0.1).clamp(0.0, 1.0)
}

/// A 1..10 dial (the Deluxe's) as 0..1.
#[inline]
fn dial1(v: f32) -> f32 {
    ((v - 1.0) / 9.0).clamp(0.0, 1.0)
}

#[inline]
fn lerp(range: (f32, f32), x: f32) -> f32 {
    range.0 + (range.1 - range.0) * x
}

/// A guitar amp (any model).
struct Amp {
    model: Model,
    v: &'static Voicing,
    sr: f32,
    primed: bool,
    in_hp: OnePole,
    pre_eq: [Biquad; 3],
    n_pre: usize,
    os_pre: Oversampler4,
    stages: [GainStage; 4],
    n_stages: usize,
    stack: Option<ToneStack>,
    tone_eq: [Biquad; 3],
    n_tone: usize,
    os_power: Oversampler2,
    post_stage: Option<GainStage>,
    power: GainStage,
    /// Supply sag: envelope of the output stage's drive, and its attack and
    /// release coefficients.
    sag_env: f32,
    sag_att: f32,
    sag_rel: f32,
    post_eq: [Biquad; 2],
    n_post: usize,
    cab: Chain,
    line: Chain,
    pre_drive: Smoothed,
    power_drive: Smoothed,
    sag: Smoothed,
    out: Smoothed,
    cab_mix: Smoothed,
    /// Tone knobs, smoothed at the control rate.
    knobs: [Smoothed; KNOBS],
    ctrl: usize,
    /// Inaudible alternating offset that keeps the filter states out of the
    /// (slow) denormal range during silence.
    guard: f32,
}

impl Amp {
    fn new(model: Model, ctx: FxContext) -> Self {
        let sr = ctx.sample_rate;
        let (pre_sr, power_sr) = (sr * OS_PRE, sr * OS_POWER);
        let v = model.voicing();
        let mut stages = [GainStage::default(); 4];
        for (g, s) in stages.iter_mut().zip(v.stages) {
            *g = GainStage::new(s, pre_sr);
        }
        let (n_pre, n_tone, n_post) = match model {
            Model::Rack => (3, 3, 2),
            Model::UkRock => (1, 0, 1),
            Model::Angel => (2, 0, 2),
            Model::JazzClean => (1, 3, 0),
            Model::Deluxe => (0, 0, 0),
        };
        let has_stack = matches!(model, Model::UkRock | Model::Angel | Model::Deluxe);
        let cr = sr / CTRL as f32;
        let mut amp = Self {
            model,
            v,
            sr,
            primed: false,
            in_hp: OnePole::new(v.in_hp, sr),
            pre_eq: Default::default(),
            n_pre,
            os_pre: Oversampler4::default(),
            stages,
            n_stages: v.stages.len().min(4),
            stack: has_stack.then(ToneStack::default),
            tone_eq: Default::default(),
            n_tone,
            os_power: Oversampler2::default(),
            post_stage: v.post_stage.map(|s| GainStage::new(&s, power_sr)),
            power: GainStage::new(&v.power, power_sr),
            sag_env: 0.0,
            sag_att: super::blocks::coef(8.0, sr),
            sag_rel: super::blocks::coef(150.0, sr),
            post_eq: Default::default(),
            n_post,
            cab: Chain::new(v.cab, sr),
            line: Chain::new(LINE_OUT, sr),
            pre_drive: Smoothed::new(1.0, SMOOTH_MS, sr),
            power_drive: Smoothed::new(1.0, SMOOTH_MS, sr),
            sag: Smoothed::new(0.0, SMOOTH_MS, sr),
            out: Smoothed::new(1.0, SMOOTH_MS, sr),
            cab_mix: Smoothed::new(1.0, SMOOTH_MS, sr),
            knobs: [Smoothed::new(0.5, TONE_SMOOTH_MS, cr); KNOBS],
            ctrl: 0,
            guard: DENORMAL_GUARD,
        };
        amp.design();
        amp
    }

    /// Recomputes the tone controls' filters from the smoothed knobs.
    fn design(&mut self) {
        let k: [f32; KNOBS] = std::array::from_fn(|i| self.knobs[i].value);
        let sr = self.sr as f64;
        let hs = |hz: f64, db: f32| BiquadCoeffs::high_shelf(sr, hz, 0.707, db as f64);
        let ls = |hz: f64, db: f32| BiquadCoeffs::low_shelf(sr, hz, 0.707, db as f64);
        let pk = |hz: f64, q: f64, db: f32| BiquadCoeffs::peak(sr, hz, q, db as f64);
        match self.model {
            Model::Rack => {
                // BUZZ, PUNCH, CRUNCH into the preamp, 30 % undone after it;
                // LO EQ and HIGH EQ after the power section.
                let band = [(k[K_BASS] - 0.5) * 24.0, (k[K_MID] - 0.5) * 24.0, (k[K_TREB] - 0.5) * 24.0];
                self.pre_eq[0].set_coefficients(ls(150.0, band[0]));
                self.pre_eq[1].set_coefficients(pk(700.0, 0.8, band[1]));
                self.pre_eq[2].set_coefficients(hs(2500.0, band[2]));
                self.tone_eq[0].set_coefficients(ls(150.0, -0.3 * band[0]));
                self.tone_eq[1].set_coefficients(pk(700.0, 0.8, -0.3 * band[1]));
                self.tone_eq[2].set_coefficients(hs(2500.0, -0.3 * band[2]));
                self.post_eq[0].set_coefficients(ls(100.0, (k[K_PRES] - 0.5) * 24.0));
                self.post_eq[1].set_coefficients(hs(3300.0, (k[K_BRIGHT] - 0.5) * 24.0));
            }
            Model::UkRock | Model::Angel => {
                let parts = if self.model == Model::Angel { &HIGH_GAIN } else { &MARSHALL };
                if let Some(s) = &mut self.stack {
                    s.design(parts, k[K_TREB] as f64, k[K_MID] as f64, audio_taper(k[K_BASS]) as f64, sr);
                }
                // Presence: the power amp's feedback shelf, boost only.
                self.post_eq[0].set_coefficients(hs(3200.0, 9.0 * k[K_PRES]));
                if self.model == Model::UkRock {
                    // The bright cap across the gain pot: most audible at low gain.
                    self.pre_eq[0].set_coefficients(hs(2000.0, 5.0 * k[K_BRIGHT]));
                } else {
                    self.pre_eq[0].set_coefficients(hs(2500.0, 6.0 * k[K_BRIGHT]));
                    self.pre_eq[1].set_coefficients(pk(800.0, 0.8, 8.0 * k[K_MIDB]));
                    self.post_eq[1].set_coefficients(ls(90.0, 6.0 * k[K_BOTTOM]));
                }
            }
            Model::JazzClean => {
                self.pre_eq[0].set_coefficients(hs(3000.0, 6.0 * k[K_BRIGHT]));
                self.tone_eq[0].set_coefficients(ls(100.0, (k[K_BASS] - 0.5) * 24.0));
                self.tone_eq[1].set_coefficients(pk(600.0, 0.7, (k[K_MID] - 0.5) * 20.0));
                self.tone_eq[2].set_coefficients(hs(3000.0, (k[K_TREB] - 0.5) * 24.0));
            }
            Model::Deluxe => {
                if let Some(s) = &mut self.stack {
                    s.design(&FENDER_AB763, k[K_TREB] as f64, 1.0, audio_taper(k[K_BASS]) as f64, sr);
                }
            }
        }
    }

    /// Advances the tone knobs one control step; redesigns if they moved.
    fn control(&mut self) {
        let mut moving = false;
        for k in &mut self.knobs {
            if k.value != k.target {
                moving = true;
                k.tick();
                if (k.value - k.target).abs() < 1e-4 {
                    k.value = k.target;
                }
            }
        }
        if moving {
            self.design();
        }
    }

    /// Processes one chunk (at most [`CTRL`] samples, mono, in place), one
    /// stage at a time over the whole chunk.
    fn render(&mut self, buf: &mut [f32]) {
        let v = self.v;
        let n = buf.len().min(CTRL);
        let buf = &mut buf[..n];
        // Input and pre-distortion EQ.
        for x in buf.iter_mut() {
            self.guard = -self.guard;
            *x = self.in_hp.hp(*x + self.guard);
        }
        for f in &mut self.pre_eq[..self.n_pre] {
            f.process(buf, 0);
        }

        // Preamp, 4x oversampled. The gain pot sits before stage `drive_at`.
        let mut drive = [0.0f32; CTRL];
        for d in &mut drive[..n] {
            *d = self.pre_drive.tick();
        }
        let mut os = [0.0f32; CTRL * 4];
        let os = &mut os[..4 * n];
        for (o, &x) in os.chunks_exact_mut(4).zip(buf.iter()) {
            o.copy_from_slice(&self.os_pre.up(x));
        }
        for (k, st) in self.stages[..self.n_stages].iter_mut().enumerate() {
            let g = v.stages[k].gain;
            if k == v.drive_at {
                for (o, d) in os.chunks_exact_mut(4).zip(&drive) {
                    let g = g * d;
                    for x in o {
                        *x = st.tick(*x, g);
                    }
                }
            } else {
                for x in os.iter_mut() {
                    *x = st.tick(*x, g);
                }
            }
        }
        for (x, o) in buf.iter_mut().zip(os.chunks_exact(4)) {
            *x = self.os_pre.down([o[0], o[1], o[2], o[3]]);
        }

        // Tone section.
        if let Some(s) = &mut self.stack {
            s.process(buf);
        }
        for f in &mut self.tone_eq[..self.n_tone] {
            f.process(buf, 0);
        }

        // Power section, 2x oversampled. The supply sags with the current
        // drawn, followed from the drive into the output stage.
        let into_power = v.power.gain * st_gain(v);
        let mut sag = [(1.0f32, 1.0f32); CTRL];
        for (x, h) in buf.iter_mut().zip(&mut sag[..n]) {
            *x *= self.power_drive.tick();
            let level = (x.abs() * into_power).min(2.0);
            let a = if level > self.sag_env { self.sag_att } else { self.sag_rel };
            self.sag_env += a * (level - self.sag_env);
            let inv = 1.0 + v.sag * self.sag.tick() * self.sag_env;
            *h = (1.0 / inv, inv);
        }
        let mut os2 = [0.0f32; CTRL * 2];
        let os2 = &mut os2[..2 * n];
        for (o, &x) in os2.chunks_exact_mut(2).zip(buf.iter()) {
            o.copy_from_slice(&self.os_power.up(x));
        }
        if let Some(st) = &mut self.post_stage {
            let g = st_gain(v);
            for x in os2.iter_mut() {
                *x = st.tick(*x, g);
            }
        }
        let g = v.power.gain;
        for (o, &(h, inv)) in os2.chunks_exact_mut(2).zip(&sag) {
            for x in o {
                *x = self.power.tick_sag(*x, g, h, inv);
            }
        }
        for (x, o) in buf.iter_mut().zip(os2.chunks_exact(2)) {
            *x = self.os_power.down([o[0], o[1]]);
        }

        // Post EQ, cabinet / line out, output level.
        for f in &mut self.post_eq[..self.n_post] {
            f.process(buf, 0);
        }
        if self.cab_mix.value == self.cab_mix.target && (self.cab_mix.value == 0.0 || self.cab_mix.value == 1.0) {
            if self.cab_mix.value == 1.0 {
                self.cab.process(buf);
            } else {
                self.line.process(buf);
            }
        } else {
            let mut line = [0.0f32; CTRL];
            let line = &mut line[..n];
            line.copy_from_slice(buf);
            self.cab.process(buf);
            self.line.process(line);
            for (x, l) in buf.iter_mut().zip(line.iter()) {
                let c = self.cab_mix.tick();
                *x = c * *x + (1.0 - c) * l;
            }
        }
        for x in buf.iter_mut() {
            *x *= self.out.tick();
        }
    }

    fn snap(&mut self) {
        for s in [&mut self.pre_drive, &mut self.power_drive, &mut self.sag, &mut self.out, &mut self.cab_mix] {
            s.snap(s.target);
        }
        for k in &mut self.knobs {
            k.snap(k.target);
        }
        self.design();
    }
}

/// Gain into the post stage (models that have one).
#[inline]
fn st_gain(v: &Voicing) -> f32 {
    v.post_stage.map_or(1.0, |s| s.gain)
}

impl FxAlgo for Amp {
    fn set_params(&mut self, p: &Params) {
        let v = self.v;
        let kn = &mut self.knobs;
        let (pre, power, sag, out);
        // Extra power-stage drive and output make-up (the rack amp's DRIVE).
        let (mut extra_db, mut makeup_db) = (0.0, 0.0);
        match self.model {
            Model::Rack => {
                pre = 0.0;
                power = dial(p.f("pre"));
                let drive_db = lerp((-6.0, 24.0), dial(p.f("drive")));
                extra_db = drive_db;
                makeup_db = -0.6 * drive_db;
                sag = 1.0;
                out = p.f("out");
                kn[K_BASS].target = dial(p.f("buzz"));
                kn[K_MID].target = dial(p.f("punch"));
                kn[K_TREB].target = dial(p.f("crunch"));
                kn[K_PRES].target = dial(p.f("leq"));
                kn[K_BRIGHT].target = dial(p.f("heq"));
            }
            Model::UkRock | Model::Angel => {
                pre = dial(p.f("gain"));
                power = dial(p.f("mstr"));
                sag = dial(p.f("sag"));
                out = p.f("out");
                kn[K_BASS].target = dial(p.f("bass"));
                kn[K_MID].target = dial(p.f("mid"));
                kn[K_TREB].target = dial(p.f("treb"));
                kn[K_PRES].target = dial(p.f("pres"));
                if self.model == Model::UkRock {
                    kn[K_BRIGHT].target = (1.0 - pre) * (1.0 - pre);
                } else {
                    kn[K_BRIGHT].target = if p.on("bri") { 1.0 } else { 0.0 };
                    kn[K_MIDB].target = if p.on("midb") { 1.0 } else { 0.0 };
                    kn[K_BOTTOM].target = if p.on("bot") { 1.0 } else { 0.0 };
                }
            }
            Model::JazzClean => {
                pre = dial(p.f("vol"));
                power = 0.0;
                sag = 0.0;
                out = p.f("out");
                kn[K_BASS].target = dial(p.f("bass"));
                kn[K_MID].target = dial(p.f("mid"));
                kn[K_TREB].target = dial(p.f("treb"));
                kn[K_BRIGHT].target = if p.on("bri") { 1.0 } else { 0.0 };
            }
            Model::Deluxe => {
                pre = 0.0;
                power = dial1(p.f("vol"));
                sag = dial1(p.f("sag"));
                out = p.f("out");
                kn[K_BASS].target = dial1(p.f("bass"));
                kn[K_TREB].target = dial1(p.f("treb"));
            }
        }
        self.pre_drive.target = db_to_gain(lerp(v.pre_db, pre));
        self.power_drive.target = db_to_gain(lerp(v.power_db, power) + extra_db);
        self.sag.target = sag;
        // OUT GAIN: +-12 dB around the calibrated level at 5.
        self.out.target = db_to_gain(v.trim_db + makeup_db + (out - 5.0) * 2.4);
        let cab = if p.on("cab") { 1.0 } else { 0.0 };
        if cab != self.cab_mix.target {
            // The chain fading in starts from rest.
            if cab > 0.5 {
                self.cab.reset();
            } else {
                self.line.reset();
            }
        }
        self.cab_mix.target = cab;
        if !self.primed {
            self.primed = true;
            self.snap();
        }
    }

    fn process(&mut self, left: &mut [f32], right: &mut [f32]) {
        let n = left.len().min(right.len());
        let mut start = 0;
        while start < n {
            if self.ctrl == 0 {
                self.ctrl = CTRL;
                self.control();
            }
            let len = self.ctrl.min(n - start);
            let mut buf = [0.0f32; CTRL];
            let buf = &mut buf[..len];
            for (b, (l, r)) in buf.iter_mut().zip(left[start..].iter().zip(&right[start..])) {
                *b = 0.5 * (l + r);
            }
            self.render(buf);
            left[start..start + len].copy_from_slice(buf);
            right[start..start + len].copy_from_slice(buf);
            self.ctrl -= len;
            start += len;
        }
    }

    fn reset(&mut self) {
        self.in_hp.reset();
        for f in self.pre_eq.iter_mut().chain(&mut self.tone_eq).chain(&mut self.post_eq) {
            f.reset();
        }
        self.os_pre.reset();
        self.os_power.reset();
        for s in &mut self.stages {
            s.reset();
        }
        if let Some(s) = &mut self.stack {
            s.reset();
        }
        if let Some(s) = &mut self.post_stage {
            s.reset();
        }
        self.power.reset();
        self.sag_env = 0.0;
        self.cab.reset();
        self.line.reset();
        self.ctrl = 0;
        self.snap();
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

    fn render(token: &str, set: &[(&str, f32)], input: &[f32]) -> Vec<f32> {
        let model = model_index(token).unwrap();
        let spec = &FX_MODELS[model];
        let mut dev = FxDevice::new(model, FxContext { sample_rate: 48_000.0, max_block: 256 }).unwrap();
        let mut values = [0.0; MAX_FX_PARAMS];
        spec.defaults_into(&mut values);
        for (id, v) in set {
            values[spec.param_index(id).unwrap()] = *v;
        }
        let mut l = input.to_vec();
        let mut r = input.to_vec();
        dev.process(&values, &mut l, &mut r);
        l
    }

    fn sine(hz: f32, secs: f32, amp: f32) -> Vec<f32> {
        (0..(secs * 48_000.0) as usize).map(|i| amp * (TAU * hz * i as f32 / 48_000.0).sin()).collect()
    }

    /// Amplitude of the `hz` component (Goertzel).
    fn level_at(x: &[f32], hz: f32) -> f32 {
        let c = 2.0 * (TAU * hz / 48_000.0).cos() as f64;
        let (mut s1, mut s2) = (0.0f64, 0.0f64);
        for &v in x {
            let s = v as f64 + c * s1 - s2;
            s2 = s1;
            s1 = s;
        }
        (2.0 * (s1 * s1 + s2 * s2 - c * s1 * s2).sqrt() / x.len() as f64) as f32
    }

    /// Harmonics 2..8 relative to the fundamental.
    fn distortion(x: &[f32], f0: f32) -> f32 {
        let h: f32 = (2..=8).map(|k| level_at(x, f0 * k as f32).powi(2)).sum();
        h.sqrt() / level_at(x, f0).max(1e-9)
    }

    fn rms(x: &[f32]) -> f32 {
        (x.iter().map(|v| v * v).sum::<f32>() / x.len() as f32).sqrt()
    }

    #[test]
    fn gain_raises_harmonics() {
        let input = sine(220.0, 0.5, 0.2);
        let cases: [(&str, &[&str]); 5] = [
            ("UKROCK", &["gain"]),
            ("ANGEL", &["gain"]),
            ("RACKAMP", &["buzz", "punch", "crunch", "drive"]),
            ("JAZZC", &["vol"]),
            ("DELUXE", &["vol"]),
        ];
        for (token, gains) in cases {
            let with = |v: f32| {
                let mut set: Vec<(&str, f32)> = gains.iter().map(|g| (*g, v)).collect();
                set.push(("cab", 0.0));
                render(token, &set, &input)
            };
            let (lo, hi) = (with(1.0), with(10.0));
            let (dl, dh) = (distortion(&lo[12_000..], 220.0), distortion(&hi[12_000..], 220.0));
            assert!(dh > dl * 2.0 && dh > 0.1, "{token}: {dl} -> {dh}");
        }
        // The rack amp's band controls drive their own band's distortion:
        // BUZZ distorts a low note more than CRUNCH does, and the other way
        // round for a high one.
        let dist = |hz: f32, knob: usize| {
            let mut set = [("buzz", 0.0), ("punch", 0.0), ("crunch", 0.0), ("drive", 0.0), ("cab", 0.0)];
            set[knob].1 = 10.0;
            let out = render("RACKAMP", &set, &sine(hz, 0.5, 0.2));
            distortion(&out[12_000..], hz)
        };
        let (low_buzz, low_crunch) = (dist(110.0, 0), dist(110.0, 2));
        let (high_buzz, high_crunch) = (dist(2500.0, 0), dist(2500.0, 2));
        assert!(low_buzz > 1.5 * low_crunch, "low note: {low_buzz} vs {low_crunch}");
        assert!(high_crunch > 1.5 * high_buzz, "high note: {high_crunch} vs {high_buzz}");
    }

    #[test]
    fn defaults_come_out_near_the_input_level() {
        // A plucked, decaying tone with harmonics, -12 dBFS peak.
        let input: Vec<f32> = (0..48_000)
            .map(|i| {
                let t = (i % 24_000) as f32 / 48_000.0;
                let s = (1..6).map(|k| (TAU * 110.0 * k as f32 * t).sin() / k as f32).sum::<f32>();
                0.25 * s * (-4.0 * t).exp() / 1.5
            })
            .collect();
        for token in TOKENS {
            let out = render(token, &[], &input);
            let db = 20.0 * (rms(&out) / rms(&input)).log10();
            assert!(db.abs() < 4.0, "{token}: {db:.1} dB");
        }
    }

    #[test]
    fn tone_controls_act_on_their_bands() {
        let lows = sine(80.0, 0.5, 0.01);
        let highs = sine(4000.0, 0.5, 0.01);
        for (token, bass, treble) in [
            ("UKROCK", "bass", "treb"),
            ("ANGEL", "bass", "treb"),
            ("JAZZC", "bass", "treb"),
            ("DELUXE", "bass", "treb"),
            ("RACKAMP", "leq", "heq"),
        ] {
            let (lo_min, lo_max) = if token == "DELUXE" { (1.0, 10.0) } else { (0.0, 10.0) };
            let quiet = [("cab", 0.0)];
            let b0 = rms(&render(token, &[quiet[0], (bass, lo_min)], &lows)[12_000..]);
            let b1 = rms(&render(token, &[quiet[0], (bass, lo_max)], &lows)[12_000..]);
            let t0 = rms(&render(token, &[quiet[0], (treble, lo_min)], &highs)[12_000..]);
            let t1 = rms(&render(token, &[quiet[0], (treble, lo_max)], &highs)[12_000..]);
            assert!(b1 > b0 * 1.6, "{token} bass: {b0} {b1}");
            assert!(t1 > t0 * 1.6, "{token} treble: {t0} {t1}");
        }
    }

    #[test]
    fn cab_sim_rolls_off_the_top() {
        let highs = sine(9000.0, 0.3, 0.01);
        for token in TOKENS {
            let on = rms(&render(token, &[("cab", 1.0)], &highs)[6000..]);
            let off = rms(&render(token, &[("cab", 0.0)], &highs)[6000..]);
            assert!(on < off * 0.3, "{token}: {on} vs {off}");
        }
    }
}
