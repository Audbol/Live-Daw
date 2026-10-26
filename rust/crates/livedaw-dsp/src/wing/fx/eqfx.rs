//! The analog EQ models as FX devices and the channel strips (gate + EQ +
//! dynamics in one slot, and the mastering chain).
//!
//! Nothing here has DSP of its own: every block runs one of the console's
//! existing processors, fed a value array built from the device's parameters
//! by id (the block's prefix stripped, a few names translated):
//!
//! * EQ blocks run a channel EQ model through [`WingEq`]. The EQ devices (SOUL
//!   = Soul Analogue/SSL, E88 = Even 88 Formant/Neve 1088, E84 = Even 84/Neve
//!   1084, F110 = Fortissimo 110/Focusrite ISA 110, PULSAR = Pultec EQP-1A +
//!   MEQ-5, MACH4 = mäag EQ4) are one such block; their MIX scales the band
//!   gains as the channel EQ's MIX does (0..125 %).
//! * Dynamics blocks run a gate/dynamics model through [`WingDynamics`].
//! * The mastering chain also uses the standard TAPE, ENHANCE and LIMITER
//!   devices.
//!
//! The strips (WING manual 11.3), FX rack inserts, in signal order:
//!
//! * *EVEN* EVEN Channel: Even 88 Gate (g_*, G-ON), Even 88-Formant EQ (EQ-ON,
//!   EQMIX) and Even Comp/Lim (d_*: the ECL33 model, whose LIM ON and CMP ON
//!   switch its two stages; GAIN is its makeup).
//! * *SOUL* SOUL Channel: SOUL 9000 Gate/Expander (g_*, G-ON), SOUL Analog EQ
//!   (EQ-ON, EQMIX), SOUL 9000 compressor (d_*, D-ON).
//! * *VINTAGE* Vintage Channel: 76 Limiter Amp (d_*, D-ON), Pulsar EQ (its
//!   own EQ1/EQ5 switches), LA Leveler (l_*, LVL-ON).
//! * *BUS* Bus Channel: SOUL Warmth (w_*, W-ON), Even 84 EQ (EQ-ON, EQMIX),
//!   SOUL Bus Comp (d_*, D-ON; GAIN is its makeup).
//! * *MASTER* Mastering: Tape Machine (t_*, T-ON), Mach EQ4 (EQ-ON), Ultra
//!   Enhancer (e_*, ENH-ON), Precision Limiter (l_*, LIM-ON).
//!
//! A block whose ON switch is off passes the signal through; switching
//! crossfades over 10 ms (EQ blocks fade their gains instead). `meter()` shows
//! the deepest gain reduction of the running dynamics blocks.

use super::{FxAlgo, FxContext};
use crate::wing::dynamics::{SidechainSettings, WingDynamics, XoverSettings};
use crate::wing::eq::WingEq;
use crate::wing::{model_index, ModelSpec, Params, DYN_MODELS, EQ_MODELS_CH, FX_MODELS, MAX_FX_PARAMS};

/// The devices this module implements.
pub(super) const TOKENS: &[&str] =
    &["SOUL", "E88", "E84", "F110", "PULSAR", "MACH4", "*EVEN*", "*SOUL*", "*VINTAGE*", "*BUS*", "*MASTER*"];

pub(super) fn create(token: &str, ctx: FxContext) -> Option<Box<dyn FxAlgo>> {
    if !TOKENS.contains(&token) {
        return None;
    }
    let device = &FX_MODELS[super::model_index(token)?];
    Some(Box::new(Strip::new(device, blocks_of(token), ctx)?))
}

// ---- block definitions -----------------------------------------------------------------

#[derive(Clone, Copy, Debug, PartialEq)]
enum Kind {
    /// A channel EQ model (EQ_MODELS_CH).
    Eq,
    /// A gate/dynamics model (DYN_MODELS).
    Dyn,
    /// A standard FX device (FX_MODELS).
    Fx,
}

/// One block of a device: which model it runs and where its controls come from.
struct BlockDef {
    kind: Kind,
    /// The model's token in its table.
    token: &'static str,
    /// Device ids are the model's ids with this prefix ...
    prefix: &'static str,
    /// ... except these (model id, device id).
    renames: &'static [(&'static str, &'static str)],
    /// Device id of the block's ON switch (none: always on).
    on: Option<&'static str>,
    /// Device id of an EQ block's MIX (none: 100 %).
    mix: Option<&'static str>,
    /// Device id of a dynamics block's makeup gain in dB (none: 0).
    makeup: Option<&'static str>,
}

const fn eq(token: &'static str, on: Option<&'static str>, mix: Option<&'static str>) -> BlockDef {
    BlockDef { kind: Kind::Eq, token, prefix: "", renames: &[], on, mix, makeup: None }
}

const fn block(kind: Kind, token: &'static str, prefix: &'static str, on: Option<&'static str>) -> BlockDef {
    BlockDef { kind, token, prefix, renames: &[], on, mix: None, makeup: None }
}

const SOUL: &[BlockDef] = &[eq("SOUL", None, Some("mix"))];
const E88: &[BlockDef] = &[eq("E88", None, Some("mix"))];
const E84: &[BlockDef] = &[eq("E84", None, Some("mix"))];
const F110: &[BlockDef] = &[eq("F110", None, Some("mix"))];
const PULSAR: &[BlockDef] = &[eq("PULSAR", None, Some("mix"))];
const MACH4: &[BlockDef] = &[eq("MACH4", None, Some("mix"))];
const EVEN_CHANNEL: &[BlockDef] = &[
    block(Kind::Dyn, "E88", "g_", Some("g_on")),
    eq("E88", Some("eq_on"), Some("mix")),
    BlockDef { renames: &[("cgain", "d_gain")], ..block(Kind::Dyn, "ECL33", "d_", None) },
];
const SOUL_CHANNEL: &[BlockDef] = &[
    block(Kind::Dyn, "9000G", "g_", Some("g_on")),
    eq("SOUL", Some("eq_on"), Some("mix")),
    block(Kind::Dyn, "9000C", "d_", Some("d_on")),
];
const VINTAGE_CHANNEL: &[BlockDef] =
    &[block(Kind::Dyn, "76LA", "d_", Some("d_on")), eq("PULSAR", None, None), block(Kind::Dyn, "LA", "l_", Some("l_on"))];
const BUS_CHANNEL: &[BlockDef] = &[
    BlockDef { renames: &[("wmix", "w_mix")], ..block(Kind::Dyn, "WARM", "w_", Some("w_on")) },
    eq("E84", Some("eq_on"), Some("mix")),
    BlockDef { makeup: Some("d_gain"), ..block(Kind::Dyn, "SBUS", "d_", Some("d_on")) },
];
const MASTERING: &[BlockDef] = &[
    block(Kind::Fx, "TAPE", "t_", Some("t_on")),
    eq("MACH4", Some("eq_on"), None),
    block(Kind::Fx, "ENHANCE", "e_", Some("e_on")),
    block(Kind::Fx, "LIMITER", "l_", Some("l_on")),
];

fn blocks_of(token: &str) -> &'static [BlockDef] {
    match token {
        "SOUL" => SOUL,
        "E88" => E88,
        "E84" => E84,
        "F110" => F110,
        "PULSAR" => PULSAR,
        "MACH4" => MACH4,
        "*EVEN*" => EVEN_CHANNEL,
        "*SOUL*" => SOUL_CHANNEL,
        "*VINTAGE*" => VINTAGE_CHANNEL,
        "*BUS*" => BUS_CHANNEL,
        "*MASTER*" => MASTERING,
        _ => &[],
    }
}

/// The response in dB at `freqs` of the EQ block of an EQ device or channel
/// strip (its EQ section as set, flat while switched off), for the screen's
/// graph. False for other devices.
pub(super) fn eq_response(token: &str, values: &[f32], freqs: &[f32], out: &mut [f32]) -> bool {
    const SR: f64 = 48_000.0;
    let Some(def) = blocks_of(token).iter().find(|d| d.kind == Kind::Eq) else {
        return false;
    };
    let (Some(device), Some(model)) = (super::model_index(token).map(|i| &FX_MODELS[i]), model_index(EQ_MODELS_CH, def.token))
    else {
        return false;
    };
    let spec = &EQ_MODELS_CH[model];
    let device_value = |i: usize| -> f32 {
        match (device.params.get(i), values.get(i)) {
            (Some(p), Some(v)) => p.sanitise(*v),
            (Some(p), None) => p.default,
            _ => 0.0,
        }
    };
    let mut eq_values = [0.0f32; MAX_FX_PARAMS];
    for (v, p) in eq_values.iter_mut().zip(spec.params) {
        *v = match device.param_index(&format!("{}{}", def.prefix, p.id)) {
            Some(i) => p.sanitise(device_value(i)),
            None => p.default,
        };
    }
    let on = def.on.and_then(|id| device.param_index(id)).is_none_or(|i| device_value(i) >= 0.5);
    let mix = def.mix.and_then(|id| device.param_index(id)).map_or(100.0, device_value);
    let d = crate::wing::eq::design(spec, &eq_values[..spec.params.len()], SR, if on { mix / 100.0 } else { 0.0 });
    for (f, o) in freqs.iter().zip(out.iter_mut()) {
        *o = d.response_db(f64::from(*f), SR) as f32;
    }
    true
}

// ---- running blocks --------------------------------------------------------------------

enum Engine {
    Eq { eq: Box<WingEq>, model: usize },
    Dyn { dynamics: Box<WingDynamics>, model: usize },
    Fx(Box<dyn FxAlgo>),
}

impl Engine {
    fn reset(&mut self) {
        match self {
            Engine::Eq { eq, .. } => eq.reset(),
            Engine::Dyn { dynamics, .. } => dynamics.reset(),
            Engine::Fx(algo) => algo.reset(),
        }
    }
}

struct Block {
    /// The model the block runs.
    spec: &'static ModelSpec,
    engine: Engine,
    /// For each of the model's parameters: its index among the device's (None: the model's default).
    map: Vec<Option<usize>>,
    values: [f32; MAX_FX_PARAMS],
    on: Option<usize>,
    mix: Option<usize>,
    makeup: Option<usize>,
    enabled: bool,
    mix_pct: f32,
    makeup_db: f32,
    /// Crossfade position: 0 = bypassed, 1 = running.
    fade: f32,
    /// Gain reduction of the last block in dB (>= 0).
    reduction: f32,
}

impl Block {
    /// Message thread: resolves the model and the id mapping.
    fn new(device: &'static ModelSpec, def: &BlockDef, ctx: FxContext) -> Option<Self> {
        let sr = ctx.sample_rate as f64;
        let (spec, engine) = match def.kind {
            Kind::Eq => {
                let model = model_index(EQ_MODELS_CH, def.token)?;
                let mut eq = Box::<WingEq>::default();
                eq.prepare(sr);
                (&EQ_MODELS_CH[model], Engine::Eq { eq, model })
            }
            Kind::Dyn => {
                let model = model_index(DYN_MODELS, def.token)?;
                let mut dynamics = Box::<WingDynamics>::default();
                dynamics.prepare(sr);
                (&DYN_MODELS[model], Engine::Dyn { dynamics, model })
            }
            Kind::Fx => {
                let model = model_index(FX_MODELS, def.token)?;
                (&FX_MODELS[model], Engine::Fx(super::utility::create(def.token, ctx)?))
            }
        };
        let map = spec
            .params
            .iter()
            .map(|p| match def.renames.iter().find(|(model_id, _)| *model_id == p.id) {
                Some((_, device_id)) => device.param_index(device_id),
                None => device.param_index(&format!("{}{}", def.prefix, p.id)),
            })
            .collect();
        let index = |id: Option<&str>| id.and_then(|id| device.param_index(id));
        Some(Self {
            spec,
            engine,
            map,
            values: [0.0; MAX_FX_PARAMS],
            on: index(def.on),
            mix: index(def.mix),
            makeup: index(def.makeup),
            enabled: true,
            mix_pct: 100.0,
            makeup_db: 0.0,
            fade: 1.0,
            reduction: 0.0,
        })
    }

    /// Runs the block on `l`/`r` (no bypass handling).
    fn run(&mut self, l: &mut [f32], r: &mut [f32]) {
        let values = &self.values[..self.spec.params.len()];
        match &mut self.engine {
            Engine::Eq { eq, model } => {
                eq.process(&mut [l, r], EQ_MODELS_CH, *model, self.enabled, self.mix_pct, values);
            }
            Engine::Dyn { dynamics, model } => {
                let status = dynamics.process(
                    &mut [l, r],
                    None,
                    *model,
                    values,
                    &SidechainSettings::default(),
                    &XoverSettings::default(),
                    1.0,
                    self.makeup_db,
                );
                self.reduction = status.gain_reduction_db;
            }
            Engine::Fx(algo) => {
                algo.process(l, r);
                self.reduction = -algo.meter();
            }
        }
    }
}

/// A device: blocks in series.
struct Strip {
    blocks: Vec<Block>,
    dry_l: Vec<f32>,
    dry_r: Vec<f32>,
    fade_step: f32,
    meter: f32,
    primed: bool,
}

impl Strip {
    fn new(device: &'static ModelSpec, defs: &[BlockDef], ctx: FxContext) -> Option<Self> {
        let blocks = defs.iter().map(|d| Block::new(device, d, ctx)).collect::<Option<Vec<_>>>()?;
        Some(Self {
            blocks,
            dry_l: vec![0.0; ctx.max_block],
            dry_r: vec![0.0; ctx.max_block],
            fade_step: 1.0 / (0.01 * ctx.sample_rate),
            meter: 0.0,
            primed: false,
        })
    }

    fn process_chunk(&mut self, left: &mut [f32], right: &mut [f32]) {
        let n = left.len();
        let mut deepest = 0.0f32;
        for blk in &mut self.blocks {
            if matches!(blk.engine, Engine::Eq { .. }) {
                // The EQ fades its band gains itself when switched.
                blk.run(left, right);
                continue;
            }
            let target = if blk.enabled { 1.0 } else { 0.0 };
            if blk.fade == 0.0 && target == 0.0 {
                blk.reduction = 0.0;
                continue;
            }
            if blk.fade == 1.0 && target == 1.0 {
                blk.run(left, right);
            } else {
                if blk.fade == 0.0 {
                    // Coming back from bypass: start from a clean state.
                    blk.engine.reset();
                }
                let (dl, dr) = (&mut self.dry_l[..n], &mut self.dry_r[..n]);
                dl.copy_from_slice(left);
                dr.copy_from_slice(right);
                blk.run(left, right);
                let step = if target > blk.fade { self.fade_step } else { -self.fade_step };
                for (((l, r), x), y) in left.iter_mut().zip(right.iter_mut()).zip(dl.iter()).zip(dr.iter()) {
                    blk.fade = (blk.fade + step).clamp(0.0, 1.0);
                    *l = x + blk.fade * (*l - x);
                    *r = y + blk.fade * (*r - y);
                }
            }
            deepest = deepest.max(blk.reduction * blk.fade);
        }
        // Analog-style headroom: linear up to +18 dBFS, never past +24 dBFS.
        for x in left.iter_mut().chain(right.iter_mut()) {
            *x = super::utility::rail(*x);
        }
        self.meter = -deepest;
    }
}

impl FxAlgo for Strip {
    fn set_params(&mut self, p: &Params) {
        let device = |i: usize| -> f32 {
            match (p.spec.params.get(i), p.values.get(i)) {
                (Some(spec), Some(v)) => spec.sanitise(*v),
                (Some(spec), None) => spec.default,
                _ => 0.0,
            }
        };
        for blk in &mut self.blocks {
            for ((v, src), tp) in blk.values.iter_mut().zip(&blk.map).zip(blk.spec.params) {
                *v = match src {
                    Some(i) => tp.sanitise(device(*i)),
                    None => tp.default,
                };
            }
            blk.enabled = blk.on.is_none_or(|i| device(i) >= 0.5);
            blk.mix_pct = blk.mix.map_or(100.0, device);
            blk.makeup_db = blk.makeup.map_or(0.0, device);
            if let Engine::Fx(algo) = &mut blk.engine {
                algo.set_params(&Params::new(blk.spec, &blk.values[..blk.spec.params.len()]));
            }
            if !self.primed {
                blk.fade = if blk.enabled { 1.0 } else { 0.0 };
            }
        }
        self.primed = true;
    }

    fn process(&mut self, left: &mut [f32], right: &mut [f32]) {
        let n = left.len().min(right.len());
        let cap = self.dry_l.len().max(1);
        for (l, r) in left[..n].chunks_mut(cap).zip(right[..n].chunks_mut(cap)) {
            self.process_chunk(l, r);
        }
    }

    fn reset(&mut self) {
        for blk in &mut self.blocks {
            blk.engine.reset();
            blk.fade = if blk.enabled { 1.0 } else { 0.0 };
            blk.reduction = 0.0;
        }
        self.meter = 0.0;
    }

    fn meter(&self) -> f32 {
        self.meter
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::wing::fx::FxDevice;
    use std::f32::consts::TAU;

    #[test]
    fn devices() {
        for token in super::TOKENS {
            crate::wing::fx::tests::check_device(token);
        }
    }

    struct Rig {
        dev: FxDevice,
        values: [f32; MAX_FX_PARAMS],
    }

    impl Rig {
        fn new(token: &str) -> Self {
            let model = super::super::model_index(token).unwrap();
            let dev = FxDevice::new(model, FxContext { sample_rate: 48_000.0, max_block: 256 }).unwrap();
            let mut values = [0.0; MAX_FX_PARAMS];
            FX_MODELS[model].defaults_into(&mut values);
            Self { dev, values }
        }

        fn set(&mut self, id: &str, v: f32) -> &mut Self {
            let i = self.dev.spec().param_index(id).unwrap_or_else(|| panic!("no parameter {id}"));
            self.values[i] = v;
            self
        }

        fn run(&mut self, l: &mut [f32], r: &mut [f32]) {
            for (a, b) in l.chunks_mut(256).zip(r.chunks_mut(256)) {
                self.dev.process(&self.values, a, b);
            }
        }

        fn sine_gain_db(&mut self, freq: f32, amp: f32) -> f32 {
            let n = 48_000;
            let x: Vec<f32> = (0..n).map(|i| (TAU * freq * i as f32 / 48_000.0).sin() * amp).collect();
            let (mut l, mut r) = (x.clone(), x.clone());
            self.run(&mut l, &mut r);
            let rms = |v: &[f32]| (v.iter().map(|s| s * s).sum::<f32>() / v.len() as f32).sqrt();
            20.0 * (rms(&l[n / 2..]) / rms(&x[n / 2..])).log10()
        }
    }

    fn noise(n: usize) -> Vec<f32> {
        (0..n).map(|i| ((i * 7919) % 997) as f32 / 997.0 - 0.5).collect()
    }

    #[test]
    fn every_parameter_is_wired() {
        // Model parameters a device doesn't have (they take the model's default).
        let absent = |token: &str, model: &str| -> &'static [&'static str] {
            match (token, model) {
                ("*MASTER*", "MACH4") => &["again"],
                ("*MASTER*", "TAPE") => &["out"],
                ("*MASTER*", "ENHANCE") => &["g", "solo"],
                _ => &[],
            }
        };
        for token in TOKENS {
            let device = &FX_MODELS[super::super::model_index(token).unwrap()];
            let ctx = FxContext { sample_rate: 48_000.0, max_block: 64 };
            let strip = Strip::new(device, blocks_of(token), ctx).unwrap();
            let mut used = vec![false; device.params.len()];
            for (blk, def) in strip.blocks.iter().zip(blocks_of(token)) {
                let missing: Vec<&str> =
                    blk.map.iter().zip(blk.spec.params).filter(|(m, _)| m.is_none()).map(|(_, p)| p.id).collect();
                assert_eq!(missing, absent(token, def.token), "{token}/{}", def.token);
                for i in blk.map.iter().flatten().chain(&blk.on).chain(&blk.mix).chain(&blk.makeup) {
                    used[*i] = true;
                }
                assert_eq!(blk.on.is_some(), def.on.is_some(), "{token}/{}: ON switch", def.token);
                assert_eq!(blk.mix.is_some(), def.mix.is_some(), "{token}/{}: MIX", def.token);
                assert_eq!(blk.makeup.is_some(), def.makeup.is_some(), "{token}/{}: makeup", def.token);
            }
            for (u, p) in used.iter().zip(device.params) {
                assert!(*u, "{token}: {} drives nothing", p.id);
            }
        }
    }

    #[test]
    fn eq_devices_are_flat_at_defaults_and_follow_the_channel_models() {
        for token in ["SOUL", "E88", "E84", "F110", "PULSAR", "MACH4"] {
            let mut rig = Rig::new(token);
            for f in [40.0, 300.0, 1000.0, 5000.0, 15_000.0] {
                let g = rig.sine_gain_db(f, 0.1);
                assert!(g.abs() < 0.1, "{token}: {g} dB at {f} Hz");
            }
        }
        let mut rig = Rig::new("SOUL");
        rig.set("lmg", 5.0).set("lmf", 5.0);
        let full = rig.sine_gain_db(700.0, 0.05);
        assert!(full >= 10.0, "{full}");
        let mut rig = Rig::new("SOUL");
        rig.set("lmg", 5.0).set("lmf", 5.0).set("mix", 50.0);
        let half = rig.sine_gain_db(700.0, 0.05);
        assert!((half - full / 2.0).abs() < 1.0, "{half} vs {full}");
        let mut rig = Rig::new("E84");
        rig.set("mg", 5.0);
        assert!(rig.sine_gain_db(1600.0, 0.05) >= 12.0);
    }

    #[test]
    fn even_channel_with_everything_off_is_unity() {
        let mut rig = Rig::new("*EVEN*");
        rig.set("g_on", 0.0).set("eq_on", 0.0).set("d_lon", 0.0).set("d_con", 0.0);
        let x = noise(9600);
        let (mut l, mut r) = (x.clone(), x.clone());
        rig.run(&mut l, &mut r);
        assert_eq!(l, x);
        assert_eq!(r, x);
        assert_eq!(rig.dev.meter(), 0.0);
    }

    #[test]
    fn strips_switch_their_blocks() {
        // Every strip with all its blocks switched off passes the signal untouched.
        for (token, offs) in [
            ("*SOUL*", &["g_on", "eq_on", "d_on"][..]),
            ("*BUS*", &["w_on", "eq_on", "d_on"][..]),
            ("*MASTER*", &["t_on", "eq_on", "e_on", "l_on"][..]),
        ] {
            let mut rig = Rig::new(token);
            for id in offs {
                rig.set(id, 0.0);
            }
            let x = noise(4800);
            let (mut l, mut r) = (x.clone(), x.clone());
            rig.run(&mut l, &mut r);
            assert_eq!(l, x, "{token}");
        }

        // The Soul gate closes on a quiet signal once switched on.
        let mut rig = Rig::new("*SOUL*");
        rig.set("g_on", 1.0).set("d_on", 0.0);
        assert!(rig.sine_gain_db(300.0, 0.001) < -20.0);

        // The bus compressor's GAIN is its makeup.
        let mut rig = Rig::new("*BUS*");
        rig.set("d_gain", 6.0).set("d_thr", 0.0);
        let g = rig.sine_gain_db(1000.0, 0.01);
        assert!((g - 6.0).abs() < 0.2, "{g}");

        // The Vintage channel's 1176 compresses a hot signal.
        let mut rig = Rig::new("*VINTAGE*");
        rig.set("d_in", 0.0);
        rig.sine_gain_db(200.0, 0.5);
        assert!(rig.dev.meter() < -3.0);
    }

    #[test]
    fn mastering_limiter_holds_the_ceiling() {
        let mut rig = Rig::new("*MASTER*");
        rig.set("l_gin", 12.0).set("l_gout", -1.0);
        let x: Vec<f32> = (0..48_000).map(|i| (TAU * 100.0 * i as f32 / 48_000.0).sin() * 0.9).collect();
        let (mut l, mut r) = (x.clone(), x.clone());
        rig.run(&mut l, &mut r);
        let peak = l.iter().chain(&r).fold(0.0f32, |a, v| a.max(v.abs()));
        assert!(peak <= 10f32.powf(-1.0 / 20.0) * 1.0001, "{peak}");
        assert!(rig.dev.meter() < -6.0);
    }

    #[test]
    fn switching_a_block_is_click_free() {
        // Toggling the 1176 mid-signal never jumps by more than the signal moves.
        let mut rig = Rig::new("*VINTAGE*");
        rig.set("d_in", 0.0);
        let n = 48_000;
        let x: Vec<f32> = (0..n).map(|i| (TAU * 100.0 * i as f32 / 48_000.0).sin() * 0.5).collect();
        let (mut l, mut r) = (x.clone(), x.clone());
        for (k, (a, b)) in l.chunks_mut(256).zip(r.chunks_mut(256)).enumerate() {
            rig.values[rig.dev.spec().param_index("d_on").unwrap()] = if (k / 40) % 2 == 0 { 1.0 } else { 0.0 };
            rig.dev.process(&rig.values, a, b);
        }
        let max_step = l.windows(2).map(|w| (w[1] - w[0]).abs()).fold(0.0f32, f32::max);
        assert!(max_step < 0.1, "{max_step}");
    }
}
