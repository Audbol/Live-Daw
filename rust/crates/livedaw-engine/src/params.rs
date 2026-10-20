//! Parameter blocks shared lock-free between the UI and the audio thread.
//!
//! The layout mirrors the WING's parameter tree: every strip has the same
//! block (INPUT, FILTER, GATE slot, EQ, DYNAMICS slot, two inserts, sends to
//! the 4 mains and 8 matrices, DCA and mute-group assignments); each kind uses
//! the parts the console gives it. All structs are `#[repr(C)]` and mirrored
//! by `livedaw_engine.h`.

use crate::atomic::AtomicF32;
use crate::monitor::MonitorControls;
use crate::solo::solo_mode;
use crate::talk::TalkParams;
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

/// The aux channels' dynamics slot holds the WING GATE at -36 dB on a
/// factory-set console.
pub const AUX_GATE_THRESHOLD_DB: f32 = -36.0;

impl DynSlotParams {
    pub fn new(model_token: &str) -> Self {
        let (sc, xo) = (SidechainSettings::default(), XoverSettings::default());
        let p = Self {
            on: AtomicBool::new(false),
            model: AtomicI32::new(0),
            values: Default::default(),
            mix: AtomicF32::new(100.0),
            gain: AtomicF32::new(0.0),
            sc_filter: AtomicI32::new(0),
            sc_freq: AtomicF32::new(sc.freq),
            sc_q: AtomicF32::new(sc.q),
            sc_source: AtomicI32::new(-1),
            sc_tap: AtomicI32::new(0),
            xo_kind: AtomicI32::new(0),
            xo_freq: AtomicF32::new(xo.freq),
            xo_q: AtomicF32::new(xo.q),
            xo_depth: AtomicF32::new(xo.depth),
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
    /// Taken at the strip's tap point instead of post-fader (the strip's mute
    /// still cuts it).
    pub pre: AtomicBool,
    pub level_db: AtomicF32,
}

/// Send from a strip to one of the 8 matrices.
#[repr(C)]
#[derive(Debug)]
pub struct MatrixSendParams {
    pub on: AtomicBool,
    /// TAP mode: taken at the strip's tap point (pre-fader for buses and mains).
    pub pre: AtomicBool,
    /// IGNORE CHANNEL MUTE (input and aux channels, TAP mode only): the send
    /// stays on while the channel is muted.
    pub ignore_mute: AtomicBool,
    /// PAN LINK (input and aux channels): the send's pan follows the
    /// channel's pan (else `pan` is used). Buses and mains always send their
    /// panned output, balanced by `pan`.
    pub pan_link: AtomicBool,
    /// With `pan_link`: follow the channel's pan mirrored (LINK REVERSED).
    pub pan_reversed: AtomicBool,
    pub level_db: AtomicF32,
    pub pan: AtomicF32,
}

/// Monitor bus a solo goes to (SOLO MODE LIVE): A = PHONES, B = SPEAKERS, both.
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
    /// Index into `PROCESS_ORDERS` (input channels; aux, buses, mains and
    /// matrices have a fixed order).
    pub process_order: AtomicI32,
    /// Index into `TAP_POINTS`: where TAP-mode sends are taken (input channels;
    /// aux, buses, mains and matrices are fixed at PFL, after the pre insert).
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

    /// ALT input (WING INPUT SELECT, input and aux channels): the source used
    /// instead of `input_left`/`input_right` when ALT is selected (-1 = none;
    /// the right one is used when stereo). Like the main input, a hardware
    /// input or a playback channel (`PLAYBACK_SOURCE_BASE + n`).
    pub alt_input_left: AtomicI32,
    pub alt_input_right: AtomicI32,
    /// The strip's own MAIN / ALT switch (used unless INPUT SELECT is global).
    pub alt_selected: AtomicBool,

    // ABI 7: the TAP-mode send signal (WING `/ch/n/tapwid`, `/ch/n/peq`).
    /// TAP WIDTH (input channels, -150..150 %): the stereo width of the
    /// signal TAP-mode sends take (stereo channels; negative swaps L and R).
    pub tap_width: AtomicF32,
    /// TAP EQ (input channels): a 3-band EQ on the TAP-mode send signal.
    pub tap_eq: TapEqParams,
}

/// One band of the TAP EQ.
#[repr(C)]
#[derive(Debug)]
pub struct TapEqBand {
    /// -15..15 dB.
    pub gain_db: AtomicF32,
    /// 20..20000 Hz.
    pub freq: AtomicF32,
    /// 0.44..10.
    pub q: AtomicF32,
}

/// TAP EQ (the WING's PRE EQ, `/ch/n/peq`): band 1 a low shelf, band 2 a
/// peak, band 3 a high shelf.
#[repr(C)]
#[derive(Debug)]
pub struct TapEqParams {
    pub on: AtomicBool,
    pub bands: [TapEqBand; TAP_EQ_BANDS],
}

pub const TAP_EQ_BANDS: usize = 3;
/// Default frequency and Q of the TAP EQ bands: 100 Hz, 1 kHz and 10 kHz at Q
/// 1.00, as a factory-set console reports them (on the parameters' steps).
pub const TAP_EQ_DEFAULTS: [(f32, f32); TAP_EQ_BANDS] = [(99.68543, 0.9979702), (999.2505, 0.9979702), (10016.53, 0.9979702)];
pub const TAP_EQ_GAIN_RANGE: f32 = 15.0;
pub const TAP_EQ_Q_RANGE: (f32, f32) = (0.442, 10.0);
pub const TAP_WIDTH_RANGE: f32 = 150.0;

impl Default for TapEqParams {
    fn default() -> Self {
        let band = |i: usize| TapEqBand {
            gain_db: AtomicF32::new(0.0),
            freq: AtomicF32::new(TAP_EQ_DEFAULTS[i].0),
            q: AtomicF32::new(TAP_EQ_DEFAULTS[i].1),
        };
        Self { on: AtomicBool::new(false), bands: std::array::from_fn(band) }
    }
}

impl TapEqParams {
    /// Back to the defaults (off, flat).
    pub fn reset(&self) {
        self.on.store(false, Relaxed);
        for (b, (f, q)) in self.bands.iter().zip(TAP_EQ_DEFAULTS) {
            b.gain_db.store(0.0);
            b.freq.store(f);
            b.q.store(q);
        }
    }
}

/// The WING's 18 strip colours (index 1..18 in `colour`): the console's names
/// and the RGB it draws. 1-12 are measured on the console's screen (firmware
/// 1.10 and 3.1 screenshots), as are 15 and 16; 13, 14, 17 and 18 are
/// estimates in the same style.
pub const WING_COLOURS: [(&str, u32); 18] = [
    ("Gray Blue", 0x4074c8),
    ("Medium Blue", 0x00a4f8),
    ("Dark Blue", 0x4000f8),
    ("Turquoise", 0x00d0d0),
    ("Green", 0x00bc00),
    ("Olive Green", 0x809800),
    ("Yellow", 0xe0d400),
    ("Brown", 0xb86018),
    ("Red", 0xe00040),
    ("Coral", 0xf85c58),
    ("Magenta", 0xd800f8),
    ("Purple", 0x9800f8),
    ("Orange", 0xf8a008),
    ("Light Blue", 0x68c8f8),
    ("Salmon", 0xf05c78),
    ("Teal", 0x0088a0),
    ("Dark Gray", 0x585858),
    ("Light Gray", 0xb0b0b0),
];

/// The RGB values earlier versions used for the same 18 colours. Shows saved
/// with ARGB colours (legacy files) used these, so they map back exactly.
const PREVIOUS_RGB: [u32; 18] = [
    0x3a6fe8, 0x4fa6f7, 0x5a4fe0, 0x2ec6c6, 0x2fb84a, 0x9acd32, 0xf2d230, 0xb07a3a, 0xe83a3a, 0xf07c6c, 0xe84aa8, 0x9b4fe0,
    0xf0a030, 0x7ec8f0, 0xf06030, 0x6ee0b0, 0x9a9a9a, 0xf0f0f0,
];

/// RGB of WING colour `index` (1..18; anything else is gray).
pub fn colour_rgb(index: i32) -> u32 {
    usize::try_from(index - 1).ok().and_then(|i| WING_COLOURS.get(i)).map_or(0x9a9a9a, |c| c.1)
}

/// The WING colour (1..18) closest to an RGB value (against the console's
/// colours and the values earlier versions saved).
pub fn nearest_colour(rgb: u32) -> i32 {
    let split = |c: u32| [(c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff].map(|v| v as i32);
    let target = split(rgb);
    let distance = |c: u32| split(c).iter().zip(&target).map(|(a, b)| (a - b) * (a - b)).sum::<i32>();
    let best = WING_COLOURS
        .iter()
        .zip(PREVIOUS_RGB)
        .enumerate()
        .min_by_key(|(_, (c, previous))| distance(c.1).min(distance(*previous)))
        .map_or(0, |(i, _)| i);
    best as i32 + 1
}

#[cfg(test)]
mod colour_tests {
    use super::*;

    #[test]
    fn colours_map_back_to_their_index() {
        for (i, (c, previous)) in WING_COLOURS.iter().zip(PREVIOUS_RGB).enumerate() {
            assert_eq!(colour_rgb(i as i32 + 1), c.1);
            assert_eq!(nearest_colour(c.1), i as i32 + 1, "{}", c.0);
            assert_eq!(nearest_colour(previous), i as i32 + 1, "previous {}", c.0);
        }
        assert_eq!(colour_rgb(0), 0x9a9a9a);
        assert_eq!(nearest_colour(0xff0000), 9); // pure red is the WING's Red
    }
}

#[cfg(test)]
mod factory_tests {
    use super::*;

    #[test]
    fn dynamics_slots_start_as_a_factory_set_console() {
        // Channels: COMP in the dynamics slot; aux: the WING GATE at -36 dB.
        let ch = StripParams::new(StripKind::Channel);
        assert_eq!(DYN_MODELS[ch.dynamics.model_index()].token, "COMP");
        let aux = StripParams::new(StripKind::Aux);
        assert_eq!(DYN_MODELS[aux.dynamics.model_index()].token, "GATE");
        assert_eq!(aux.dynamics.param("thr").unwrap().load(), AUX_GATE_THRESHOLD_DB);
        // Key filters 1k0 Hz / Q 2.00, the crossover 1k0 Hz / Q 0.40 / DEPTH 6 dB.
        let s = &ch.gate;
        assert_eq!((s.sc_freq.load(), s.sc_q.load()), (1002.374, 1.995882));
        let s = &ch.dynamics;
        assert_eq!((s.xo_freq.load(), s.xo_q.load(), s.xo_depth.load()), (1002.374, 0.4, 6.0));
        assert_eq!(ch.tap_eq.bands[2].q.load(), 0.9979702);
    }
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
            ignore_mute: AtomicBool::new(false),
            pan_link: AtomicBool::new(true),
            pan_reversed: AtomicBool::new(false),
            level_db: AtomicF32::new(0.0),
            pan: AtomicF32::new(0.0),
        };
        let p = Self {
            input: InputParams::default(),
            filter: FilterParams::default(),
            gate: DynSlotParams::new("GATE"),
            eq: EqBlockParams::new(output),
            dynamics: DynSlotParams::new(if kind == StripKind::Aux { "GATE" } else { "COMP" }),
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
            alt_input_left: AtomicI32::new(-1),
            alt_input_right: AtomicI32::new(-1),
            alt_selected: AtomicBool::new(false),
            tap_width: AtomicF32::new(100.0),
            tap_eq: TapEqParams::default(),
        };
        p.eq.on.store(true, Relaxed);
        if kind == StripKind::Aux {
            if let Some(thr) = p.dynamics.param("thr") {
                thr.store(AUX_GATE_THRESHOLD_DB);
            }
        }
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
    /// After trim, balance and polarity (the INPUT screen's "post TRIM" meter).
    pub trim_peak: [AtomicF32; 2],
    /// Pre-fader, at the PFL point (the METERS screen).
    pub pfl_peak: [AtomicF32; 2],
    /// ABI 9: the GATE / DYNAMICS slot's detector level in the latest block,
    /// dB on its static curve's input scale (the transfer graph's dot; -120
    /// when off).
    pub gate_key_db: AtomicF32,
    pub dyn_key_db: AtomicF32,
    /// ABI 9: the slots' output peaks (max since the last read; linear).
    pub gate_out_peak: AtomicF32,
    pub dyn_out_peak: AtomicF32,
}

/// A DCA group.
#[repr(C)]
#[derive(Debug)]
pub struct DcaParams {
    pub fader_db: AtomicF32,
    pub mute: AtomicBool,
    pub solo: AtomicBool,
    /// Scribble strip light.
    pub led: AtomicBool,
    pub colour: AtomicI32,
    /// WING icon number on the scribble strip (0 = none).
    pub icon: AtomicI32,
}

impl Default for DcaParams {
    fn default() -> Self {
        Self {
            fader_db: AtomicF32::new(0.0),
            mute: AtomicBool::new(false),
            solo: AtomicBool::new(false),
            led: AtomicBool::new(true),
            colour: AtomicI32::new(7),
            icon: AtomicI32::new(0),
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

/// MAIN LINK choices (SETUP > PREFERENCES): Main 1's fader also moves Main 2
/// (1>2), Mains 2-3 (1>2-3) or Mains 2-4 (1>2-4), like a DCA.
pub const MAIN_LINK_MODES: [&str; 4] = ["OFF", "1 > 2", "1 > 2-3", "1 > 2-4"];

/// Default monitor DIM amount in dB.
pub const DEFAULT_DIM_DB: f32 = 20.0;

/// A monitor path's SOURCE choices (`MonitorSourceParams::source`): what the
/// path plays without a solo: MAIN.1-4, MTX.1-8, BUS.1-16, AUX.1-8.
pub const NUM_MONITOR_SOURCES: usize = NUM_MAINS + NUM_MATRICES + WING_BUSES + NUM_AUX;

/// The strip a monitor SOURCE index plays: (kind, 0-based number; buses by slot).
pub fn monitor_source(index: i32) -> Option<(StripKind, usize)> {
    let mut i = usize::try_from(index).ok()?;
    for (kind, count) in
        [(StripKind::Main, NUM_MAINS), (StripKind::Matrix, NUM_MATRICES), (StripKind::Bus, WING_BUSES), (StripKind::Aux, NUM_AUX)]
    {
        if i < count {
            return Some((kind, i));
        }
        i -= count;
    }
    None
}

/// The display name of a monitor SOURCE index ("MAIN.1", "BUS.12").
pub fn monitor_source_name(index: i32) -> String {
    match monitor_source(index) {
        Some((StripKind::Matrix, i)) => format!("MTX.{}", i + 1),
        Some((kind, i)) => format!("{}.{}", kind.label(), i + 1),
        None => "OFF".into(),
    }
}

/// SRC MIX default: the source is silent while soloing (-oo).
pub const SOURCE_MIX_OFF_DB: f32 = -144.0;

/// What a monitor path plays besides the solo bus (MONITORS: SOURCE, SRC
/// LEVEL, SRC MIX, PFL DIM).
#[repr(C)]
#[derive(Debug)]
pub struct MonitorSourceParams {
    /// Index of the SOURCE (see [`monitor_source`]); default MAIN.1.
    pub source: AtomicI32,
    /// SRC LEVEL: the source's level in dB.
    pub source_level_db: AtomicF32,
    /// SRC MIX: the source's level while a solo reaches the path (-144 = off).
    pub source_mix_db: AtomicF32,
    /// PFL DIM: attenuation of pre-fader solos in dB (>= 0).
    pub pfl_dim_db: AtomicF32,
}

impl Default for MonitorSourceParams {
    fn default() -> Self {
        Self {
            source: AtomicI32::new(0),
            source_level_db: AtomicF32::new(0.0),
            source_mix_db: AtomicF32::new(SOURCE_MIX_OFF_DB),
            pfl_dim_db: AtomicF32::new(0.0),
        }
    }
}

impl MonitorSourceParams {
    fn reset(&self) {
        let d = Self::default();
        self.source.store(d.source.load(Relaxed), Relaxed);
        self.source_level_db.store(d.source_level_db.load());
        self.source_mix_db.store(d.source_mix_db.load());
        self.pfl_dim_db.store(d.pfl_dim_db.load());
    }
}

/// The PHONES monitor path: its source, level, DIM amount, outputs and meter.
/// The SPEAKERS path is the console's monitor (`monitor_level_db`, ...).
#[repr(C)]
#[derive(Debug)]
pub struct MonitorPathParams {
    pub source: MonitorSourceParams,
    pub level_db: AtomicF32,
    /// DIM amount in dB (an attenuation, >= 0).
    pub dim_db: AtomicF32,
    /// Hardware outputs (-1 = none).
    pub output_left: AtomicI32,
    pub output_right: AtomicI32,
    pub peak: [AtomicF32; 2],
}

impl Default for MonitorPathParams {
    fn default() -> Self {
        Self {
            source: MonitorSourceParams::default(),
            level_db: AtomicF32::new(0.0),
            dim_db: AtomicF32::new(DEFAULT_DIM_DB),
            output_left: AtomicI32::new(-1),
            output_right: AtomicI32::new(-1),
            peak: Default::default(),
        }
    }
}

impl MonitorPathParams {
    fn reset(&self) {
        self.source.reset();
        self.level_db.store(0.0);
        self.dim_db.store(DEFAULT_DIM_DB);
        self.output_left.store(-1, Relaxed);
        self.output_right.store(-1, Relaxed);
    }
}

/// Engine-wide controls and status, shared with the UI.
#[repr(C)]
#[derive(Debug)]
pub struct EngineControls {
    /// CHANNEL SOLO (input and aux channels): AFL instead of PFL.
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

    /// BUS / MAIN / MATRIX SOLO: AFL (the default) instead of PFL.
    pub bus_solo_afl: AtomicBool,
    pub main_solo_afl: AtomicBool,
    pub matrix_solo_afl: AtomicBool,
    /// Monitor keys: DIM attenuates by `monitor_dim_db`, MUTE (hold DIM)
    /// silences, MONO sums L and R, SWAP (hold MONO) swaps them.
    pub monitor_dim: AtomicBool,
    pub monitor_mute: AtomicBool,
    pub monitor_mono: AtomicBool,
    pub monitor_swap: AtomicBool,
    /// DCA MUTEGROUPS: a muted DCA also mutes its strips.
    pub dca_mute_groups: AtomicBool,
    /// STARTUP MAIN MUTE: the application mutes the mains when it starts.
    pub startup_main_mute: AtomicBool,
    /// Automix X ENABLE / Y ENABLE.
    pub automix_enable: [AtomicBool; 2],
    /// DIM amount in dB (an attenuation, >= 0).
    pub monitor_dim_db: AtomicF32,
    /// Index into [`MAIN_LINK_MODES`].
    pub main_link: AtomicI32,
    /// INPUT SELECT: true = every input strip follows `global_alt` (the
    /// SETUP > AUDIO MAIN / ALT switch), false = each strip's `alt_selected`.
    pub input_select_global: AtomicBool,
    /// The global MAIN / ALT switch (true = ALT).
    pub global_alt: AtomicBool,

    // ---- console level, ABI 7: solo modes, talkback, monitor paths -----------
    /// SOLO MODE: `solo::solo_mode` (LIVE, STUDIO, SIP).
    pub solo_mode: AtomicI32,
    /// The solo bus / selected monitoring path (MONITORS: PHONES, SPEAKERS,
    /// PH+SPK) as a `MONITOR_BUSES` choice (A = PHONES, B = SPEAKERS, A+B).
    /// STUDIO and SIP solo to it; DIM, MUTE, MONO, SWAP and the talk MON DIM
    /// act on it.
    pub solo_bus: AtomicI32,
    /// LIVE: a path picked on MONITORS while soloing overrides the strips'
    /// own choice until no solo is left (the engine then clears it).
    pub solo_bus_override: AtomicBool,
    /// MUTEGROUP/SIP OVERRIDE (SETUP > PREFERENCES).
    pub mute_override: AtomicBool,
    /// Status: SIP is muting the channels that aren't soloed (the SIP badge).
    pub sip_active: AtomicBool,
    /// Talkback (`talk.rs`).
    pub talk: TalkParams,
    /// The SPEAKERS path: SOURCE, SRC LEVEL, SRC MIX, PFL DIM (its
    /// level, DIM amount, outputs and meter are the `monitor_*` fields).
    pub speakers: MonitorSourceParams,
    /// The PHONES path.
    pub phones: MonitorPathParams,

    // ---- ABI 8: monitor path processing and SOURCE SOLO (`monitor.rs`) --------
    /// Both paths' DIRECT IN, BAND SOLO, EQ, PAN / WIDTH, INVERT, DELAY and
    /// LIMITER, and SOURCE SOLO.
    pub monitor: MonitorControls,
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
            bus_solo_afl: AtomicBool::new(true),
            main_solo_afl: AtomicBool::new(true),
            matrix_solo_afl: AtomicBool::new(true),
            monitor_dim: AtomicBool::new(false),
            monitor_mute: AtomicBool::new(false),
            monitor_mono: AtomicBool::new(false),
            monitor_swap: AtomicBool::new(false),
            dca_mute_groups: AtomicBool::new(true),
            startup_main_mute: AtomicBool::new(false),
            automix_enable: [AtomicBool::new(true), AtomicBool::new(true)],
            monitor_dim_db: AtomicF32::new(DEFAULT_DIM_DB),
            main_link: AtomicI32::new(0),
            input_select_global: AtomicBool::new(false),
            global_alt: AtomicBool::new(false),
            solo_mode: AtomicI32::new(solo_mode::STUDIO),
            solo_bus: AtomicI32::new(2),
            solo_bus_override: AtomicBool::new(false),
            mute_override: AtomicBool::new(false),
            sip_active: AtomicBool::new(false),
            talk: TalkParams::default(),
            speakers: MonitorSourceParams::default(),
            phones: MonitorPathParams::default(),
            monitor: MonitorControls::default(),
        }
    }
}

impl EngineControls {
    /// Solo on a strip of `kind` listens after the fader (AFL), else before it (PFL).
    pub fn solo_is_afl(&self, kind: StripKind) -> bool {
        match kind {
            StripKind::Channel | StripKind::Aux => &self.solo_afl,
            StripKind::Bus => &self.bus_solo_afl,
            StripKind::Main => &self.main_solo_afl,
            StripKind::Matrix => &self.matrix_solo_afl,
        }
        .load(Relaxed)
    }

    /// Main 2-4 linked to Main 1's fader (MAIN LINK): the highest linked main index.
    pub fn main_link_last(&self) -> usize {
        self.main_link.load(Relaxed).clamp(0, MAIN_LINK_MODES.len() as i32 - 1) as usize
    }

    /// Resets the console-wide settings (not the status counters).
    pub fn reset(&self) {
        self.solo_afl.store(false, Relaxed);
        for afl in [&self.bus_solo_afl, &self.main_solo_afl, &self.matrix_solo_afl] {
            afl.store(true, Relaxed);
        }
        for key in [&self.monitor_dim, &self.monitor_mute, &self.monitor_mono, &self.monitor_swap] {
            key.store(false, Relaxed);
        }
        self.monitor_dim_db.store(DEFAULT_DIM_DB);
        self.dca_mute_groups.store(true, Relaxed);
        self.startup_main_mute.store(false, Relaxed);
        for enable in &self.automix_enable {
            enable.store(true, Relaxed);
        }
        self.main_link.store(0, Relaxed);
        self.input_select_global.store(false, Relaxed);
        self.global_alt.store(false, Relaxed);
        self.solo_mode.store(solo_mode::STUDIO, Relaxed);
        self.solo_bus.store(2, Relaxed);
        self.solo_bus_override.store(false, Relaxed);
        self.mute_override.store(false, Relaxed);
        self.talk.reset();
        self.speakers.reset();
        self.phones.reset();
        self.monitor.reset();
        self.monitor_level_db.store(0.0);
        self.mute_groups.store(0, Relaxed);
        for d in &self.dcas {
            d.fader_db.store(0.0);
            d.mute.store(false, Relaxed);
            d.solo.store(false, Relaxed);
            d.led.store(true, Relaxed);
            d.colour.store(7, Relaxed);
            d.icon.store(0, Relaxed);
        }
        for f in &self.fx {
            f.bypass.store(false, Relaxed);
            f.mix.store(100.0);
            f.set_model(FX_NONE, true);
        }
    }
}
