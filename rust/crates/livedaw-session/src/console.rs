//! Console state per section: capture/recall for scenes and snippets, and the
//! full console (structure, DCAs, mute groups, FX slots) for show files.
//!
//! Strips are stored WING-style: one element per processing block, model
//! parameters by their console id (`<P n="thr" v="-40"/>`), so a show stays
//! readable when models gain parameters.

use crate::refs::StripRef;
use crate::xml::{fmt_f32, parse, Element};
use livedaw_dsp::wing::{model_index, ModelSpec, DYN_MODELS, EQ_MODELS_BUS, EQ_MODELS_CH, FILTER_TOOLS, FX_MODELS};
use livedaw_engine::analyser::RtaViewSettings;
use livedaw_engine::atomic::AtomicF32;
use livedaw_engine::params::{
    DynSlotParams, EqBlockParams, InsertParams, StripParams, FX_EXT, FX_NONE, NUM_DCAS, NUM_FX, NUM_MUTE_GROUPS, WING_BUSES,
};
use livedaw_engine::{Engine, Strip, StripKind, MAX_BUSES};
use std::collections::HashMap;
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicU32, Ordering::Relaxed};
use std::sync::Arc;

pub const CONSOLE_VERSION: i32 = 3;

// ---- sections -------------------------------------------------------------------

/// Parameter groups of a strip. Scenes and snippets recall a chosen set (their scope).
pub mod section {
    pub const NAME: u32 = 1 << 0; // name, colour, icon, scribble light
    pub const ROUTING: u32 = 1 << 1; // CONN: input patch, ALT input, mono/stereo, outputs, monitor bus (+ monitor setup, solo PFL/AFL, INPUT SELECT)
    pub const PREAMP: u32 = 1 << 2; // INPUT block (trim, balance, polarity, delay) and FILTER
    pub const GATE: u32 = 1 << 3;
    pub const EQ: u32 = 1 << 4;
    pub const DYNAMICS: u32 = 1 << 5;
    /// Older name of [`DYNAMICS`].
    pub const COMP: u32 = DYNAMICS;
    pub const INSERTS: u32 = 1 << 6; // insert points, process order, tap (+ FX slot states, automix X/Y enables)
    pub const SENDS: u32 = 1 << 7; // bus, main and matrix sends, matrix direct input
    pub const FADER: u32 = 1 << 8; // fader, pan, width, mute (+ DCA levels)
    pub const GROUPS: u32 = 1 << 9; // DCA and mute group membership (+ mute group states)
    pub const ALL: u32 = 0x3ff;
}

// ---- plugins (hosted by the C++ side) ---------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum InsertMode {
    /// Scene/snippet recall: only restore state into the same processor already loaded.
    StateOnly,
    /// Show load: create the processor.
    Instantiate,
}

pub struct CapturedInsert {
    /// The plugin description as an XML element (`<PLUGIN .../>`).
    pub description: String,
    /// Plugin state, base64.
    pub state: String,
}

#[derive(Debug, Clone, PartialEq)]
pub enum RestoreError {
    /// StateOnly: a different plugin (or none) is loaded in the slot.
    Mismatch,
    /// The plugin could not be created or restored.
    Failed(String),
}

/// The FX slots' processors live in the host application; the session reaches
/// them through this.
pub trait PluginBridge {
    fn capture_fx(&mut self, slot: usize) -> Option<CapturedInsert>;
    fn restore_fx(&mut self, slot: usize, mode: InsertMode, description: &str, state: &str) -> Result<(), RestoreError>;
    /// Show load: empties a slot (before restoring the saved processors).
    fn clear_fx(&mut self, _slot: usize) {}
}

/// For sessions without plugin hosting (tests, safe tools).
pub struct NoPlugins;

impl PluginBridge for NoPlugins {
    fn capture_fx(&mut self, _: usize) -> Option<CapturedInsert> {
        None
    }
    fn restore_fx(&mut self, _: usize, _: InsertMode, _: &str, _: &str) -> Result<(), RestoreError> {
        Err(RestoreError::Failed("plugin hosting is not available".into()))
    }
}

// ---- results and fades -------------------------------------------------------------

#[derive(Debug, Default, Clone, PartialEq)]
pub struct ApplyResult {
    pub strips_applied: i32,
    pub strips_skipped_safe: i32,
    pub strips_missing: i32,
    pub warnings: Vec<String>,
}

impl ApplyResult {
    pub fn merge(&mut self, other: ApplyResult) {
        self.strips_applied += other.strips_applied;
        self.strips_skipped_safe += other.strips_skipped_safe;
        self.strips_missing += other.strips_missing;
        self.warnings.extend(other.warnings);
    }
}

/// A continuous value that glides on recall. Holds the strip alive, so a strip
/// removed during a fade is never written through a dangling pointer.
#[derive(Clone)]
pub enum FadeParam {
    Fader(Arc<Strip>),
    Pan(Arc<Strip>),
    SendLevel(Arc<Strip>, usize),
    MainLevel(Arc<Strip>, usize),
    MatrixLevel(Arc<Strip>, usize),
}

impl FadeParam {
    pub fn value(&self) -> Option<&AtomicF32> {
        match self {
            FadeParam::Fader(s) => Some(&s.params.fader_db),
            FadeParam::Pan(s) => Some(&s.params.pan),
            FadeParam::SendLevel(s, slot) => s.sends.find(*slot).map(|p| &p.level_db),
            FadeParam::MainLevel(s, i) => s.params.mains.get(*i).map(|m| &m.level_db),
            FadeParam::MatrixLevel(s, i) => s.params.matrix.get(*i).map(|m| &m.level_db),
        }
    }

    pub fn same(&self, other: &FadeParam) -> bool {
        match (self, other) {
            (FadeParam::Fader(a), FadeParam::Fader(b)) | (FadeParam::Pan(a), FadeParam::Pan(b)) => Arc::ptr_eq(a, b),
            (FadeParam::SendLevel(a, x), FadeParam::SendLevel(b, y))
            | (FadeParam::MainLevel(a, x), FadeParam::MainLevel(b, y))
            | (FadeParam::MatrixLevel(a, x), FadeParam::MatrixLevel(b, y)) => Arc::ptr_eq(a, b) && x == y,
            _ => false,
        }
    }
}

#[derive(Clone)]
pub struct FadeTarget {
    pub param: FadeParam,
    pub target: f32,
}

// ---- attribute helpers --------------------------------------------------------------

fn put_b(e: &mut Element, name: &str, v: &AtomicBool) {
    e.set_bool(name, v.load(Relaxed));
}
fn put_i(e: &mut Element, name: &str, v: &AtomicI32) {
    e.set(name, v.load(Relaxed));
}
fn put_u(e: &mut Element, name: &str, v: &AtomicU32) {
    e.set(name, v.load(Relaxed));
}
fn put_f(e: &mut Element, name: &str, v: &AtomicF32) {
    e.set(name, fmt_f32(v.load()));
}
fn get_b(e: &Element, name: &str, v: &AtomicBool) {
    if e.has_attr(name) {
        v.store(e.bool_or(name, false), Relaxed);
    }
}
fn get_i(e: &Element, name: &str, v: &AtomicI32) {
    if e.has_attr(name) {
        v.store(e.int_or(name, 0), Relaxed);
    }
}
fn get_u(e: &Element, name: &str, v: &AtomicU32) {
    if let Some(s) = e.attr(name) {
        v.store(s.trim().parse::<u32>().unwrap_or(0), Relaxed);
    }
}
fn get_f(e: &Element, name: &str, v: &AtomicF32) {
    if e.has_attr(name) {
        let x = e.double_or(name, 0.0) as f32;
        if x.is_finite() {
            v.store(x);
        }
    }
}

/// Sets a continuous value, or queues it for a fade.
fn get_fadable(e: &Element, name: &str, param: FadeParam, fades: &mut Option<&mut Vec<FadeTarget>>) {
    if !e.has_attr(name) {
        return;
    }
    let target = e.double_or(name, 0.0) as f32;
    if !target.is_finite() {
        return;
    }
    match fades {
        Some(f) => f.push(FadeTarget { param, target }),
        None => {
            if let Some(v) = param.value() {
                v.store(target);
            }
        }
    }
}

/// Writes a model block's parameters as `<P n="id" v="value"/>` children.
fn put_model_params(e: &mut Element, spec: &ModelSpec, values: &[AtomicF32]) {
    for (p, v) in spec.params.iter().zip(values) {
        e.add_new("P").set("n", p.id).set("v", fmt_f32(v.load()));
    }
}

/// Selects the model named by `mdl` (if any) and restores the listed parameters.
fn get_model_params(e: &Element, table: &[ModelSpec], current: usize, values: &[AtomicF32], select: impl FnOnce(usize)) -> usize {
    let model = match e.attr("mdl").and_then(|t| model_index(table, t)) {
        Some(m) => {
            if m != current {
                select(m);
            }
            m
        }
        None => current,
    };
    let Some(spec) = table.get(model) else { return model };
    for p in e.children_named("P") {
        if let (Some(i), Some(v)) = (spec.param_index(p.str_or("n", "")), p.attr("v")) {
            let x = crate::xml::parse_double_prefix(v) as f32;
            values[i].store(spec.params[i].sanitise(x));
        }
    }
    model
}

fn put_dyn_slot(e: &mut Element, slot: &DynSlotParams) {
    let spec = &DYN_MODELS[slot.model_index()];
    put_b(e, "on", &slot.on);
    e.set("mdl", spec.token);
    put_f(e, "mix", &slot.mix);
    put_f(e, "gain", &slot.gain);
    put_i(e, "scf", &slot.sc_filter);
    put_f(e, "scfreq", &slot.sc_freq);
    put_f(e, "scq", &slot.sc_q);
    put_i(e, "scsrc", &slot.sc_source);
    put_i(e, "sctap", &slot.sc_tap);
    put_i(e, "xo", &slot.xo_kind);
    put_f(e, "xof", &slot.xo_freq);
    put_f(e, "xoq", &slot.xo_q);
    put_f(e, "xod", &slot.xo_depth);
    put_model_params(e, spec, &slot.values);
}

fn get_dyn_slot(e: &Element, slot: &DynSlotParams) {
    get_b(e, "on", &slot.on);
    get_f(e, "mix", &slot.mix);
    get_f(e, "gain", &slot.gain);
    get_i(e, "scf", &slot.sc_filter);
    get_f(e, "scfreq", &slot.sc_freq);
    get_f(e, "scq", &slot.sc_q);
    get_i(e, "scsrc", &slot.sc_source);
    get_i(e, "sctap", &slot.sc_tap);
    get_i(e, "xo", &slot.xo_kind);
    get_f(e, "xof", &slot.xo_freq);
    get_f(e, "xoq", &slot.xo_q);
    get_f(e, "xod", &slot.xo_depth);
    get_model_params(e, DYN_MODELS, slot.model_index(), &slot.values, |m| slot.set_model(m));
}

fn put_eq(e: &mut Element, eq: &EqBlockParams, bus: bool) {
    let table = if bus { EQ_MODELS_BUS } else { EQ_MODELS_CH };
    let spec = &table[(eq.model.load(Relaxed).max(0) as usize).min(table.len() - 1)];
    put_b(e, "on", &eq.on);
    e.set("mdl", spec.token);
    put_f(e, "mix", &eq.mix);
    put_model_params(e, spec, &eq.values);
}

fn get_eq(e: &Element, eq: &EqBlockParams, bus: bool) {
    let table = if bus { EQ_MODELS_BUS } else { EQ_MODELS_CH };
    get_b(e, "on", &eq.on);
    get_f(e, "mix", &eq.mix);
    let current = (eq.model.load(Relaxed).max(0) as usize).min(table.len() - 1);
    get_model_params(e, table, current, &eq.values, |m| eq.set_model(m, bus));
}

fn put_insert(e: &mut Element, ins: &InsertParams) {
    put_b(e, "on", &ins.on);
    put_i(e, "fx", &ins.fx);
    put_i(e, "mode", &ins.mode);
    put_f(e, "w", &ins.weight);
}

fn get_insert(e: &Element, ins: &InsertParams) {
    get_b(e, "on", &ins.on);
    get_i(e, "fx", &ins.fx);
    get_i(e, "mode", &ins.mode);
    get_f(e, "w", &ins.weight);
}

// ---- strip lookup -------------------------------------------------------------------

/// All strips of the console, indexed by reference, for one capture/apply pass.
pub struct StripIndex {
    pub strips: Vec<Arc<Strip>>,
    by_ref: HashMap<StripRef, Arc<Strip>>,
}

impl StripIndex {
    pub fn new(engine: &Engine) -> Self {
        let mut strips = Vec::new();
        for kind in StripKind::ALL {
            strips.extend(engine.strips(kind));
        }
        let by_ref = strips.iter().map(|s| (StripRef::of(s), s.clone())).collect();
        Self { strips, by_ref }
    }

    pub fn find(&self, r: &StripRef) -> Option<&Arc<Strip>> {
        self.by_ref.get(r)
    }

    fn bus_slots(&self) -> Vec<usize> {
        self.strips.iter().filter(|s| s.kind() == StripKind::Bus).map(|b| b.id() as usize).collect()
    }
}

/// Finds a strip by reference (message thread).
pub fn find_strip(engine: &Engine, r: &StripRef) -> Option<Arc<Strip>> {
    match r.kind {
        StripKind::Channel => engine.find_channel(r.id),
        StripKind::Bus => engine.bus_in_slot(r.id),
        kind => engine.strip_at(kind, usize::try_from(r.id).ok()?),
    }
}

fn contains(only: Option<&[StripRef]>, r: &StripRef) -> bool {
    only.is_none_or(|o| o.contains(r))
}

// ---- capture --------------------------------------------------------------------------

fn capture_strip(s: &Strip, sections: u32, slots: &[usize]) -> Element {
    use section::*;
    let p = &s.params;
    let kind = s.kind();
    let mut e = Element::new("Strip");
    e.set("ref", StripRef::of(s)).set("sections", sections);

    if sections & NAME != 0 {
        e.set("name", s.name());
        put_i(&mut e, "colour", &p.colour);
        put_i(&mut e, "icon", &p.icon);
        put_b(&mut e, "led", &p.led);
    }
    if sections & ROUTING != 0 {
        let r = e.add_new("Route");
        put_b(r, "stereo", &p.stereo);
        put_i(r, "inL", &p.input_left);
        put_i(r, "inR", &p.input_right);
        put_i(r, "outL", &p.output_left);
        put_i(r, "outR", &p.output_right);
        put_i(r, "mon", &p.monitor_bus);
        put_b(r, "solosafe", &p.solo_safe);
        put_b(r, "mono", &p.mono);
        if kind.is_input() {
            put_i(r, "altL", &p.alt_input_left);
            put_i(r, "altR", &p.alt_input_right);
            put_b(r, "altSel", &p.alt_selected);
        }
    }
    if sections & PREAMP != 0 {
        let i = e.add_new("In");
        put_f(i, "trim", &p.input.trim_db);
        put_f(i, "bal", &p.input.balance_db);
        put_b(i, "inv", &p.input.invert);
        put_b(i, "dlyon", &p.input.delay_on);
        put_i(i, "dlymode", &p.input.delay_mode);
        put_f(i, "dly", &p.input.delay);
        if kind == StripKind::Channel {
            let f = e.add_new("Flt");
            let fp = &p.filter;
            put_b(f, "lc", &fp.low_cut);
            put_f(f, "lcf", &fp.low_cut_freq);
            put_i(f, "lcs", &fp.low_cut_slope);
            put_b(f, "hc", &fp.high_cut);
            put_f(f, "hcf", &fp.high_cut_freq);
            put_i(f, "hcs", &fp.high_cut_slope);
            put_b(f, "tf", &fp.tool);
            let tool = (fp.tool_model.load(Relaxed).max(0) as usize).min(FILTER_TOOLS.len() - 1);
            f.set("mdl", FILTER_TOOLS[tool].token);
            put_model_params(f, &FILTER_TOOLS[tool], &fp.tool_params);
        }
    }
    if sections & GATE != 0 && kind == StripKind::Channel {
        put_dyn_slot(e.add_new("Gate"), &p.gate);
    }
    if sections & EQ != 0 {
        put_eq(e.add_new("Eq"), &p.eq, kind.is_output());
    }
    if sections & DYNAMICS != 0 {
        put_dyn_slot(e.add_new("Dyn"), &p.dynamics);
    }
    if sections & INSERTS != 0 {
        let ins = e.add_new("Ins");
        put_i(ins, "proc", &p.process_order);
        put_i(ins, "tap", &p.tap);
        put_insert(ins.add_new("Pre"), &p.pre_insert);
        put_insert(ins.add_new("Post"), &p.post_insert);
    }
    if sections & SENDS != 0 {
        if kind != StripKind::Matrix {
            let m = e.add_new("Mtx");
            for (i, ms) in p.matrix.iter().enumerate() {
                let x = m.add_new("S");
                x.set("i", i);
                put_b(x, "on", &ms.on);
                put_b(x, "pre", &ms.pre);
                put_b(x, "ignmute", &ms.ignore_mute);
                put_f(x, "lvl", &ms.level_db);
                put_f(x, "pan", &ms.pan);
            }
        }
        if !matches!(kind, StripKind::Main | StripKind::Matrix) {
            let m = e.add_new("Mains");
            for (i, ms) in p.mains.iter().enumerate() {
                let x = m.add_new("S");
                x.set("i", i);
                put_b(x, "on", &ms.on);
                put_b(x, "pre", &ms.pre);
                put_f(x, "lvl", &ms.level_db);
            }
            let sends = e.add_new("Sends");
            for &slot in slots {
                if kind == StripKind::Bus && (s.id() as usize) < 8 || kind == StripKind::Bus && slot >= 8 {
                    continue; // buses 9+ feed buses 1-8 only
                }
                let x = sends.add_new("S");
                x.set("slot", slot);
                match s.sends.find(slot) {
                    Some(send) => {
                        put_b(x, "on", &send.enabled);
                        put_f(x, "lvl", &send.level_db);
                        put_i(x, "mode", &send.mode);
                        put_f(x, "pan", &send.pan);
                        put_b(x, "plink", &send.pan_link);
                        put_b(x, "prev", &send.pan_reversed);
                        put_b(x, "ignmute", &send.ignore_mute);
                    }
                    None => {
                        // Never touched: record the defaults so a recall resets it.
                        x.set("on", 0)
                            .set("lvl", 0)
                            .set("mode", 0)
                            .set("pan", 0)
                            .set("plink", 1)
                            .set("prev", 0)
                            .set("ignmute", 0);
                    }
                }
            }
        }
        if kind == StripKind::Channel {
            capture_tap(&mut e, p);
        }
        if kind == StripKind::Matrix {
            let d = e.add_new("Dir");
            put_b(d, "on", &p.direct_on);
            put_b(d, "inv", &p.direct_invert);
            put_b(d, "pre", &p.direct_pre);
            put_i(d, "src", &p.direct_source);
            put_f(d, "lvl", &p.direct_level_db);
        }
    }
    if sections & FADER != 0 {
        let o = e.add_new("Fdr");
        put_f(o, "lvl", &p.fader_db);
        put_f(o, "pan", &p.pan);
        put_f(o, "wid", &p.width);
        put_b(o, "mute", &p.mute);
    }
    if sections & GROUPS != 0 {
        let g = e.add_new("Grp");
        put_u(g, "dca", &p.dca_mask);
        put_u(g, "mgrp", &p.mute_group_mask);
        let tags = s.tags();
        if !tags.is_empty() {
            g.set("tags", tags);
        }
    }
    e
}

/// TAP WIDTH and TAP EQ (input channels; recalled with SENDS):
/// `<Tap wid="100" eq="0"><B i="0" g="0" f="100" q="0.7"/>...</Tap>`.
fn capture_tap(e: &mut Element, p: &StripParams) {
    let t = e.add_new("Tap");
    put_f(t, "wid", &p.tap_width);
    put_b(t, "eq", &p.tap_eq.on);
    for (i, b) in p.tap_eq.bands.iter().enumerate() {
        let x = t.add_new("B");
        x.set("i", i);
        put_f(x, "g", &b.gain_db);
        put_f(x, "f", &b.freq);
        put_f(x, "q", &b.q);
    }
}

/// Shows from before TAP WIDTH / TAP EQ have no `<Tap>`: the strip keeps its values.
fn apply_tap(t: &Element, p: &StripParams) {
    get_f(t, "wid", &p.tap_width);
    get_b(t, "eq", &p.tap_eq.on);
    for x in t.children_named("B") {
        let Some(b) = usize::try_from(x.int_or("i", -1)).ok().and_then(|i| p.tap_eq.bands.get(i)) else { continue };
        get_f(x, "g", &b.gain_db);
        get_f(x, "f", &b.freq);
        get_f(x, "q", &b.q);
    }
}

/// RTA SETTINGS and EQ/RTA SETTINGS, FILTER's RTA SOURCE and the BAND SOLO
/// trim (console settings, kept with the show; scenes don't recall them).
fn capture_rta(engine: &Engine, parent: &mut Element) {
    let c = engine.analysis().controls();
    let r = parent.add_new("Rta");
    put_i(r, "filterTap", &c.filter_tap);
    put_f(r, "bandSoloTrim", &c.band_solo_trim_db);
    for (tag, v) in [("Home", &c.home), ("Eq", &c.eq)] {
        let x = r.add_new(tag);
        put_rta_view(x, v);
    }
}

fn put_rta_view(x: &mut Element, v: &RtaViewSettings) {
    put_i(x, "display", &v.display);
    put_i(x, "colour", &v.colour);
    put_i(x, "tap", &v.tap);
    put_i(x, "range", &v.range);
    put_i(x, "decay", &v.decay);
    put_i(x, "detector", &v.detector);
    put_b(x, "autogain", &v.autogain);
    put_f(x, "gain", &v.gain_db);
}

fn apply_rta(engine: &Engine, parent: &Element) {
    let Some(r) = parent.child("Rta") else { return };
    let c = engine.analysis().controls();
    get_i(r, "filterTap", &c.filter_tap);
    get_f(r, "bandSoloTrim", &c.band_solo_trim_db);
    for (tag, v) in [("Home", &c.home), ("Eq", &c.eq)] {
        if let Some(x) = r.child(tag) {
            get_i(x, "display", &v.display);
            get_i(x, "colour", &v.colour);
            get_i(x, "tap", &v.tap);
            get_i(x, "range", &v.range);
            get_i(x, "decay", &v.decay);
            get_i(x, "detector", &v.detector);
            get_b(x, "autogain", &v.autogain);
            get_f(x, "gain", &v.gain_db);
        }
    }
}

/// IGNORE CHANNEL MUTE. Shows from before the flag had pre-fader sends that
/// never followed the channel mute: such a send (`pre`) loads with the flag on.
fn get_ignore_mute(e: &Element, v: &AtomicBool, pre: bool) {
    v.store(e.bool_or("ignmute", pre), Relaxed);
}

/// The monitor setup (scenes recall it with ROUTING). `keys` adds the monitor
/// keys (DIM, mute, MONO, swap), which only show files keep.
fn capture_monitor(engine: &Engine, parent: &mut Element, keys: bool) {
    let c = &engine.controls;
    let m = parent.add_new("Monitor");
    put_b(m, "afl", &c.solo_afl);
    put_b(m, "aflBus", &c.bus_solo_afl);
    put_b(m, "aflMain", &c.main_solo_afl);
    put_b(m, "aflMtx", &c.matrix_solo_afl);
    put_f(m, "levelDb", &c.monitor_level_db);
    put_f(m, "dimDb", &c.monitor_dim_db);
    put_i(m, "outL", &c.monitor_output_left);
    put_i(m, "outR", &c.monitor_output_right);
    if keys {
        put_b(m, "dim", &c.monitor_dim);
        put_b(m, "mute", &c.monitor_mute);
        put_b(m, "mono", &c.monitor_mono);
        put_b(m, "swap", &c.monitor_swap);
    }
    crate::globals::capture_monitor_paths(engine, m);
}

/// Missing attributes (older shows) leave today's values: shows load into a
/// reset engine (buses, mains and matrices solo AFL, as before per-type solo).
fn apply_monitor(engine: &Engine, parent: &Element) {
    if let Some(m) = parent.child("Monitor") {
        let c = &engine.controls;
        get_b(m, "afl", &c.solo_afl);
        get_b(m, "aflBus", &c.bus_solo_afl);
        get_b(m, "aflMain", &c.main_solo_afl);
        get_b(m, "aflMtx", &c.matrix_solo_afl);
        get_f(m, "levelDb", &c.monitor_level_db);
        get_f(m, "dimDb", &c.monitor_dim_db);
        get_i(m, "outL", &c.monitor_output_left);
        get_i(m, "outR", &c.monitor_output_right);
        get_b(m, "dim", &c.monitor_dim);
        get_b(m, "mute", &c.monitor_mute);
        get_b(m, "mono", &c.monitor_mono);
        get_b(m, "swap", &c.monitor_swap);
        crate::globals::apply_monitor_paths(engine, m);
    }
}

/// INPUT SELECT: global or per strip, and the global MAIN / ALT switch
/// (scenes recall them with ROUTING).
fn capture_input_select(engine: &Engine, parent: &mut Element) {
    let i = parent.add_new("InputSelect");
    put_b(i, "global", &engine.controls.input_select_global);
    put_b(i, "alt", &engine.controls.global_alt);
}

fn apply_input_select(engine: &Engine, parent: &Element) {
    if let Some(i) = parent.child("InputSelect") {
        get_b(i, "global", &engine.controls.input_select_global);
        get_b(i, "alt", &engine.controls.global_alt);
    }
}

/// Automix X/Y ENABLE (scenes recall them with INSERTS).
fn capture_automix(engine: &Engine, parent: &mut Element) {
    let a = parent.add_new("Automix");
    put_b(a, "x", &engine.controls.automix_enable[0]);
    put_b(a, "y", &engine.controls.automix_enable[1]);
}

fn apply_automix(engine: &Engine, parent: &Element) {
    if let Some(a) = parent.child("Automix") {
        get_b(a, "x", &engine.controls.automix_enable[0]);
        get_b(a, "y", &engine.controls.automix_enable[1]);
    }
}

/// SETUP > PREFERENCES kept with the show (scenes don't recall them).
fn capture_prefs(engine: &Engine, parent: &mut Element) {
    let c = &engine.controls;
    let p = parent.add_new("Prefs");
    put_b(p, "dcaMuteGroups", &c.dca_mute_groups);
    put_i(p, "mainLink", &c.main_link);
    put_b(p, "startupMainMute", &c.startup_main_mute);
}

fn apply_prefs(engine: &Engine, parent: &Element) {
    if let Some(p) = parent.child("Prefs") {
        let c = &engine.controls;
        get_b(p, "dcaMuteGroups", &c.dca_mute_groups);
        get_i(p, "mainLink", &c.main_link);
        get_b(p, "startupMainMute", &c.startup_main_mute);
    }
}

fn capture_dcas(engine: &Engine, parent: &mut Element, names: bool) {
    let d = parent.add_new("Dcas");
    for (i, dca) in engine.controls.dcas.iter().enumerate() {
        let x = d.add_new("Dca");
        x.set("i", i);
        if names {
            x.set("name", engine.dca_name(i));
            put_i(x, "colour", &dca.colour);
        }
        put_f(x, "lvl", &dca.fader_db);
        put_b(x, "mute", &dca.mute);
    }
}

fn apply_dcas(engine: &Engine, parent: &Element) {
    let Some(d) = parent.child("Dcas") else { return };
    for x in d.children_named("Dca") {
        let i = x.int_or("i", -1);
        let Some(dca) = engine.controls.dcas.get(usize::try_from(i).unwrap_or(NUM_DCAS)) else { continue };
        if let Some(n) = x.attr("name") {
            engine.set_dca_name(i as usize, n);
        }
        get_i(x, "colour", &dca.colour);
        get_f(x, "lvl", &dca.fader_db);
        get_b(x, "mute", &dca.mute);
    }
}

fn capture_mute_groups(engine: &Engine, parent: &mut Element, names: bool) {
    let g = parent.add_new("MuteGroups");
    put_u(g, "on", &engine.controls.mute_groups);
    if names {
        for i in 0..NUM_MUTE_GROUPS {
            g.add_new("G").set("i", i).set("name", engine.mute_group_name(i));
        }
    }
}

fn apply_mute_groups(engine: &Engine, parent: &Element) {
    let Some(g) = parent.child("MuteGroups") else { return };
    get_u(g, "on", &engine.controls.mute_groups);
    for x in g.children_named("G") {
        if let (Ok(i), Some(n)) = (usize::try_from(x.int_or("i", -1)), x.attr("name")) {
            engine.set_mute_group_name(i, n);
        }
    }
}

fn capture_fx(engine: &Engine, parent: &mut Element, bridge: &mut dyn PluginBridge) {
    let fx = parent.add_new("Fx");
    for i in 0..NUM_FX {
        fx.add(capture_fx_slot(engine, i, bridge));
    }
}

/// One FX slot as a `<Slot i=..>`: its device and parameters, or its plugin
/// (description and state through the bridge).
pub(crate) fn capture_fx_slot(engine: &Engine, i: usize, bridge: &mut dyn PluginBridge) -> Element {
    let mut x = Element::new("Slot");
    x.set("i", i);
    let Some(p) = engine.controls.fx.get(i) else { return x };
    put_b(&mut x, "bypass", &p.bypass);
    put_f(&mut x, "mix", &p.mix);
    let model = p.model_index();
    x.set("mdl", FX_MODELS[model].token);
    if model != FX_EXT {
        put_model_params(&mut x, &FX_MODELS[model], &p.values);
        return x;
    }
    if !engine.fx(i).is_some_and(|r| r.is_loaded()) {
        return x;
    }
    let Some(captured) = bridge.capture_fx(i) else { return x };
    if let Ok(desc) = parse(&captured.description) {
        x.add(desc);
    }
    x.add_new("State").add_text(captured.state);
    x
}

/// Restores the FX slots. Built-in devices are selected and set (creating a
/// device is safe at any time); plugins (EXT) follow `mode`: none in safe mode
/// (`None`), state only for recalls, instantiated for show loads. A recall never
/// removes or creates a plugin.
fn apply_fx(
    engine: &Engine,
    parent: &Element,
    mode: Option<InsertMode>,
    bridge: &mut dyn PluginBridge,
    warnings: &mut Vec<String>,
) {
    let Some(fx) = parent.child("Fx") else { return };
    for x in fx.children_named("Slot") {
        let Ok(i) = usize::try_from(x.int_or("i", -1)) else { continue };
        apply_fx_slot(engine, x, i, mode, bridge, warnings);
    }
}

/// Restores a `<Slot>` element into FX slot `i` (see [`apply_fx`]).
pub(crate) fn apply_fx_slot(
    engine: &Engine,
    x: &Element,
    i: usize,
    mode: Option<InsertMode>,
    bridge: &mut dyn PluginBridge,
    warnings: &mut Vec<String>,
) {
    let (Some(p), Some(rack)) = (engine.controls.fx.get(i), engine.fx(i)) else { return };
    get_b(x, "bypass", &p.bypass);
    get_f(x, "mix", &p.mix);

    // Shows from before the FX rack hold plugins only.
    let saved = match x.attr("mdl") {
        Some(t) => model_index(FX_MODELS, t).unwrap_or(FX_NONE),
        None if x.child("PLUGIN").is_some() => FX_EXT,
        None => FX_NONE,
    };
    let current = p.model_index();
    let recall = mode == Some(InsertMode::StateOnly);

    if saved != FX_EXT {
        if current == FX_EXT && rack.is_loaded() {
            if recall {
                warnings.push(format!("FX {}: a plugin is loaded, {} not recalled", i + 1, FX_MODELS[saved].name));
                return;
            }
            bridge.clear_fx(i);
        }
        get_model_params(x, FX_MODELS, current, &p.values, |m| p.set_model(m, true));
        if !engine.set_fx_model(i, saved, false) {
            warnings.push(format!("FX {}: {} needs a premium slot (FX 1-8)", i + 1, FX_MODELS[saved].name));
            engine.set_fx_model(i, FX_NONE, true);
        }
        return;
    }

    let Some(mode) = mode else { return };
    let Some(desc) = x.child("PLUGIN") else { return };
    let name = desc.str_or("name", "plugin");
    if recall && current != FX_EXT {
        warnings.push(format!("FX {}: {name} isn't loaded, not recalled", i + 1));
        return;
    }
    let state = x.child("State").map(Element::text).unwrap_or_default();
    match bridge.restore_fx(i, mode, &desc.to_compact(), &state) {
        Ok(()) => {}
        Err(RestoreError::Mismatch) => warnings.push(format!("FX {}: expected {name}, not recalled", i + 1)),
        Err(RestoreError::Failed(msg)) => warnings.push(format!("FX {}: {name} ({msg})", i + 1)),
    }
}

/// Captures `sections` of every strip (or only `only`) into a `<State>` element.
/// Console-wide parts (DCAs, mute groups, FX, monitor) are captured when the
/// whole console is in the scope (`only` is None).
pub fn capture(engine: &Engine, sections: u32, only: Option<&[StripRef]>, bridge: &mut dyn PluginBridge) -> Element {
    let index = StripIndex::new(engine);
    let slots = index.bus_slots();
    let mut state = Element::new("State");
    state.set("version", CONSOLE_VERSION).set("sections", sections);

    for s in &index.strips {
        if contains(only, &StripRef::of(s)) {
            state.add(capture_strip(s, sections, &slots));
        }
    }
    if only.is_none() {
        if sections & section::ROUTING != 0 {
            capture_monitor(engine, &mut state, false);
            capture_input_select(engine, &mut state);
        }
        if sections & section::FADER != 0 {
            capture_dcas(engine, &mut state, sections & section::NAME != 0);
        }
        if sections & section::GROUPS != 0 {
            capture_mute_groups(engine, &mut state, sections & section::NAME != 0);
        }
        if sections & section::INSERTS != 0 {
            capture_fx(engine, &mut state, bridge);
            capture_automix(engine, &mut state);
        }
    }
    state
}

// ---- apply ----------------------------------------------------------------------------

fn apply_strip(s: &Arc<Strip>, e: &Element, requested: u32, fades: &mut Option<&mut Vec<FadeTarget>>, result: &mut ApplyResult) {
    use section::*;
    // Only touch what was both captured and requested.
    let sections = requested & (e.int_or("sections", ALL as i32) as u32);
    let p = &s.params;
    let kind = s.kind();

    if sections & NAME != 0 {
        if let Some(n) = e.attr("name") {
            s.set_name(n);
        }
        get_i(e, "colour", &p.colour);
        get_i(e, "icon", &p.icon);
        get_b(e, "led", &p.led);
    }
    if let Some(r) = e.child("Route").filter(|_| sections & ROUTING != 0) {
        if kind.is_input() {
            get_b(r, "stereo", &p.stereo);
        }
        get_i(r, "inL", &p.input_left);
        get_i(r, "inR", &p.input_right);
        get_i(r, "outL", &p.output_left);
        get_i(r, "outR", &p.output_right);
        get_i(r, "mon", &p.monitor_bus);
        get_b(r, "solosafe", &p.solo_safe);
        get_b(r, "mono", &p.mono);
        // Shows from before ALT inputs keep the current ones.
        get_i(r, "altL", &p.alt_input_left);
        get_i(r, "altR", &p.alt_input_right);
        get_b(r, "altSel", &p.alt_selected);
    }
    if sections & PREAMP != 0 {
        if let Some(i) = e.child("In") {
            get_f(i, "trim", &p.input.trim_db);
            get_f(i, "bal", &p.input.balance_db);
            get_b(i, "inv", &p.input.invert);
            get_b(i, "dlyon", &p.input.delay_on);
            get_i(i, "dlymode", &p.input.delay_mode);
            get_f(i, "dly", &p.input.delay);
        }
        if let Some(f) = e.child("Flt") {
            let fp = &p.filter;
            get_b(f, "lc", &fp.low_cut);
            get_f(f, "lcf", &fp.low_cut_freq);
            get_i(f, "lcs", &fp.low_cut_slope);
            get_b(f, "hc", &fp.high_cut);
            get_f(f, "hcf", &fp.high_cut_freq);
            get_i(f, "hcs", &fp.high_cut_slope);
            get_b(f, "tf", &fp.tool);
            let current = fp.tool_model.load(Relaxed).max(0) as usize;
            get_model_params(f, FILTER_TOOLS, current, &fp.tool_params, |m| fp.set_tool_model(m));
        }
    }
    if let Some(g) = e.child("Gate").filter(|_| sections & GATE != 0) {
        get_dyn_slot(g, &p.gate);
    }
    if let Some(q) = e.child("Eq").filter(|_| sections & EQ != 0) {
        get_eq(q, &p.eq, kind.is_output());
    }
    if let Some(d) = e.child("Dyn").filter(|_| sections & DYNAMICS != 0) {
        get_dyn_slot(d, &p.dynamics);
    }
    if let Some(ins) = e.child("Ins").filter(|_| sections & INSERTS != 0) {
        get_i(ins, "proc", &p.process_order);
        get_i(ins, "tap", &p.tap);
        if let Some(x) = ins.child("Pre") {
            get_insert(x, &p.pre_insert);
        }
        if let Some(x) = ins.child("Post") {
            get_insert(x, &p.post_insert);
        }
    }
    if sections & SENDS != 0 {
        if let Some(m) = e.child("Mtx") {
            for x in m.children_named("S") {
                let Ok(i) = usize::try_from(x.int_or("i", -1)) else { continue };
                let Some(ms) = p.matrix.get(i) else { continue };
                get_b(x, "on", &ms.on);
                get_b(x, "pre", &ms.pre);
                get_ignore_mute(x, &ms.ignore_mute, ms.pre.load(Relaxed));
                get_fadable(x, "lvl", FadeParam::MatrixLevel(s.clone(), i), fades);
                get_f(x, "pan", &ms.pan);
            }
        }
        if let Some(m) = e.child("Mains") {
            for x in m.children_named("S") {
                let Ok(i) = usize::try_from(x.int_or("i", -1)) else { continue };
                let Some(ms) = p.mains.get(i) else { continue };
                get_b(x, "on", &ms.on);
                get_b(x, "pre", &ms.pre);
                get_fadable(x, "lvl", FadeParam::MainLevel(s.clone(), i), fades);
            }
        }
        if let Some(sends) = e.child("Sends") {
            for x in sends.children_named("S") {
                let Ok(slot) = usize::try_from(x.int_or("slot", -1)) else { continue };
                if slot >= MAX_BUSES {
                    continue;
                }
                let Some(send) = s.sends.get_or_create(slot) else { continue };
                get_b(x, "on", &send.enabled);
                get_fadable(x, "lvl", FadeParam::SendLevel(s.clone(), slot), fades);
                get_i(x, "mode", &send.mode);
                get_f(x, "pan", &send.pan);
                get_b(x, "plink", &send.pan_link);
                send.pan_reversed.store(x.bool_or("prev", false), Relaxed);
                get_ignore_mute(x, &send.ignore_mute, send.mode.load(Relaxed) == livedaw_engine::sends::send_mode::PRE);
            }
        }
        if let Some(t) = e.child("Tap") {
            apply_tap(t, p);
        }
        if let Some(d) = e.child("Dir") {
            get_b(d, "on", &p.direct_on);
            get_b(d, "inv", &p.direct_invert);
            get_b(d, "pre", &p.direct_pre);
            get_i(d, "src", &p.direct_source);
            get_f(d, "lvl", &p.direct_level_db);
        }
    }
    if let Some(o) = e.child("Fdr").filter(|_| sections & FADER != 0) {
        get_fadable(o, "lvl", FadeParam::Fader(s.clone()), fades);
        get_fadable(o, "pan", FadeParam::Pan(s.clone()), fades);
        get_f(o, "wid", &p.width);
        get_b(o, "mute", &p.mute);
    }
    if let Some(g) = e.child("Grp").filter(|_| sections & GROUPS != 0) {
        get_u(g, "dca", &p.dca_mask);
        get_u(g, "mgrp", &p.mute_group_mask);
        s.set_tags(g.attr("tags").unwrap_or_default());
    }

    result.strips_applied += 1;
}

/// Applies a `<State>` element. Only the intersection of `sections` and what was
/// captured is touched. FX processors are never created or destroyed: their
/// state is recalled only into the same plugin already loaded. With `fades`,
/// continuous values are queued there instead of being set.
pub fn apply(
    engine: &Engine,
    state: &Element,
    sections: u32,
    only: Option<&[StripRef]>,
    respect_recall_safe: bool,
    mut fades: Option<&mut Vec<FadeTarget>>,
    bridge: &mut dyn PluginBridge,
) -> ApplyResult {
    let index = StripIndex::new(engine);
    let mut result = ApplyResult::default();
    let captured = state.int_or("sections", section::ALL as i32) as u32;
    let effective = sections & captured;

    for e in state.children_named("Strip") {
        let r = StripRef::parse(e.str_or("ref", ""));
        if !contains(only, &r) {
            continue;
        }
        let Some(strip) = index.find(&r) else {
            result.strips_missing += 1;
            continue;
        };
        if respect_recall_safe && strip.params.recall_safe.load(Relaxed) {
            result.strips_skipped_safe += 1;
            continue;
        }
        apply_strip(strip, e, sections, &mut fades, &mut result);
    }

    if only.is_none() {
        let main_safe = respect_recall_safe
            && index.find(&StripRef::new(StripKind::Main, 0)).is_some_and(|m| m.params.recall_safe.load(Relaxed));
        if effective & section::ROUTING != 0 && !main_safe {
            apply_monitor(engine, state);
        }
        if effective & section::ROUTING != 0 {
            apply_input_select(engine, state);
        }
        if effective & section::FADER != 0 {
            apply_dcas(engine, state);
        }
        if effective & section::GROUPS != 0 {
            apply_mute_groups(engine, state);
        }
        if effective & section::INSERTS != 0 {
            apply_fx(engine, state, Some(InsertMode::StateOnly), bridge, &mut result.warnings);
            apply_automix(engine, state);
        }
    }
    result
}

// ---- full console (show files) ----------------------------------------------------------

const LISTS: [(StripKind, &str); 5] = [
    (StripKind::Channel, "Channels"),
    (StripKind::Aux, "Aux"),
    (StripKind::Bus, "Buses"),
    (StripKind::Main, "Mains"),
    (StripKind::Matrix, "Matrices"),
];

/// The full console, including structure and FX processors.
pub fn save_console(engine: &Engine, bridge: &mut dyn PluginBridge) -> Element {
    let index = StripIndex::new(engine);
    let slots = index.bus_slots();
    let mut console = Element::new("Console");
    console.set("version", CONSOLE_VERSION);
    put_b(&mut console, "outputProtection", &engine.controls.output_protection);
    put_f(&mut console, "outputCeilingDb", &engine.controls.output_ceiling_db);
    capture_monitor(engine, &mut console, true);
    capture_input_select(engine, &mut console);
    capture_prefs(engine, &mut console);
    capture_rta(engine, &mut console);
    capture_automix(engine, &mut console);
    capture_dcas(engine, &mut console, true);
    capture_mute_groups(engine, &mut console, true);
    capture_fx(engine, &mut console, bridge);

    for (kind, tag) in LISTS {
        let list = console.add_new(tag);
        for s in index.strips.iter().filter(|s| s.kind() == kind) {
            let mut e = capture_strip(s, section::ALL, &slots);
            e.set("id", s.id()).set_bool("recallSafe", s.params.recall_safe.load(Relaxed));
            list.add(e);
        }
    }
    crate::globals::capture(engine, &mut console);
    console
}

/// Rebuilds the console from a `<Console>` element (version 3; older consoles
/// are converted by `legacy` first). With `load_plugins` false (safe mode) no
/// FX processor is instantiated. Returns the warnings.
pub fn load_console(engine: &Engine, console: &Element, bridge: &mut dyn PluginBridge, load_plugins: bool) -> Vec<String> {
    engine.reset();
    let mut result = ApplyResult::default();

    get_b(console, "outputProtection", &engine.controls.output_protection);
    get_f(console, "outputCeilingDb", &engine.controls.output_ceiling_db);
    apply_monitor(engine, console);
    apply_input_select(engine, console);
    apply_prefs(engine, console);
    apply_rta(engine, console);
    apply_automix(engine, console);
    apply_dcas(engine, console);
    apply_mute_groups(engine, console);
    for i in 0..NUM_FX {
        bridge.clear_fx(i);
    }
    apply_fx(engine, console, load_plugins.then_some(InsertMode::Instantiate), bridge, &mut result.warnings);

    // Build the structure first, then apply: creating a bus resets every send
    // to its slot, so sends must only be restored once all buses exist.
    let mut loaded = Vec::new();
    for (kind, tag) in LISTS {
        let Some(list) = console.child(tag) else { continue };
        for e in list.children_named("Strip") {
            let id = e.int_or("id", -1);
            let strip = match kind {
                StripKind::Channel => Some(engine.add_channel(id)),
                StripKind::Bus => {
                    if (0..WING_BUSES as i32).contains(&id) {
                        engine.bus_in_slot(id)
                    } else {
                        engine.add_bus(id)
                    }
                }
                _ => usize::try_from(id).ok().and_then(|i| engine.strip_at(kind, i)),
            };
            match strip {
                Some(s) => loaded.push((s, e)),
                None if kind == StripKind::Bus => result.warnings.push("Bus could not be created (all slots in use)".into()),
                None => result.warnings.push(format!("Unknown {} {} skipped", kind.as_str(), id)),
            }
        }
    }
    for (s, e) in loaded {
        apply_strip(&s, e, section::ALL, &mut None, &mut result);
        s.params.recall_safe.store(e.bool_or("recallSafe", false), Relaxed);
    }
    crate::globals::apply(engine, console);
    result.warnings
}
