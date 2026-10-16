//! Parameter blocks shared lock-free between the UI and the audio thread.
//!
//! The layout mirrors the WING's parameter tree: every strip has the same
//! block (INPUT, FILTER, GATE slot, EQ, DYNAMICS slot, two inserts, sends to
//! the 4 mains and 8 matrices, DCA and mute-group assignments); each kind uses
//! the parts the console gives it. All structs are `#[repr(C)]` and mirrored
//! by `livedaw_engine.h`.

use crate::atomic::AtomicF32;
use livedaw_dsp::wing::{
    dynamics::{SidechainSettings, XoverSettings},
    filter::FilterSettings,
    model_index, DYN_MODELS, EQ_MODELS_BUS, EQ_MODELS_CH, FILTER_TOOLS, FX_MODELS, MAX_DYN_PARAMS, MAX_EQ_PARAMS, MAX_FX_PARAMS,
    MAX_TOOL_PARAMS,
};
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicU32, Ordering::Relaxed};

pub const NUM_MAINS: usize = 4;
pub const NUM_MATRICES: usize = 8;
pub const NUM_AUX: usize = 8;
pub const NUM_DCAS: usize = 16;
pub const NUM_MUTE_GROUPS: usize = 8;
pub const NUM_FX: usize = 16;
/// FX slot models (indices into `FX_MODELS`): an empty slot, and EXT (a plugin
/// hosted by the application).
pub const FX_NONE: usize = 0;
pub const FX_EXT: usize = 1;
/// The WING's channel and bus counts (this console can grow beyond them).
pub const WING_CHANNELS: usize = 40;
pub const WING_BUSES: usize = 16;
/// Buses this console can feed from other buses (buses 9+ into buses 1-8).
pub const BUS_TO_BUS_TARGETS: usize = 8;

#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum StripKind {
    /// Input channel (40 on the console).
    Channel = 0,
    /// Aux input channel (8).
    Aux = 1,
    /// Stereo mix bus (16).
    Bus = 2,
    /// Stereo main bus (4).
    Main = 3,
    /// Stereo matrix bus (8).
    Matrix = 4,
}

impl StripKind {
    pub const ALL: [StripKind; 5] = [StripKind::Channel, StripKind::Aux, StripKind::Bus, StripKind::Main, StripKind::Matrix];

    pub fn from_i32(v: i32) -> Option<Self> {
        Self::ALL.get(usize::try_from(v).ok()?).copied()
    }

    /// Fed by sources (input channels and aux channels).
    pub fn is_input(self) -> bool {
        matches!(self, Self::Channel | Self::Aux)
    }

    pub fn is_bus(self) -> bool {
        self == Self::Bus
    }

    /// Summing buses (bus, main, matrix).
    pub fn is_output(self) -> bool {
        !self.is_input()
    }

    /// Name used in show files and parameter paths ("ch", "aux", "bus", "main", "mtx").
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Channel => "ch",
            Self::Aux => "aux",
            Self::Bus => "bus",
            Self::Main => "main",
            Self::Matrix => "mtx",
        }
    }

    /// Inverse of [`as_str`](Self::as_str); unknown names are channels.
    pub fn parse(s: &str) -> Self {
        match s {
            "aux" => Self::Aux,
            "bus" => Self::Bus,
            "main" => Self::Main,
            "mtx" => Self::Matrix,
            _ => Self::Channel,
        }
    }

    /// Short label as on the scribble strips ("CH", "AUX", "BUS", "MAIN", "MTX").
    pub fn label(self) -> &'static str {
        match self {
            Self::Channel => "CH",
            Self::Aux => "AUX",
            Self::Bus => "BUS",
            Self::Main => "MAIN",
            Self::Matrix => "MTX",
        }
    }
}

// ---- blocks ---------------------------------------------------------------------------

/// INPUT: trim, balance, polarity and delay (input delay on channels, output
/// delay on buses, mains and matrices).
#[repr(C)]
#[derive(Debug)]
pub struct InputParams {
    pub trim_db: AtomicF32,
    pub balance_db: AtomicF32,
    pub invert: AtomicBool,
    pub delay_on: AtomicBool,
    /// Index into `livedaw_dsp::delay::DELAY_MODES` (M, FT, MS, SMP).
    pub delay_mode: AtomicI32,
    /// In `delay_mode` units.
    pub delay: AtomicF32,
}

impl Default for InputParams {
    fn default() -> Self {
        Self {
            trim_db: AtomicF32::new(0.0),
            balance_db: AtomicF32::new(0.0),
            invert: AtomicBool::new(false),
            delay_on: AtomicBool::new(false),
            delay_mode: AtomicI32::new(2),
            delay: AtomicF32::new(0.0),
        }
    }
}

/// FILTER: low cut, high cut, tool filter.
#[repr(C)]
#[derive(Debug)]
pub struct FilterParams {
    pub low_cut: AtomicBool,
    pub high_cut: AtomicBool,
    pub tool: AtomicBool,
    pub low_cut_freq: AtomicF32,
    pub low_cut_slope: AtomicI32,
    pub high_cut_freq: AtomicF32,
    pub high_cut_slope: AtomicI32,
    pub tool_model: AtomicI32,
    pub tool_params: [AtomicF32; MAX_TOOL_PARAMS],
}

impl Default for FilterParams {
    fn default() -> Self {
        let d = FilterSettings::default();
        let p = Self {
            low_cut: AtomicBool::new(d.low_cut),
            high_cut: AtomicBool::new(d.high_cut),
            tool: AtomicBool::new(d.tool),
            low_cut_freq: AtomicF32::new(d.low_cut_freq),
            low_cut_slope: AtomicI32::new(d.low_cut_slope as i32),
            high_cut_freq: AtomicF32::new(d.high_cut_freq),
            high_cut_slope: AtomicI32::new(d.high_cut_slope as i32),
            tool_model: AtomicI32::new(0),
            tool_params: Default::default(),
        };
        p.set_tool_model(0);
        p
    }
}

impl FilterParams {
    /// Selects a tool filter and loads its defaults.
    pub fn set_tool_model(&self, model: usize) {
        let Some(spec) = FILTER_TOOLS.get(model) else { return };
        self.tool_model.store(model as i32, Relaxed);
        for (i, v) in self.tool_params.iter().enumerate() {
            v.store(spec.params.get(i).map_or(0.0, |p| p.default));
        }
    }

    pub fn snapshot(&self) -> FilterSettings {
        FilterSettings {
            low_cut: self.low_cut.load(Relaxed),
            low_cut_freq: self.low_cut_freq.load(),
            low_cut_slope: self.low_cut_slope.load(Relaxed).clamp(0, 3) as usize,
            high_cut: self.high_cut.load(Relaxed),
            high_cut_freq: self.high_cut_freq.load(),
            high_cut_slope: self.high_cut_slope.load(Relaxed).clamp(0, 1) as usize,
            tool: self.tool.load(Relaxed),
            tool_model: self.tool_model.load(Relaxed).max(0) as usize,
            tool_params: [self.tool_params[0].load(), self.tool_params[1].load()],
        }
    }
}

/// A dynamics slot (GATE or DYNAMICS) holding any of the 32 models.
#[repr(C)]
#[derive(Debug)]
pub struct DynSlotParams {
    pub on: AtomicBool,
    /// Index into `DYN_MODELS`.
    pub model: AtomicI32,
    /// The model's parameters, in its table order.
    pub values: [AtomicF32; MAX_DYN_PARAMS],
    /// Wet/dry in %.
    pub mix: AtomicF32,
    /// Make-up gain in dB.
    pub gain: AtomicF32,
    pub sc_filter: AtomicI32,
    pub sc_freq: AtomicF32,
    pub sc_q: AtomicF32,
    /// -1 = the strip itself, else the index of the keying input channel.
    pub sc_source: AtomicI32,
    pub sc_tap: AtomicI32,
    pub xo_kind: AtomicI32,
    pub xo_freq: AtomicF32,
    pub xo_q: AtomicF32,
    pub xo_depth: AtomicF32,
}

impl DynSlotParams {
    pub fn new(model_token: &str) -> Self {
        let p = Self {
            on: AtomicBool::new(false),
            model: AtomicI32::new(0),
            values: Default::default(),
            mix: AtomicF32::new(100.0),
            gain: AtomicF32::new(0.0),
            sc_filter: AtomicI32::new(0),
            sc_freq: AtomicF32::new(1000.0),
            sc_q: AtomicF32::new(1.0),
            sc_source: AtomicI32::new(-1),
            sc_tap: AtomicI32::new(0),
            xo_kind: AtomicI32::new(0),
            xo_freq: AtomicF32::new(1000.0),
            xo_q: AtomicF32::new(1.0),
            xo_depth: AtomicF32::new(0.0),
        };
        p.set_model(model_index(DYN_MODELS, model_token).unwrap_or(0));
        p
    }

    /// Selects a model and loads its defaults (the session keeps per-model memories).
    pub fn set_model(&self, model: usize) {
        let Some(spec) = DYN_MODELS.get(model) else { return };
        self.model.store(model as i32, Relaxed);
        let mut d = [0.0; MAX_DYN_PARAMS];
        spec.defaults_into(&mut d);
        for (v, x) in self.values.iter().zip(d) {
            v.store(x);
        }
    }

    pub fn model_index(&self) -> usize {
        (self.model.load(Relaxed).max(0) as usize).min(DYN_MODELS.len() - 1)
    }

    /// A parameter of the current model by its console id ("thr").
    pub fn param(&self, id: &str) -> Option<&AtomicF32> {
        self.values.get(DYN_MODELS[self.model_index()].param_index(id)?)
    }

    pub fn values_snapshot(&self, out: &mut [f32; MAX_DYN_PARAMS]) {
        for (o, v) in out.iter_mut().zip(&self.values) {
            *o = v.load();
        }
    }

    pub fn sidechain(&self) -> SidechainSettings {
        SidechainSettings {
            filter: self.sc_filter.load(Relaxed).clamp(0, 4) as usize,
            freq: self.sc_freq.load(),
            q: self.sc_q.load(),
        }
    }

    pub fn xover(&self) -> XoverSettings {
        XoverSettings {
            kind: self.xo_kind.load(Relaxed).clamp(0, 5) as usize,
            freq: self.xo_freq.load(),
            q: self.xo_q.load(),
            depth: self.xo_depth.load(),
        }
    }
}

/// EQ block: model + its parameters.
#[repr(C)]
#[derive(Debug)]
pub struct EqBlockParams {
    pub on: AtomicBool,
    /// Index into `EQ_MODELS_CH` (channels) or `EQ_MODELS_BUS` (buses, mains, matrices).
    pub model: AtomicI32,
    /// 0..125 %.
    pub mix: AtomicF32,
    pub values: [AtomicF32; MAX_EQ_PARAMS],
}

impl EqBlockParams {
    pub fn new(bus: bool) -> Self {
        let p =
            Self { on: AtomicBool::new(true), model: AtomicI32::new(0), mix: AtomicF32::new(100.0), values: Default::default() };
        p.set_model(0, bus);
        p
    }

    pub fn set_model(&self, model: usize, bus: bool) {
        let table = if bus { EQ_MODELS_BUS } else { EQ_MODELS_CH };
        let Some(spec) = table.get(model) else { return };
        self.model.store(model as i32, Relaxed);
        let mut d = [0.0; MAX_EQ_PARAMS];
        spec.defaults_into(&mut d);
        for (v, x) in self.values.iter().zip(d) {
            v.store(x);
        }
    }

    pub fn model_index(&self, bus: bool) -> usize {
        let table = if bus { EQ_MODELS_BUS } else { EQ_MODELS_CH };
        (self.model.load(Relaxed).max(0) as usize).min(table.len() - 1)
    }

    /// A parameter of the current model by its console id ("2g").
    pub fn param(&self, id: &str, bus: bool) -> Option<&AtomicF32> {
        let table = if bus { EQ_MODELS_BUS } else { EQ_MODELS_CH };
        self.values.get(table[self.model_index(bus)].param_index(id)?)
    }

    pub fn values_snapshot(&self, out: &mut [f32; MAX_EQ_PARAMS]) {
        for (o, v) in out.iter_mut().zip(&self.values) {
            *o = v.load();
        }
    }
}

/// POST INSERT modes.
pub const POST_INSERT_MODES: [&str; 3] = ["FX", "AUTO_X", "AUTO_Y"];

/// An insert point: one of the 16 FX slots (or automix on the post insert).
#[repr(C)]
#[derive(Debug)]
pub struct InsertParams {
    pub on: AtomicBool,
    /// FX slot index, -1 = none.
    pub fx: AtomicI32,
    /// Post insert only: index into [`POST_INSERT_MODES`].
    pub mode: AtomicI32,
    /// Automix weight in dB.
    pub weight: AtomicF32,
}

impl Default for InsertParams {
    fn default() -> Self {
        Self { on: AtomicBool::new(false), fx: AtomicI32::new(-1), mode: AtomicI32::new(0), weight: AtomicF32::new(0.0) }
    }
}

/// Send from a strip to one of the 4 main buses.
#[repr(C)]
#[derive(Debug)]
pub struct MainSendParams {
    pub on: AtomicBool,
    /// Taken at the strip's tap point instead of post-fader.
    pub pre: AtomicBool,
    pub level_db: AtomicF32,
}

/// Send from a strip to one of the 8 matrices.
#[repr(C)]
#[derive(Debug)]
pub struct MatrixSendParams {
    pub on: AtomicBool,
    pub pre: AtomicBool,
    pub level_db: AtomicF32,
    pub pan: AtomicF32,
}

/// Monitor bus a solo goes to.
pub const MONITOR_BUSES: [&str; 3] = ["A", "B", "A+B"];

/// Everything the UI can change on a strip. Bus sends live in its `SendBank`.
#[repr(C)]
#[derive(Debug)]
pub struct StripParams {
    pub input: InputParams,
    pub filter: FilterParams,
    pub gate: DynSlotParams,
    pub eq: EqBlockParams,
    pub dynamics: DynSlotParams,
    pub pre_insert: InsertParams,
    pub post_insert: InsertParams,
    /// Index into `PROCESS_ORDERS` (channels and aux).
    pub process_order: AtomicI32,
    /// Index into `TAP_POINTS`: where pre-fader sends are taken.
    pub tap: AtomicI32,

    pub fader_db: AtomicF32,
    /// -100 (left) .. +100 (right).
    pub pan: AtomicF32,
    /// Stereo width in % (-150..150).
    pub width: AtomicF32,
    pub mute: AtomicBool,
    pub solo: AtomicBool,
    pub solo_safe: AtomicBool,
    /// Index into [`MONITOR_BUSES`].
    pub monitor_bus: AtomicI32,

    /// Mono or stereo processing (inputs; buses, mains and matrices are stereo).
    pub stereo: AtomicBool,
    /// Buses: sum the bus to mono.
    pub mono: AtomicBool,
    /// Hardware input channels (-1 = none); the right one is used when stereo.
    pub input_left: AtomicI32,
    pub input_right: AtomicI32,
    /// Hardware outputs (-1 = none). Channels: direct out (pre-fader).
    pub output_left: AtomicI32,
    pub output_right: AtomicI32,

    pub mains: [MainSendParams; NUM_MAINS],
    pub matrix: [MatrixSendParams; NUM_MATRICES],

    /// Bit n = member of DCA n+1.
    pub dca_mask: AtomicU32,
    /// Bit n = member of mute group n+1.
    pub mute_group_mask: AtomicU32,

    /// Matrices: direct input from an input channel.
    pub direct_on: AtomicBool,
    pub direct_invert: AtomicBool,
    pub direct_pre: AtomicBool,
    /// Index of the input channel (-1 = none).
    pub direct_source: AtomicI32,
    pub direct_level_db: AtomicF32,

    /// WING colour index 1..18.
    pub colour: AtomicI32,
    /// WING icon number (0 = none).
    pub icon: AtomicI32,
    /// Scribble strip light.
    pub led: AtomicBool,

    /// Scene recalls skip this strip (snippets still apply).
    pub recall_safe: AtomicBool,
}

/// The WING's 18 strip colours (index 1..18 in `colour`), name and RGB.
pub const WING_COLOURS: [(&str, u32); 18] = [
    ("Blue", 0x3a6fe8),
    ("Azure", 0x4fa6f7),
    ("Indigo", 0x5a4fe0),
    ("Turquoise", 0x2ec6c6),
    ("Green", 0x2fb84a),
    ("Lime", 0x9acd32),
    ("Yellow", 0xf2d230),
    ("Brown", 0xb07a3a),
    ("Red", 0xe83a3a),
    ("Salmon", 0xf07c6c),
    ("Magenta", 0xe84aa8),
    ("Purple", 0x9b4fe0),
    ("Amber", 0xf0a030),
    ("Sky Blue", 0x7ec8f0),
    ("Orange Red", 0xf06030),
    ("Mint Green", 0x6ee0b0),
    ("Gray", 0x9a9a9a),
    ("White", 0xf0f0f0),
];

/// RGB of WING colour `index` (1..18; anything else is gray).
pub fn colour_rgb(index: i32) -> u32 {
    usize::try_from(index - 1).ok().and_then(|i| WING_COLOURS.get(i)).map_or(0x9a9a9a, |c| c.1)
}

/// The WING colour (1..18) closest to an RGB value.
pub fn nearest_colour(rgb: u32) -> i32 {
    let split = |c: u32| [(c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff].map(|v| v as i32);
    let target = split(rgb);
    let distance = |c: u32| split(c).iter().zip(&target).map(|(a, b)| (a - b) * (a - b)).sum::<i32>();
    let best = WING_COLOURS.iter().enumerate().min_by_key(|(_, c)| distance(c.1)).map_or(0, |(i, _)| i);
    best as i32 + 1
}

/// Default WING colour per kind.
pub fn default_colour(kind: StripKind) -> i32 {
    match kind {
        StripKind::Channel => 1,
        StripKind::Aux => 4,
        StripKind::Bus => 5,
        StripKind::Main => 9,
        StripKind::Matrix => 13,
    }
}

impl StripParams {
    pub fn new(kind: StripKind) -> Self {
        let output = kind.is_output();
        let new_main = |i: usize| MainSendParams {
            // Channels and aux feed Main 1 by default; buses are aux mixes until routed.
            on: AtomicBool::new(i == 0 && kind.is_input()),
            pre: AtomicBool::new(false),
            level_db: AtomicF32::new(0.0),
        };
        let new_mtx = |_| MatrixSendParams {
            on: AtomicBool::new(false),
            pre: AtomicBool::new(false),
            level_db: AtomicF32::new(0.0),
            pan: AtomicF32::new(0.0),
        };
        let p = Self {
            input: InputParams::default(),
            filter: FilterParams::default(),
            gate: DynSlotParams::new("GATE"),
            eq: EqBlockParams::new(output),
            dynamics: DynSlotParams::new("COMP"),
            pre_insert: InsertParams::default(),
            post_insert: InsertParams::default(),
            process_order: AtomicI32::new(0),
            tap: AtomicI32::new(5), // PFL
            fader_db: AtomicF32::new(0.0),
            pan: AtomicF32::new(0.0),
            width: AtomicF32::new(100.0),
            mute: AtomicBool::new(false),
            solo: AtomicBool::new(false),
            solo_safe: AtomicBool::new(false),
            monitor_bus: AtomicI32::new(0),
            stereo: AtomicBool::new(output),
            mono: AtomicBool::new(false),
            input_left: AtomicI32::new(-1),
            input_right: AtomicI32::new(-1),
            output_left: AtomicI32::new(-1),
            output_right: AtomicI32::new(-1),
            mains: std::array::from_fn(new_main),
            matrix: std::array::from_fn(new_mtx),
            dca_mask: AtomicU32::new(0),
            mute_group_mask: AtomicU32::new(0),
            direct_on: AtomicBool::new(false),
            direct_invert: AtomicBool::new(false),
            direct_pre: AtomicBool::new(false),
            direct_source: AtomicI32::new(-1),
            direct_level_db: AtomicF32::new(0.0),
            colour: AtomicI32::new(default_colour(kind)),
            icon: AtomicI32::new(0),
            led: AtomicBool::new(true),
            recall_safe: AtomicBool::new(false),
        };
        p.eq.on.store(true, Relaxed);
        p
    }
}

/// Meters written by the audio thread. Peaks are "max since last read" (the UI
/// exchanges them with 0); dynamics meters hold the latest block's value.
#[repr(C)]
#[derive(Debug, Default)]
pub struct StripMeters {
    pub input_peak: [AtomicF32; 2],
    pub output_peak: [AtomicF32; 2],
    pub gate_reduction_db: AtomicF32,
    pub dyn_reduction_db: AtomicF32,
    /// Gate closed (the console's GATE LED).
    pub gate_active: AtomicBool,
    /// Dynamics working (DYN LED).
    pub dyn_active: AtomicBool,
    pub current_width: AtomicI32,
    /// Blocks that produced NaN/inf and were silenced.
    pub dsp_faults: AtomicI32,
    /// Automix gain applied in the last block, dB.
    pub automix_db: AtomicF32,
}

/// A DCA group.
#[repr(C)]
#[derive(Debug)]
pub struct DcaParams {
    pub fader_db: AtomicF32,
    pub mute: AtomicBool,
    pub solo: AtomicBool,
    pub colour: AtomicI32,
}

impl Default for DcaParams {
    fn default() -> Self {
        Self {
            fader_db: AtomicF32::new(0.0),
            mute: AtomicBool::new(false),
            solo: AtomicBool::new(false),
            colour: AtomicI32::new(7),
        }
    }
}

/// One of the 16 FX slots (the console's EFFECTS screen).
#[repr(C)]
#[derive(Debug)]
pub struct FxParams {
    pub bypass: AtomicBool,
    /// Wet/dry in %.
    pub mix: AtomicF32,
    /// Index into `FX_MODELS`: `FX_NONE`, `FX_EXT` (a plugin) or a built-in device.
    pub model: AtomicI32,
    /// The device's parameters, in its table order.
    pub values: [AtomicF32; MAX_FX_PARAMS],
}

impl Default for FxParams {
    fn default() -> Self {
        Self {
            bypass: AtomicBool::new(false),
            mix: AtomicF32::new(100.0),
            model: AtomicI32::new(FX_NONE as i32),
            values: std::array::from_fn(|_| AtomicF32::new(0.0)),
        }
    }
}

impl FxParams {
    pub fn model_index(&self) -> usize {
        (self.model.load(Relaxed).max(0) as usize).min(FX_MODELS.len() - 1)
    }

    /// Selects a model; with `defaults`, loads the device's default settings.
    /// (The engine creates the device itself: use `Engine::set_fx_model`.)
    pub fn set_model(&self, model: usize, defaults: bool) {
        let Some(spec) = FX_MODELS.get(model) else { return };
        if defaults {
            let mut d = [0.0; MAX_FX_PARAMS];
            spec.defaults_into(&mut d);
            for (v, x) in self.values.iter().zip(d) {
                v.store(x);
            }
        }
        self.model.store(model as i32, Relaxed);
    }

    /// A parameter of the current device by its console id ("dcy").
    pub fn param(&self, id: &str) -> Option<&AtomicF32> {
        self.values.get(FX_MODELS[self.model_index()].param_index(id)?)
    }

    pub fn values_snapshot(&self, out: &mut [f32; MAX_FX_PARAMS]) {
        for (o, v) in out.iter_mut().zip(&self.values) {
            *o = v.load();
        }
    }
}

/// Engine-wide controls and status, shared with the UI.
#[repr(C)]
#[derive(Debug)]
pub struct EngineControls {
    pub solo_afl: AtomicBool,
    pub any_solo_active: AtomicBool,
    pub output_protection: AtomicBool,
    pub monitor_level_db: AtomicF32,
    pub monitor_output_left: AtomicI32,
    pub monitor_output_right: AtomicI32,
    pub output_ceiling_db: AtomicF32,
    /// Blocks in which the output safety clip engaged.
    pub output_clip_events: AtomicI32,
    pub monitor_peak: [AtomicF32; 2],
    /// Smoothed callback time relative to the block duration.
    pub dsp_load: AtomicF32,
    /// Engine-level faults (a caught panic silences the block).
    pub engine_faults: AtomicI32,
    /// Bit n = mute group n+1 engaged.
    pub mute_groups: AtomicU32,
    pub dcas: [DcaParams; NUM_DCAS],
    pub fx: [FxParams; NUM_FX],
}

impl Default for EngineControls {
    fn default() -> Self {
        Self {
            solo_afl: AtomicBool::new(false),
            any_solo_active: AtomicBool::new(false),
            output_protection: AtomicBool::new(true),
            monitor_level_db: AtomicF32::new(0.0),
            monitor_output_left: AtomicI32::new(-1),
            monitor_output_right: AtomicI32::new(-1),
            output_ceiling_db: AtomicF32::new(0.0),
            output_clip_events: AtomicI32::new(0),
            monitor_peak: Default::default(),
            dsp_load: AtomicF32::new(0.0),
            engine_faults: AtomicI32::new(0),
            mute_groups: AtomicU32::new(0),
            dcas: Default::default(),
            fx: Default::default(),
        }
    }
}

impl EngineControls {
    /// Resets the console-wide settings (not the status counters).
    pub fn reset(&self) {
        self.solo_afl.store(false, Relaxed);
        self.monitor_level_db.store(0.0);
        self.mute_groups.store(0, Relaxed);
        for d in &self.dcas {
            d.fader_db.store(0.0);
            d.mute.store(false, Relaxed);
            d.solo.store(false, Relaxed);
            d.colour.store(7, Relaxed);
        }
        for f in &self.fx {
            f.bypass.store(false, Relaxed);
            f.mix.store(100.0);
            f.set_model(FX_NONE, true);
        }
    }
}
