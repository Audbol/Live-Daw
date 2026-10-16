//! Console state per section: capture/recall for scenes and snippets, and the
//! full console (structure + plugins) for show files.
//!
//! The XML format is the one Live DAW has always written (`<State>`, `<Strip>`,
//! `<Console>`), so existing show files load unchanged.

use crate::refs::StripRef;
use crate::xml::{fmt_f32, parse, Element};
use livedaw_engine::atomic::AtomicF32;
use livedaw_engine::params::NUM_BANDS;
use livedaw_engine::{Engine, Strip, StripKind, MAX_BUSES, NUM_INSERT_SLOTS};
use std::collections::HashMap;
use std::sync::atomic::{AtomicBool, AtomicI32, Ordering::Relaxed};
use std::sync::Arc;

// ---- sections -------------------------------------------------------------------

/// Parameter groups of a strip. Scenes and snippets recall a chosen set (their scope).
pub mod section {
    pub const NAME: u32 = 1 << 0; // name, colour
    pub const ROUTING: u32 = 1 << 1; // mono/stereo, input patch, outputs, main assign (+ monitor on the master)
    pub const PREAMP: u32 = 1 << 2; // trim, polarity, HPF
    pub const GATE: u32 = 1 << 3;
    pub const EQ: u32 = 1 << 4;
    pub const COMP: u32 = 1 << 5;
    pub const INSERTS: u32 = 1 << 6; // plugin state + bypass
    pub const SENDS: u32 = 1 << 7;
    pub const FADER: u32 = 1 << 8; // fader, pan, mute
    pub const ALL: u32 = 0x1ff;
}

// ---- plugins (hosted by the C++ side) ---------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum InsertMode {
    /// Scene/snippet recall: only restore state into the same plugin already loaded.
    StateOnly,
    /// Show load: create the plugin.
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

/// Plugin hosting lives in the host application; the session reaches it through this.
pub trait PluginBridge {
    fn capture_insert(&mut self, strip: &Strip, slot: usize) -> Option<CapturedInsert>;
    fn restore_insert(
        &mut self,
        strip: &Strip,
        slot: usize,
        mode: InsertMode,
        description: &str,
        state: &str,
    ) -> Result<(), RestoreError>;
}

/// For sessions without plugin hosting (tests, safe tools).
pub struct NoPlugins;

impl PluginBridge for NoPlugins {
    fn capture_insert(&mut self, _: &Strip, _: usize) -> Option<CapturedInsert> {
        None
    }
    fn restore_insert(&mut self, _: &Strip, _: usize, _: InsertMode, _: &str, _: &str) -> Result<(), RestoreError> {
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
}

impl FadeParam {
    pub fn value(&self) -> Option<&AtomicF32> {
        match self {
            FadeParam::Fader(s) => Some(&s.params.fader_db),
            FadeParam::Pan(s) => Some(&s.params.pan),
            FadeParam::SendLevel(s, slot) => s.sends.find(*slot).map(|p| &p.level_db),
        }
    }

    pub fn same(&self, other: &FadeParam) -> bool {
        match (self, other) {
            (FadeParam::Fader(a), FadeParam::Fader(b)) | (FadeParam::Pan(a), FadeParam::Pan(b)) => Arc::ptr_eq(a, b),
            (FadeParam::SendLevel(a, x), FadeParam::SendLevel(b, y)) => Arc::ptr_eq(a, b) && x == y,
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

fn colour_to_string(argb: u32) -> String {
    format!("{argb:08x}")
}

/// Like juce::Colour::fromString: hex digits, anything else ignored.
fn colour_from_string(s: &str) -> u32 {
    s.chars().filter_map(|c| c.to_digit(16)).fold(0u32, |acc, d| acc.wrapping_shl(4) | d)
}

// ---- strip lookup -------------------------------------------------------------------

/// All strips of the console, indexed by reference, for one capture/apply pass.
pub struct StripIndex {
    pub master: Arc<Strip>,
    pub buses: Vec<Arc<Strip>>,
    pub inputs: Vec<Arc<Strip>>,
    by_ref: HashMap<StripRef, Arc<Strip>>,
}

impl StripIndex {
    pub fn new(engine: &Engine) -> Self {
        let master = engine.master();
        let buses = engine.buses();
        let inputs = engine.inputs();
        let mut by_ref = HashMap::with_capacity(inputs.len() + buses.len() + 1);
        for s in std::iter::once(&master).chain(&buses).chain(&inputs) {
            by_ref.insert(StripRef::of(s), s.clone());
        }
        Self { master, buses, inputs, by_ref }
    }

    pub fn find(&self, r: &StripRef) -> Option<&Arc<Strip>> {
        if r.kind == StripKind::Master {
            return Some(&self.master);
        }
        self.by_ref.get(r)
    }

    fn bus_slots(&self) -> Vec<usize> {
        self.buses.iter().map(|b| b.id() as usize).collect()
    }
}

/// Finds a strip by reference (message thread).
pub fn find_strip(engine: &Engine, r: &StripRef) -> Option<Arc<Strip>> {
    match r.kind {
        StripKind::Master => Some(engine.master()),
        StripKind::Aux | StripKind::Group => engine.bus_in_slot(r.id).filter(|b| b.kind() == r.kind),
        StripKind::Input => engine.inputs().into_iter().find(|s| s.id() == r.id),
    }
}

fn contains(only: Option<&[StripRef]>, r: &StripRef) -> bool {
    only.is_none_or(|o| o.contains(r))
}

// ---- capture --------------------------------------------------------------------------

fn capture_strip(s: &Strip, sections: u32, slots: &[usize], bridge: &mut dyn PluginBridge) -> Element {
    use section::*;
    let p = &s.params;
    let mut e = Element::new("Strip");
    e.set("ref", StripRef::of(s)).set("sections", sections as i32);

    if sections & NAME != 0 {
        e.set("name", s.name()).set("colour", colour_to_string(s.colour()));
    }
    if sections & ROUTING != 0 {
        put_b(&mut e, "stereo", &p.stereo);
        put_i(&mut e, "inL", &p.input_left);
        put_i(&mut e, "inR", &p.input_right);
        put_i(&mut e, "outL", &p.output_left);
        put_i(&mut e, "outR", &p.output_right);
        put_b(&mut e, "mainAssign", &p.main_assign);
    }
    if sections & PREAMP != 0 {
        put_f(&mut e, "trimDb", &p.trim_db);
        put_b(&mut e, "polarity", &p.polarity_invert);
        put_b(&mut e, "hpf", &p.hpf_enabled);
        put_f(&mut e, "hpfFreq", &p.hpf_frequency);
    }
    if sections & FADER != 0 {
        put_f(&mut e, "faderDb", &p.fader_db);
        put_f(&mut e, "pan", &p.pan);
        put_b(&mut e, "mute", &p.mute);
    }
    if sections & GATE != 0 {
        let g = e.add_new("Gate");
        put_b(g, "on", &p.gate.enabled);
        put_f(g, "threshold", &p.gate.threshold_db);
        put_f(g, "range", &p.gate.range_db);
        put_f(g, "attack", &p.gate.attack_ms);
        put_f(g, "hold", &p.gate.hold_ms);
        put_f(g, "release", &p.gate.release_ms);
    }
    if sections & EQ != 0 {
        let eq = e.add_new("Eq");
        put_b(eq, "on", &p.eq.enabled);
        for (i, b) in p.eq.bands.iter().enumerate() {
            let band = eq.add_new("Band");
            band.set("index", i);
            put_i(band, "type", &b.band_type);
            put_f(band, "freq", &b.frequency);
            put_f(band, "gain", &b.gain_db);
            put_f(band, "q", &b.q);
            put_b(band, "on", &b.enabled);
        }
    }
    if sections & COMP != 0 {
        let c = e.add_new("Comp");
        put_b(c, "on", &p.comp.enabled);
        put_f(c, "threshold", &p.comp.threshold_db);
        put_f(c, "ratio", &p.comp.ratio);
        put_f(c, "attack", &p.comp.attack_ms);
        put_f(c, "release", &p.comp.release_ms);
        put_f(c, "knee", &p.comp.knee_db);
        put_f(c, "makeup", &p.comp.makeup_db);
    }
    if sections & SENDS != 0 && s.kind() == StripKind::Input {
        let sends = e.add_new("Sends");
        for &slot in slots {
            let se = sends.add_new("Send");
            se.set("slot", slot);
            match s.sends.find(slot) {
                Some(send) => {
                    put_b(se, "on", &send.enabled);
                    put_f(se, "level", &send.level_db);
                    put_b(se, "pre", &send.pre_fader);
                }
                None => {
                    // Never touched: record the defaults so a recall resets it.
                    se.set("on", 0).set("level", 0).set("pre", 1);
                }
            }
        }
    }
    if sections & INSERTS != 0 {
        let inserts = e.add_new("Inserts");
        for slot in 0..NUM_INSERT_SLOTS {
            let ie = inserts.add_new("Insert");
            ie.set("slot", slot);
            if !s.inserts.is_loaded(slot) {
                continue;
            }
            let Some(captured) = bridge.capture_insert(s, slot) else { continue };
            ie.set_bool("bypass", s.inserts.is_bypassed(slot));
            if let Ok(desc) = parse(&captured.description) {
                ie.add(desc);
            }
            ie.add_new("State").add_text(captured.state);
        }
    }
    e
}

fn capture_monitor(engine: &Engine, parent: &mut Element) {
    let c = &engine.controls;
    let m = parent.add_new("Monitor");
    put_b(m, "afl", &c.solo_afl);
    put_f(m, "levelDb", &c.monitor_level_db);
    put_i(m, "outL", &c.monitor_output_left);
    put_i(m, "outR", &c.monitor_output_right);
}

fn apply_monitor(engine: &Engine, parent: &Element) {
    if let Some(m) = parent.child("Monitor") {
        let c = &engine.controls;
        get_b(m, "afl", &c.solo_afl);
        get_f(m, "levelDb", &c.monitor_level_db);
        get_i(m, "outL", &c.monitor_output_left);
        get_i(m, "outR", &c.monitor_output_right);
    }
}

/// Captures `sections` of every strip (or only `only`) into a `<State>` element.
pub fn capture(engine: &Engine, sections: u32, only: Option<&[StripRef]>, bridge: &mut dyn PluginBridge) -> Element {
    let index = StripIndex::new(engine);
    let slots = index.bus_slots();
    let mut state = Element::new("State");
    state.set("sections", sections as i32);

    for s in std::iter::once(&index.master).chain(&index.buses).chain(&index.inputs) {
        if contains(only, &StripRef::of(s)) {
            state.add(capture_strip(s, sections, &slots, bridge));
        }
    }

    if sections & section::ROUTING != 0 && contains(only, &StripRef::of(&index.master)) {
        capture_monitor(engine, &mut state);
    }
    state
}

// ---- apply ----------------------------------------------------------------------------

#[derive(Clone, Copy, PartialEq, Eq)]
enum PluginMode {
    Instantiate,
    StateOnly,
    Skip,
}

fn apply_strip(
    s: &Arc<Strip>,
    e: &Element,
    requested: u32,
    mode: PluginMode,
    bridge: &mut dyn PluginBridge,
    fades: &mut Option<&mut Vec<FadeTarget>>,
    result: &mut ApplyResult,
) {
    use section::*;
    // Only touch what was both captured and requested.
    let sections = requested & (e.int_or("sections", ALL as i32) as u32);
    let p = &s.params;

    if sections & NAME != 0 {
        if let Some(n) = e.attr("name") {
            s.set_name(n);
        }
        if let Some(c) = e.attr("colour") {
            s.set_colour(colour_from_string(c));
        }
    }
    if sections & ROUTING != 0 {
        if s.kind() != StripKind::Master {
            get_b(e, "stereo", &p.stereo);
        }
        get_i(e, "inL", &p.input_left);
        get_i(e, "inR", &p.input_right);
        get_i(e, "outL", &p.output_left);
        get_i(e, "outR", &p.output_right);
        get_b(e, "mainAssign", &p.main_assign);
    }
    if sections & PREAMP != 0 {
        get_f(e, "trimDb", &p.trim_db);
        get_b(e, "polarity", &p.polarity_invert);
        get_b(e, "hpf", &p.hpf_enabled);
        get_f(e, "hpfFreq", &p.hpf_frequency);
    }
    if sections & FADER != 0 {
        get_fadable(e, "faderDb", FadeParam::Fader(s.clone()), fades);
        get_fadable(e, "pan", FadeParam::Pan(s.clone()), fades);
        get_b(e, "mute", &p.mute);
    }
    if let Some(g) = e.child("Gate").filter(|_| sections & GATE != 0) {
        get_b(g, "on", &p.gate.enabled);
        get_f(g, "threshold", &p.gate.threshold_db);
        get_f(g, "range", &p.gate.range_db);
        get_f(g, "attack", &p.gate.attack_ms);
        get_f(g, "hold", &p.gate.hold_ms);
        get_f(g, "release", &p.gate.release_ms);
    }
    if let Some(eq) = e.child("Eq").filter(|_| sections & EQ != 0) {
        get_b(eq, "on", &p.eq.enabled);
        for band in eq.children_named("Band") {
            let i = band.int_or("index", -1);
            if i < 0 || i as usize >= NUM_BANDS {
                continue;
            }
            let b = &p.eq.bands[i as usize];
            get_i(band, "type", &b.band_type);
            get_f(band, "freq", &b.frequency);
            get_f(band, "gain", &b.gain_db);
            get_f(band, "q", &b.q);
            get_b(band, "on", &b.enabled);
        }
    }
    if let Some(c) = e.child("Comp").filter(|_| sections & COMP != 0) {
        get_b(c, "on", &p.comp.enabled);
        get_f(c, "threshold", &p.comp.threshold_db);
        get_f(c, "ratio", &p.comp.ratio);
        get_f(c, "attack", &p.comp.attack_ms);
        get_f(c, "release", &p.comp.release_ms);
        get_f(c, "knee", &p.comp.knee_db);
        get_f(c, "makeup", &p.comp.makeup_db);
    }
    if let Some(sends) = e.child("Sends").filter(|_| sections & SENDS != 0) {
        for se in sends.children_named("Send") {
            let slot = se.int_or("slot", -1);
            if slot < 0 || slot as usize >= MAX_BUSES {
                continue;
            }
            let slot = slot as usize;
            let Some(send) = s.sends.get_or_create(slot) else { continue };
            get_b(se, "on", &send.enabled);
            get_fadable(se, "level", FadeParam::SendLevel(s.clone(), slot), fades);
            get_b(se, "pre", &send.pre_fader);
        }
    }
    if let Some(inserts) = e.child("Inserts").filter(|_| sections & INSERTS != 0 && mode != PluginMode::Skip) {
        for ie in inserts.children_named("Insert") {
            let slot = ie.int_or("slot", -1);
            if slot < 0 || slot as usize >= NUM_INSERT_SLOTS {
                continue;
            }
            let slot = slot as usize;
            // An empty slot when captured: leave whatever is there now.
            let Some(desc) = ie.child("PLUGIN") else { continue };
            let plugin_name = desc.str_or("name", "plugin");
            let state = ie.child("State").map(Element::text).unwrap_or_default();
            let insert_mode = if mode == PluginMode::Instantiate { InsertMode::Instantiate } else { InsertMode::StateOnly };

            match bridge.restore_insert(s, slot, insert_mode, &desc.to_compact(), &state) {
                Ok(()) => s.inserts.set_bypassed(slot, ie.bool_or("bypass", false)),
                Err(RestoreError::Mismatch) => {
                    result.warnings.push(format!("{} insert {}: expected {plugin_name}, not recalled", s.name(), slot + 1))
                }
                Err(RestoreError::Failed(msg)) => result.warnings.push(format!("{}: {plugin_name} ({msg})", s.name())),
            }
        }
    }

    result.strips_applied += 1;
}

/// Applies a `<State>` element. Only the intersection of `sections` and what was
/// captured is touched. Plugins are never created or destroyed: inserts are
/// recalled only into the same plugin already loaded. With `fades`, continuous
/// values are queued there instead of being set.
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
        apply_strip(strip, e, sections, PluginMode::StateOnly, bridge, &mut fades, &mut result);
    }

    let master_ref = StripRef::of(&index.master);
    if sections & section::ROUTING != 0
        && contains(only, &master_ref)
        && !(respect_recall_safe && index.master.params.recall_safe.load(Relaxed))
    {
        apply_monitor(engine, state);
    }
    result
}

// ---- full console (show files) ----------------------------------------------------------

/// The full console, including structure and plugins.
pub fn save_console(engine: &Engine, bridge: &mut dyn PluginBridge) -> Element {
    let index = StripIndex::new(engine);
    let slots = index.bus_slots();
    let mut console = Element::new("Console");

    let mut save = |s: &Strip| {
        let mut e = capture_strip(s, section::ALL, &slots, bridge);
        e.set("kind", s.kind().as_str()).set("id", s.id()).set_bool("recallSafe", s.params.recall_safe.load(Relaxed));
        e
    };

    let master = save(&index.master);
    let buses: Vec<_> = index.buses.iter().map(|b| save(b)).collect();
    let inputs: Vec<_> = index.inputs.iter().map(|s| save(s)).collect();

    capture_monitor(engine, &mut console);
    put_b(&mut console, "outputProtection", &engine.controls.output_protection);
    put_f(&mut console, "outputCeilingDb", &engine.controls.output_ceiling_db);

    console.add_new("Master").add(master);
    let b = console.add_new("Buses");
    for e in buses {
        b.add(e);
    }
    let i = console.add_new("Inputs");
    for e in inputs {
        i.add(e);
    }
    console
}

/// Rebuilds the console from a `<Console>` element. With `load_plugins` false
/// (safe mode) no plugin is instantiated. Returns the warnings (plugins that
/// failed to load, buses that could not be created).
pub fn load_console(engine: &Engine, console: &Element, bridge: &mut dyn PluginBridge, load_plugins: bool) -> Vec<String> {
    engine.reset();
    let mut result = ApplyResult::default();
    let mode = if load_plugins { PluginMode::Instantiate } else { PluginMode::Skip };

    apply_monitor(engine, console);
    get_b(console, "outputProtection", &engine.controls.output_protection);
    get_f(console, "outputCeilingDb", &engine.controls.output_ceiling_db);

    let mut load = |s: &Arc<Strip>, e: &Element, result: &mut ApplyResult| {
        apply_strip(s, e, section::ALL, mode, bridge, &mut None, result);
        s.params.recall_safe.store(e.bool_or("recallSafe", false), Relaxed);
        // Sessions from before per-channel width: a right input meant stereo.
        if s.kind() == StripKind::Input && !e.has_attr("stereo") {
            s.params.stereo.store(e.int_or("inR", -1) >= 0, Relaxed);
        }
    };

    if let Some(e) = console.child("Master").and_then(|m| m.child("Strip")) {
        load(&engine.master(), e, &mut result);
    }

    if let Some(buses) = console.child("Buses") {
        for e in buses.children_named("Strip") {
            let kind = StripKind::parse(e.str_or("kind", ""));
            if !kind.is_bus() {
                continue;
            }
            match engine.add_bus(kind, e.int_or("id", -1)) {
                Some(bus) => load(&bus, e, &mut result),
                None => result.warnings.push("Bus could not be created (all slots in use)".into()),
            }
        }
    }

    if let Some(inputs) = console.child("Inputs") {
        for e in inputs.children_named("Strip") {
            let strip = engine.add_input(e.int_or("id", -1));
            load(&strip, e, &mut result);
        }
    }

    result.warnings
}
