//! Fader layers like the WING's three fader sections.
//!
//! The surface has a left section (12 faders), a center section (8) and a
//! right section (4), each with its own layer selection. Each section has the
//! WING's seven layer keys (manual 4.2), generated from the console whenever
//! they're asked for so they always cover every strip:
//!
//! - left: `1-12`, `13-24`, `25-36`, `37-40 AUX IN`, `BUS MASTER`, `USER 1`, `USER 2`
//! - center: `DCA`, `MAIN MATRIX`, `AUX IN`, `BUS MASTER`, `USER 1`, `USER 2` (no INPUTS key,
//!   as on the console)
//! - right: `MAIN MATRIX`, `DCA`, `INPUTS`, `AUX IN`, `BUS MASTER`, `USER 1`, `USER 2`
//!
//! A layer longer than its section stays one layer: the surface scrolls it with
//! the shift keys. Bigger consoles get more input keys on the left. `USER 1` and
//! `USER 2` show the first two user layers, which hold any faders (strips,
//! DCAs, send levels and FX parameters) in any order and are saved with the
//! show. User layers have fixed slots: a slot can be blank ([`FaderRef::Empty`],
//! an inactive strip on the surface), and editing a slot never moves the
//! others. A section can also fit as many faders as its space allows (width
//! "Auto"), for big channel counts on wide screens.
//!
//! **Spill** puts a computed layer on a section for a while: a DCA's members,
//! the sources sending to a bus, an FX slot's parameters, an automix group's
//! channels or a strip's sends. Spill from the right section goes to the
//! center section, everything else to the left (firmware 3.0). Ending the
//! spill brings back the layer and the scroll position the section had.

use crate::refs::StripRef;
use crate::xml::{parse_int_prefix, Element};
use livedaw_dsp::wing::{ParamKind, FX_MODELS};
use livedaw_engine::params::{FX_EXT, NUM_DCAS};
use livedaw_engine::{Engine, StripKind};
use std::collections::hash_map::RandomState;
use std::fmt;
use std::hash::{BuildHasher, Hasher};
use std::sync::atomic::Ordering;
use std::sync::atomic::{AtomicU64, Ordering::Relaxed};
use std::time::{SystemTime, UNIX_EPOCH};

/// A fader on the surface: a strip ("ch:3"), a DCA ("dca:0".."dca:15"), a
/// send level from a strip to a bus, main or matrix ("send:ch:3>bus:0"), a
/// parameter of an FX slot's device ("fxp:2.5": slot 3, parameter 6) or a MIDI
/// CC sent out of EXTERNAL MIDI CONTROL ("midi:0.7": MIDI channel 1, CC 7; the
/// WING's layer node type MIDI). `Empty` is a blank slot of a user layer (or
/// an edited layer key): an inactive strip, saved as "-".
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum FaderRef {
    Empty,
    Strip(StripRef),
    Dca(i32),
    Send {
        from: StripRef,
        to: StripRef,
    },
    FxParam {
        slot: i32,
        param: i32,
    },
    /// `channel` 0-15, `cc` 0-127.
    MidiCc {
        channel: i32,
        cc: i32,
    },
}

impl FaderRef {
    /// A MIDI CC fader, its channel and CC kept in range.
    pub fn midi_cc(channel: i32, cc: i32) -> Self {
        FaderRef::MidiCc { channel: channel.clamp(0, 15), cc: cc.clamp(0, 127) }
    }

    /// A blank slot.
    pub fn is_empty(&self) -> bool {
        matches!(self, FaderRef::Empty)
    }

    pub fn parse(s: &str) -> Self {
        if s == BLANK_TOKEN {
            return FaderRef::Empty;
        }
        if let Some(i) = s.strip_prefix("dca:") {
            return FaderRef::Dca(parse_int_prefix(i));
        }
        if let Some(rest) = s.strip_prefix("send:") {
            let (from, to) = rest.split_once('>').unwrap_or((rest, ""));
            return FaderRef::Send { from: StripRef::parse(from), to: StripRef::parse(to) };
        }
        if let Some(rest) = s.strip_prefix("fxp:") {
            let (slot, param) = rest.split_once('.').unwrap_or((rest, "0"));
            return FaderRef::FxParam { slot: parse_int_prefix(slot), param: parse_int_prefix(param) };
        }
        if let Some(rest) = s.strip_prefix("midi:") {
            let (channel, cc) = rest.split_once('.').unwrap_or((rest, "0"));
            return FaderRef::midi_cc(parse_int_prefix(channel), parse_int_prefix(cc));
        }
        FaderRef::Strip(StripRef::parse(s))
    }
}

impl fmt::Display for FaderRef {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            FaderRef::Empty => f.write_str(BLANK_TOKEN),
            FaderRef::Strip(r) => r.fmt(f),
            FaderRef::Dca(i) => write!(f, "dca:{i}"),
            FaderRef::Send { from, to } => write!(f, "send:{from}>{to}"),
            FaderRef::FxParam { slot, param } => write!(f, "fxp:{slot}.{param}"),
            FaderRef::MidiCc { channel, cc } => write!(f, "midi:{channel}.{cc}"),
        }
    }
}

impl From<StripRef> for FaderRef {
    fn from(r: StripRef) -> Self {
        FaderRef::Strip(r)
    }
}

/// How a blank slot is saved.
const BLANK_TOKEN: &str = "-";

/// Fixed-slot editing of a layer's faders: slot `i` is `faders[i]`, or blank
/// past the end. Nothing here moves another slot; trailing blanks are dropped
/// (the slots past the end are blank anyway).
pub mod slots {
    use super::{FaderRef, MAX_SLOTS};

    /// The fader in slot `i` (blank past the end).
    pub fn get(faders: &[FaderRef], i: usize) -> FaderRef {
        faders.get(i).copied().unwrap_or(FaderRef::Empty)
    }

    /// Drops the trailing blanks.
    pub fn trim(faders: &mut Vec<FaderRef>) {
        while faders.last().is_some_and(FaderRef::is_empty) {
            faders.pop();
        }
    }

    /// Puts `f` in slot `i`, replacing what was there (blanks fill the gap).
    /// Slots past [`MAX_SLOTS`] are ignored.
    pub fn set(faders: &mut Vec<FaderRef>, i: usize, f: FaderRef) {
        if i >= MAX_SLOTS {
            return;
        }
        if faders.len() <= i {
            faders.resize(i + 1, FaderRef::Empty);
        }
        faders[i] = f;
        trim(faders);
    }

    /// Makes slot `i` blank.
    pub fn clear(faders: &mut Vec<FaderRef>, i: usize) {
        set(faders, i, FaderRef::Empty);
    }

    /// Swaps slots `a` and `b` (moving into a blank slot leaves `a` blank).
    pub fn swap(faders: &mut Vec<FaderRef>, a: usize, b: usize) {
        if a >= MAX_SLOTS || b >= MAX_SLOTS {
            return;
        }
        let (fa, fb) = (get(faders, a), get(faders, b));
        set(faders, a, fb);
        set(faders, b, fa);
    }
}

pub fn faders_to_string(faders: &[FaderRef]) -> String {
    faders.iter().map(ToString::to_string).collect::<Vec<_>>().join(",")
}

pub fn faders_from_string(s: &str) -> Vec<FaderRef> {
    s.split(',').map(str::trim).filter(|p| !p.is_empty()).map(FaderRef::parse).collect()
}

#[derive(Debug, Clone, PartialEq)]
pub struct Layer {
    /// Stable: "in-1".."in-<n>", "bus-1", "dca-1", "out-1", "aux", "inputs",
    /// "user-1", "user-2" (the USER keys) or "user-<uuid>" (a user layer).
    pub id: String,
    pub name: String,
    pub faders: Vec<FaderRef>,
    pub user: bool,
}

pub const NUM_SECTIONS: usize = 3;
pub const LEFT: usize = 0;
pub const CENTER: usize = 1;
pub const RIGHT: usize = 2;
/// The WING's section widths.
pub const WING_WIDTHS: [i32; NUM_SECTIONS] = [12, 8, 4];
/// Width 0 = Auto: the section holds as many faders as fit.
pub const AUTO_WIDTH: i32 = 0;
pub const MAX_WIDTH: i32 = 256;
/// Slots a layer can have (keeps a stray index from making a huge layer).
pub const MAX_SLOTS: usize = 1024;
/// The first key of each section, lit after a reset (manual fig. 9).
pub const DEFAULT_SELECTION: [&str; NUM_SECTIONS] = ["in-1", "dca-1", "out-1"];
/// Channels per input key on the left section (`1-12`, `13-24`, ...).
pub const INPUT_KEY_CHANNELS: usize = 12;
/// The ids of the USER 1 and USER 2 keys.
pub const USER_KEYS: [&str; 2] = ["user-1", "user-2"];

/// What a section spills (see the module docs).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SpillKind {
    /// The strips assigned to a DCA (0-based).
    Dca(i32),
    /// The send levels of the strips sending to a bus (its slot), as faders.
    Bus(i32),
    /// The parameters of an FX slot's device (0-based slot).
    Fx(i32),
    /// The channels of automix group X (0) or Y (1).
    Automix(i32),
    /// A strip's sends to the buses, mains and matrices (SELECTED CH SENDS).
    Sends(StripRef),
}

/// The layer id of a spill.
pub const SPILL_ID: &str = "spill";

#[derive(Debug, Clone, PartialEq)]
struct Spill {
    kind: SpillKind,
    /// What the section showed before (brought back when the spill ends).
    layer: String,
    offset: i32,
}

/// The section a spill from `origin` goes to: the center for the right
/// section (firmware 3.0), the left for everything else (DCA SPILL, the
/// screens' SPILL keys, custom controls).
pub fn spill_target(origin: Option<usize>) -> usize {
    if origin == Some(RIGHT) {
        CENTER
    } else {
        LEFT
    }
}

fn bus_can_send(from: &StripRef, to_slot: i32) -> bool {
    match from.kind {
        StripKind::Channel | StripKind::Aux => true,
        // Buses 9 and up feed buses 1-8.
        StripKind::Bus => from.id >= 8 && (0..8).contains(&to_slot),
        _ => false,
    }
}

/// The faders a spill shows, from the console as it is now.
pub fn spill_faders(engine: &Engine, kind: &SpillKind) -> Vec<FaderRef> {
    let strips = || StripKind::ALL.into_iter().flat_map(|k| engine.strips(k));
    match *kind {
        SpillKind::Dca(i) if (0..NUM_DCAS as i32).contains(&i) => strips()
            .filter(|s| s.params.dca_mask.load(Ordering::Relaxed) & (1 << i) != 0)
            .map(|s| FaderRef::Strip(StripRef::of(&s)))
            .collect(),
        SpillKind::Automix(group) => engine
            .strips(StripKind::Channel)
            .iter()
            .filter(|s| s.automix_group() == usize::try_from(group).ok())
            .map(|s| FaderRef::Strip(StripRef::of(s)))
            .collect(),
        SpillKind::Bus(slot) => {
            let to = StripRef::new(StripKind::Bus, slot);
            if engine.bus_in_slot(slot).is_none() {
                return Vec::new();
            }
            strips()
                .filter(|s| {
                    let from = StripRef::of(s);
                    bus_can_send(&from, slot)
                        && usize::try_from(slot)
                            .ok()
                            .and_then(|i| s.sends.find(i))
                            .is_some_and(|p| p.enabled.load(Ordering::Relaxed))
                })
                .map(|s| FaderRef::Send { from: StripRef::of(&s), to })
                .collect()
        }
        SpillKind::Sends(from) => {
            let mut out = Vec::new();
            if from.kind.is_input() || from.kind == StripKind::Bus {
                for b in engine.strips(StripKind::Bus) {
                    if bus_can_send(&from, b.id()) && !(from.kind == StripKind::Bus && from.id == b.id()) {
                        out.push(FaderRef::Send { from, to: StripRef::of(&b) });
                    }
                }
                for m in engine.strips(StripKind::Main) {
                    out.push(FaderRef::Send { from, to: StripRef::of(&m) });
                }
            }
            if from.kind != StripKind::Matrix {
                for m in engine.strips(StripKind::Matrix) {
                    out.push(FaderRef::Send { from, to: StripRef::of(&m) });
                }
            }
            out
        }
        SpillKind::Fx(slot) => {
            let Some(p) = usize::try_from(slot).ok().and_then(|i| engine.controls.fx.get(i)) else { return Vec::new() };
            let model = p.model_index();
            if model == FX_EXT {
                return Vec::new();
            }
            FX_MODELS
                .get(model)
                .map(|m| {
                    m.params
                        .iter()
                        .enumerate()
                        .filter(|(_, ps)| matches!(ps.kind, ParamKind::Linear | ParamKind::Log))
                        .map(|(i, _)| FaderRef::FxParam { slot, param: i as i32 })
                        .collect()
                })
                .unwrap_or_default()
        }
        SpillKind::Dca(_) => Vec::new(),
    }
}

/// The spill layer's name ("DCA 3 SPILL", "FX 2 SPILL", ...).
fn spill_name(kind: &SpillKind) -> String {
    match kind {
        SpillKind::Dca(i) => format!("DCA {} SPILL", i + 1),
        SpillKind::Bus(i) => format!("BUS {} SPILL", i + 1),
        SpillKind::Fx(i) => format!("FX {} SPILL", i + 1),
        SpillKind::Automix(g) => format!("AUTO-{} SPILL", if *g == 0 { 'X' } else { 'Y' }),
        SpillKind::Sends(r) => format!("{} SENDS", r.to_string().to_uppercase().replace(':', " ")),
    }
}

/// A default layer key edited on VIEW > CONFIG: its faders replace the generated ones.
#[derive(Debug, Clone, PartialEq)]
struct Override {
    section: usize,
    id: String,
    faders: Vec<FaderRef>,
}

#[derive(Debug, Clone)]
pub struct LayerSet {
    widths: [i32; NUM_SECTIONS],
    fitted: [i32; NUM_SECTIONS],
    user_layers: Vec<Layer>,
    selected: [String; NUM_SECTIONS],
    overrides: Vec<Override>,
    /// The first fader each section shows (scroll position; not saved).
    offsets: [i32; NUM_SECTIONS],
    /// A section's spill (not saved).
    spills: [Option<Spill>; NUM_SECTIONS],
}

impl Default for LayerSet {
    fn default() -> Self {
        Self {
            widths: WING_WIDTHS,
            fitted: WING_WIDTHS,
            user_layers: Vec::new(),
            selected: DEFAULT_SELECTION.map(String::from),
            overrides: Vec::new(),
            offsets: [0; NUM_SECTIONS],
            spills: [None, None, None],
        }
    }
}

/// The faders of the console in layer order, independent of the engine (for tests).
#[derive(Debug, Default, Clone)]
pub struct ConsoleRefs {
    pub channels: Vec<StripRef>,
    pub auxes: Vec<StripRef>,
    pub buses: Vec<StripRef>,
    pub mains: Vec<StripRef>,
    pub matrices: Vec<StripRef>,
    pub num_dcas: usize,
}

impl ConsoleRefs {
    pub fn of(engine: &Engine) -> Self {
        let refs = |kind| engine.strips(kind).iter().map(|s| StripRef::of(s)).collect();
        ConsoleRefs {
            channels: refs(StripKind::Channel),
            auxes: refs(StripKind::Aux),
            buses: refs(StripKind::Bus),
            mains: refs(StripKind::Main),
            matrices: refs(StripKind::Matrix),
            num_dcas: NUM_DCAS,
        }
    }

    /// The WING's default: a fully populated console.
    pub fn wing(channels: usize) -> Self {
        let refs = |kind, ids: std::ops::Range<i32>| ids.map(|i| StripRef::new(kind, i)).collect();
        ConsoleRefs {
            channels: refs(StripKind::Channel, 1..channels as i32 + 1),
            auxes: refs(StripKind::Aux, 0..8),
            buses: refs(StripKind::Bus, 0..16),
            mains: refs(StripKind::Main, 0..4),
            matrices: refs(StripKind::Matrix, 0..8),
            num_dcas: NUM_DCAS,
        }
    }
}

/// A fader with its position-based label ("CH" 1, "AUX" 3, ...).
#[derive(Clone, Copy)]
struct Labelled {
    fader: FaderRef,
    label: &'static str,
    number: usize,
}

fn labelled<'a>(refs: &'a [StripRef], label: &'static str) -> impl Iterator<Item = Labelled> + 'a {
    refs.iter().enumerate().map(move |(i, r)| Labelled { fader: FaderRef::Strip(*r), label, number: i + 1 })
}

/// "CH 37-40 · AUX 1-8": runs of the same label, numbered by position.
fn bank_name(chunk: &[Labelled]) -> String {
    let mut parts: Vec<String> = Vec::new();
    let mut i = 0;
    while i < chunk.len() {
        let start = chunk[i];
        let mut j = i;
        while j + 1 < chunk.len() && chunk[j + 1].label == start.label {
            j += 1;
        }
        let end = chunk[j].number;
        parts.push(if end == start.number {
            format!("{} {}", start.label, start.number)
        } else {
            format!("{} {}-{end}", start.label, start.number)
        });
        i = j + 1;
    }
    parts.join(" · ")
}

fn layer(id: &str, faders: &[Labelled], fallback_name: &str) -> Layer {
    Layer {
        id: id.into(),
        name: if faders.is_empty() { fallback_name.into() } else { bank_name(faders) },
        faders: faders.iter().map(|l| l.fader).collect(),
        user: false,
    }
}

/// The left section's input keys: channels in chunks of `size`, the last chunk
/// followed by the aux inputs ("CH 1-12", ..., "CH 37-40 · AUX 1-8").
fn input_keys(channels: &[Labelled], auxes: &[Labelled], size: usize) -> Vec<Layer> {
    let mut chunks: Vec<Vec<Labelled>> = channels.chunks(size.max(1)).map(<[Labelled]>::to_vec).collect();
    match chunks.last_mut() {
        Some(last) => last.extend_from_slice(auxes),
        None => chunks.push(auxes.to_vec()),
    }
    chunks.iter().enumerate().map(|(i, c)| layer(&format!("in-{}", i + 1), c, "CH")).collect()
}

/// The key that now holds the faders of a layer id from an older show ("bus-2",
/// "dca-2", "out-3", input banks of another width), if the section has one.
fn legacy_layer_id(id: &str, layers: &[Layer]) -> Option<&'static str> {
    let has = |key: &str| layers.iter().any(|l| l.id == key);
    let key = match id.split_once('-').map_or(id, |(prefix, _)| prefix) {
        "bus" => "bus-1",
        "dca" => "dca-1",
        "out" => "out-1",
        "in" if has("inputs") => "inputs",
        "in" => "in-1",
        _ => return None,
    };
    has(key).then_some(key)
}

impl LayerSet {
    // ---- section widths ------------------------------------------------------------

    /// The width setting of a section (0 = Auto).
    pub fn width(&self, section: usize) -> i32 {
        self.widths.get(section).copied().unwrap_or(AUTO_WIDTH)
    }

    pub fn set_width(&mut self, section: usize, width: i32) {
        if let Some(w) = self.widths.get_mut(section) {
            // 0 and below = Auto.
            *w = width.clamp(AUTO_WIDTH, MAX_WIDTH);
        }
    }

    pub fn is_auto_width(&self, section: usize) -> bool {
        self.width(section) == AUTO_WIDTH
    }

    /// How many faders fit the section right now (used when its width is Auto).
    pub fn set_fitted_width(&mut self, section: usize, width: i32) {
        if let Some(w) = self.fitted.get_mut(section) {
            *w = width.clamp(1, MAX_WIDTH);
        }
    }

    pub fn effective_width(&self, section: usize) -> i32 {
        if self.is_auto_width(section) {
            self.fitted.get(section).copied().unwrap_or(1)
        } else {
            self.width(section)
        }
    }

    // ---- layers ----------------------------------------------------------------------

    /// The section's layer keys, top to bottom (see the module docs).
    pub fn default_layers_for(&self, section: usize, console: &ConsoleRefs) -> Vec<Layer> {
        let channels: Vec<Labelled> = labelled(&console.channels, "CH").collect();
        let auxes: Vec<Labelled> = labelled(&console.auxes, "AUX").collect();
        let buses: Vec<Labelled> = labelled(&console.buses, "BUS").collect();
        let dcas: Vec<Labelled> =
            (0..console.num_dcas).map(|i| Labelled { fader: FaderRef::Dca(i as i32), label: "DCA", number: i + 1 }).collect();
        let outputs: Vec<Labelled> = labelled(&console.mains, "MAIN").chain(labelled(&console.matrices, "MTX")).collect();

        let bus = layer("bus-1", &buses, "BUS");
        let dca = layer("dca-1", &dcas, "DCA");
        let out = layer("out-1", &outputs, "MAIN");
        let aux = layer("aux", &auxes, "AUX");
        let inputs = layer("inputs", &channels, "CH");

        let mut layers = match section {
            CENTER => vec![dca, out, aux, bus],
            RIGHT => vec![out, dca, inputs, aux, bus],
            _ => {
                // A section wider than 12 (Auto width on a big screen) gets longer input keys.
                let size = INPUT_KEY_CHANNELS.max(self.effective_width(section).max(1) as usize);
                let mut keys = input_keys(&channels, &auxes, size);
                keys.push(bus);
                keys
            }
        };
        // "The default layers are entirely user-assignable": edited keys keep
        // their name and show the edited faders.
        for l in &mut layers {
            if let Some(o) = self.overrides.iter().find(|o| o.section == section && o.id == l.id) {
                l.faders.clone_from(&o.faders);
            }
        }
        for (i, id) in USER_KEYS.iter().enumerate() {
            let user = self.user_layers.get(i);
            layers.push(Layer {
                id: (*id).into(),
                name: user.map_or_else(|| format!("USER {}", i + 1), |l| l.name.clone()),
                faders: user.map(|l| l.faders.clone()).unwrap_or_default(),
                user: false,
            });
        }
        layers
    }

    pub fn default_layers(&self, engine: &Engine, section: usize) -> Vec<Layer> {
        self.default_layers_for(section, &ConsoleRefs::of(engine))
    }

    /// Default layers followed by user layers.
    pub fn all_layers(&self, engine: &Engine, section: usize) -> Vec<Layer> {
        let mut layers = self.default_layers(engine, section);
        layers.extend(self.user_layers.iter().cloned());
        layers
    }

    /// Finds a layer by id. Ids from older shows map to the key that holds those
    /// faders now; anything else falls back to the section's first layer.
    pub fn find_layer(&self, engine: &Engine, section: usize, id: &str) -> Layer {
        if id == SPILL_ID {
            if let Some(layer) = self.spill_layer(engine, section) {
                return layer;
            }
        }
        self.find_layer_for(section, &ConsoleRefs::of(engine), id)
    }

    pub fn find_layer_for(&self, section: usize, console: &ConsoleRefs, id: &str) -> Layer {
        if id == SPILL_ID {
            // Without an engine the spill has no faders; find_layer fills them in.
            if let Some(kind) = self.spill(section) {
                return Layer { id: SPILL_ID.into(), name: spill_name(&kind), faders: Vec::new(), user: false };
            }
        }
        let mut layers = self.default_layers_for(section, console);
        layers.extend(self.user_layers.iter().cloned());
        let index = layers
            .iter()
            .position(|l| l.id == id)
            .or_else(|| legacy_layer_id(id, &layers).and_then(|key| layers.iter().position(|l| l.id == key)))
            .unwrap_or(0);
        layers.swap_remove(index)
    }

    /// The layer a section shows now (its spill while it spills).
    pub fn selected_layer(&self, engine: &Engine, section: usize) -> Layer {
        if let Some(layer) = self.spill_layer(engine, section) {
            return layer;
        }
        self.find_layer(engine, section, self.selected(section))
    }

    /// The default layer of a section that contains `fader` (not the USER keys).
    pub fn find_default_layer_containing(&self, engine: &Engine, section: usize, fader: &FaderRef) -> Option<String> {
        if fader.is_empty() {
            return None;
        }
        self.default_layers(engine, section)
            .into_iter()
            .find(|l| !USER_KEYS.contains(&l.id.as_str()) && l.faders.contains(fader))
            .map(|l| l.id)
    }

    pub fn selected(&self, section: usize) -> &str {
        self.selected.get(section).map_or(DEFAULT_SELECTION[0], String::as_str)
    }

    /// The layer id a section shows: [`SPILL_ID`] while it spills, else its selected key.
    pub fn shown(&self, section: usize) -> &str {
        if self.spill(section).is_some() {
            SPILL_ID
        } else {
            self.selected(section)
        }
    }

    /// Selects a layer key; a spill on the section ends (without going back).
    pub fn select(&mut self, section: usize, id: &str) {
        if id == SPILL_ID {
            return;
        }
        let Some(s) = self.selected.get_mut(section) else { return };
        let spilling = self.spills[section].take().is_some();
        if *s != id || spilling {
            // Another layer starts at its first fader.
            self.offsets[section] = 0;
        }
        *s = id.to_owned();
    }

    // ---- scroll positions and spill ------------------------------------------------

    /// The first fader a section shows.
    pub fn offset(&self, section: usize) -> i32 {
        self.offsets.get(section).copied().unwrap_or(0)
    }

    pub fn set_offset(&mut self, section: usize, offset: i32) {
        if let Some(o) = self.offsets.get_mut(section) {
            *o = offset.max(0);
        }
    }

    /// Spills `kind` onto `section` (see [`spill_target`]). The layer and
    /// scroll position before the first spill come back when it ends.
    pub fn start_spill(&mut self, section: usize, kind: SpillKind) {
        if section >= NUM_SECTIONS {
            return;
        }
        let (layer, offset) = match &self.spills[section] {
            Some(previous) => (previous.layer.clone(), previous.offset),
            None => (self.selected(section).to_owned(), self.offset(section)),
        };
        self.spills[section] = Some(Spill { kind, layer, offset });
        self.offsets[section] = 0;
    }

    /// Ends a section's spill: its layer and scroll position come back.
    /// False if it wasn't spilling.
    pub fn end_spill(&mut self, section: usize) -> bool {
        let Some(spill) = self.spills.get_mut(section).and_then(Option::take) else { return false };
        self.selected[section] = spill.layer;
        self.offsets[section] = spill.offset;
        true
    }

    /// What a section spills, if it does.
    pub fn spill(&self, section: usize) -> Option<SpillKind> {
        self.spills.get(section).and_then(|s| s.as_ref()).map(|s| s.kind)
    }

    /// The section spilling `kind`, if any.
    pub fn section_spilling(&self, kind: &SpillKind) -> Option<usize> {
        (0..NUM_SECTIONS).find(|&s| self.spill(s).as_ref() == Some(kind))
    }

    /// The spill layer a section shows, if it spills.
    pub fn spill_layer(&self, engine: &Engine, section: usize) -> Option<Layer> {
        let kind = self.spill(section)?;
        Some(Layer { id: SPILL_ID.into(), name: spill_name(&kind), faders: spill_faders(engine, &kind), user: false })
    }

    // ---- user layers ---------------------------------------------------------------

    pub fn user_layers(&self) -> &[Layer] {
        &self.user_layers
    }

    pub fn user_layer(&self, id: &str) -> Option<&Layer> {
        self.user_layers.iter().find(|l| l.id == id)
    }

    fn user_layer_mut(&mut self, id: &str) -> Option<&mut Layer> {
        self.user_layers.iter_mut().find(|l| l.id == id)
    }

    /// Creates a user layer and returns its id.
    pub fn add_user_layer(&mut self, name: &str, mut faders: Vec<FaderRef>) -> String {
        slots::trim(&mut faders);
        let id = format!("user-{}", new_uuid());
        self.user_layers.push(Layer { id: id.clone(), name: name.into(), faders, user: true });
        id
    }

    pub fn remove_user_layer(&mut self, id: &str) {
        self.user_layers.retain(|l| l.id != id);
    }

    pub fn rename_user_layer(&mut self, id: &str, name: &str) {
        if let Some(l) = self.user_layer_mut(id) {
            l.name = name.into();
        }
    }

    pub fn move_user_layer(&mut self, id: &str, delta: i32) {
        if let Some(i) = self.user_layers.iter().position(|l| l.id == id) {
            let target = i as i64 + i64::from(delta);
            if target >= 0 && (target as usize) < self.user_layers.len() {
                self.user_layers.swap(i, target as usize);
            }
        }
    }

    /// Adds a fader to a user layer's first blank slot, or after its last
    /// (no duplicates).
    pub fn add_to_user_layer(&mut self, id: &str, f: FaderRef) {
        if f.is_empty() {
            return;
        }
        if let Some(l) = self.user_layer_mut(id) {
            if !l.faders.contains(&f) {
                let slot = l.faders.iter().position(FaderRef::is_empty).unwrap_or(l.faders.len());
                slots::set(&mut l.faders, slot, f);
            }
        }
    }

    /// Replaces a user layer's faders (as given: blanks and duplicates are kept).
    pub fn set_user_layer_faders(&mut self, id: &str, mut faders: Vec<FaderRef>) {
        faders.truncate(MAX_SLOTS);
        slots::trim(&mut faders);
        if let Some(l) = self.user_layer_mut(id) {
            l.faders = faders;
        }
    }

    /// Blanks the slots holding `f` (the other slots stay where they are).
    pub fn remove_from_user_layer(&mut self, id: &str, f: &FaderRef) {
        if let Some(l) = self.user_layer_mut(id) {
            for x in l.faders.iter_mut().filter(|x| **x == *f) {
                *x = FaderRef::Empty;
            }
            slots::trim(&mut l.faders);
        }
    }

    /// Puts `f` in slot `index` of a user layer, replacing what was there.
    pub fn set_user_slot(&mut self, id: &str, index: usize, f: FaderRef) {
        if let Some(l) = self.user_layer_mut(id) {
            slots::set(&mut l.faders, index, f);
        }
    }

    /// Makes slot `index` of a user layer blank.
    pub fn clear_user_slot(&mut self, id: &str, index: usize) {
        self.set_user_slot(id, index, FaderRef::Empty);
    }

    /// Swaps two slots of a user layer (a move when one of them is blank).
    pub fn swap_user_slots(&mut self, id: &str, a: usize, b: usize) {
        if let Some(l) = self.user_layer_mut(id) {
            slots::swap(&mut l.faders, a, b);
        }
    }

    /// Swaps slot `index` with slot `index + delta`.
    pub fn move_within_user_layer(&mut self, id: &str, index: i32, delta: i32) {
        let target = i64::from(index) + i64::from(delta);
        if let (Ok(a), Ok(b)) = (usize::try_from(index), usize::try_from(target)) {
            self.swap_user_slots(id, a, b);
        }
    }

    // ---- edited default layers -----------------------------------------------------

    /// Replaces the faders of a section's default key (not USER 1 / USER 2,
    /// which show user layers).
    pub fn set_default_override(&mut self, section: usize, id: &str, mut faders: Vec<FaderRef>) {
        faders.truncate(MAX_SLOTS);
        slots::trim(&mut faders);
        if section >= NUM_SECTIONS || id.is_empty() || id.starts_with("user-") {
            return;
        }
        match self.overrides.iter_mut().find(|o| o.section == section && o.id == id) {
            Some(o) => o.faders = faders,
            None => self.overrides.push(Override { section, id: id.to_owned(), faders }),
        }
    }

    /// Back to the generated faders.
    pub fn clear_default_override(&mut self, section: usize, id: &str) {
        self.overrides.retain(|o| o.section != section || o.id != id);
    }

    pub fn has_default_override(&self, section: usize, id: &str) -> bool {
        self.overrides.iter().any(|o| o.section == section && o.id == id)
    }

    // ---- persistence ---------------------------------------------------------------

    pub fn to_xml(&self) -> Element {
        let mut xml = Element::new("Layers");
        let widths = self.widths.iter().map(ToString::to_string).collect::<Vec<_>>().join(",");
        xml.set("widths", widths)
            .set("left", self.selected(LEFT))
            .set("center", self.selected(CENTER))
            .set("right", self.selected(RIGHT));
        for l in &self.user_layers {
            xml.add_new("Layer").set("id", &l.id).set("name", &l.name).set("faders", faders_to_string(&l.faders));
        }
        for o in &self.overrides {
            xml.add_new("Override").set("section", o.section).set("id", &o.id).set("faders", faders_to_string(&o.faders));
        }
        xml
    }

    pub fn from_xml(&mut self, xml: &Element) {
        self.reset();
        if let Some(w) = xml.attr("widths") {
            for (i, part) in w.split(',').take(NUM_SECTIONS).enumerate() {
                self.set_width(i, parse_int_prefix(part));
            }
        }
        for (i, name) in ["left", "center", "right"].iter().enumerate() {
            if let Some(id) = xml.attr(name) {
                self.select(i, id);
            }
        }
        for e in xml.children_named("Layer") {
            let mut id = e.str_or("id", "").to_owned();
            if !id.starts_with("user-") {
                id = format!("user-{}", new_uuid());
            }
            let mut faders = faders_from_string(e.str_or("faders", ""));
            faders.truncate(MAX_SLOTS);
            slots::trim(&mut faders);
            self.user_layers.push(Layer { id, name: e.str_or("name", "User").to_owned(), faders, user: true });
        }
        for e in xml.children_named("Override") {
            let section = usize::try_from(parse_int_prefix(e.str_or("section", "-1"))).unwrap_or(NUM_SECTIONS);
            self.set_default_override(section, e.str_or("id", ""), faders_from_string(e.str_or("faders", "")));
        }
    }

    pub fn reset(&mut self) {
        let fitted = self.fitted; // a property of the window, not the show
        *self = Self { fitted, ..Self::default() };
    }
}

/// 32 hex digits, unique per process run and across runs (not cryptographic).
fn new_uuid() -> String {
    static COUNTER: AtomicU64 = AtomicU64::new(0);
    let nanos = SystemTime::now().duration_since(UNIX_EPOCH).map_or(0, |d| d.as_nanos() as u64);
    let n = COUNTER.fetch_add(1, Relaxed);
    let mut words = [0u64; 2];
    for (i, w) in words.iter_mut().enumerate() {
        let mut h = RandomState::new().build_hasher();
        h.write_u64(nanos);
        h.write_u64(n);
        h.write_usize(i);
        *w = h.finish();
    }
    format!("{:016x}{:016x}", words[0], words[1])
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ch(i: i32) -> FaderRef {
        FaderRef::Strip(StripRef::new(StripKind::Channel, i))
    }

    fn ids(layers: &[Layer]) -> Vec<&str> {
        layers.iter().map(|l| l.id.as_str()).collect()
    }

    #[test]
    fn wing_layer_keys() {
        let set = LayerSet::default();
        let console = ConsoleRefs::wing(40);

        let left = set.default_layers_for(LEFT, &console);
        let names: Vec<_> = left.iter().map(|l| (l.id.as_str(), l.name.as_str())).collect();
        assert_eq!(
            names,
            [
                ("in-1", "CH 1-12"),
                ("in-2", "CH 13-24"),
                ("in-3", "CH 25-36"),
                ("in-4", "CH 37-40 · AUX 1-8"),
                ("bus-1", "BUS 1-16"),
                ("user-1", "USER 1"),
                ("user-2", "USER 2"),
            ]
        );
        assert_eq!(left[3].faders[4], FaderRef::Strip(StripRef::new(StripKind::Aux, 0)));
        assert_eq!(left[4].faders.len(), 16);
        assert!(left[5].faders.is_empty());

        // Layers longer than the section stay one key (the shift keys scroll them).
        let center = set.default_layers_for(CENTER, &console);
        assert_eq!(ids(&center), ["dca-1", "out-1", "aux", "bus-1", "user-1", "user-2"]);
        assert_eq!(center[0].name, "DCA 1-16");
        assert_eq!(center[0].faders[8], FaderRef::Dca(8));
        assert_eq!(center[1].name, "MAIN 1-4 · MTX 1-8");
        assert_eq!(center[2].name, "AUX 1-8");
        assert_eq!(center[3].name, "BUS 1-16");

        let right = set.default_layers_for(RIGHT, &console);
        assert_eq!(ids(&right), ["out-1", "dca-1", "inputs", "aux", "bus-1", "user-1", "user-2"]);
        assert_eq!(right[0].faders.len(), 12);

        let empty = set.default_layers_for(LEFT, &ConsoleRefs::default());
        assert_eq!(ids(&empty), ["in-1", "bus-1", "user-1", "user-2"]);
        assert_eq!(empty[0].name, "CH");
        assert_eq!(empty[1].name, "BUS");
    }

    #[test]
    fn input_keys_follow_the_console() {
        let mut set = LayerSet::default();
        let names = |set: &LayerSet, console: &ConsoleRefs| -> Vec<String> {
            set.default_layers_for(LEFT, console).into_iter().filter(|l| l.id.starts_with("in-")).map(|l| l.name).collect()
        };

        assert_eq!(names(&set, &ConsoleRefs::wing(2)), ["CH 1-2 · AUX 1-8"]);
        assert_eq!(names(&set, &ConsoleRefs::wing(48))[3..], ["CH 37-48 · AUX 1-8"]);
        assert_eq!(names(&set, &ConsoleRefs::wing(64)).len(), 6);

        // A wide Auto section gets keys as wide as the section; never narrower than 12.
        set.set_width(LEFT, AUTO_WIDTH);
        set.set_fitted_width(LEFT, 30);
        assert_eq!(names(&set, &ConsoleRefs::wing(40)), ["CH 1-30", "CH 31-40 · AUX 1-8"]);
        set.set_fitted_width(LEFT, 4);
        assert_eq!(names(&set, &ConsoleRefs::wing(40)).len(), 4);
    }

    #[test]
    fn user_keys_show_the_first_user_layers() {
        let mut set = LayerSet::default();
        let console = ConsoleRefs::wing(8);
        let drums = set.add_user_layer("Drums", vec![ch(1), ch(2)]);
        set.add_user_layer("Vox", vec![FaderRef::Dca(0)]);
        set.add_user_layer("Spare", vec![]);

        let right = set.default_layers_for(RIGHT, &console);
        let user1 = right.iter().find(|l| l.id == "user-1").unwrap();
        assert_eq!((user1.name.as_str(), user1.faders.len(), user1.user), ("Drums", 2, false));
        assert_eq!(right.iter().find(|l| l.id == "user-2").unwrap().faders, [FaderRef::Dca(0)]);

        // A user layer can still be selected by its own id.
        assert_eq!(set.find_layer_for(LEFT, &console, &drums).name, "Drums");
        assert!(set.find_layer_for(LEFT, &console, &drums).user);
    }

    #[test]
    fn layer_ids_of_older_shows_still_resolve() {
        let set = LayerSet::default();
        let console = ConsoleRefs::wing(40);
        let find = |section, id| set.find_layer_for(section, &console, id).id;

        assert_eq!(find(LEFT, "in-3"), "in-3");
        assert_eq!(find(LEFT, "bus-2"), "bus-1");
        assert_eq!(find(LEFT, "in-5"), "in-1");
        assert_eq!(find(LEFT, "dca-2"), "in-1"); // no DCA key on the left
        assert_eq!(find(CENTER, "dca-2"), "dca-1");
        assert_eq!(find(CENTER, "in-3"), "dca-1"); // no INPUTS key in the center
        assert_eq!(find(RIGHT, "in-3"), "inputs");
        assert_eq!(find(CENTER, "all"), "dca-1");
        assert_eq!(find(RIGHT, "out-3"), "out-1");
        assert_eq!(find(RIGHT, "user-0123"), "out-1");

        let default: Vec<_> = (0..NUM_SECTIONS).map(|s| set.find_layer_for(s, &console, set.selected(s)).id).collect();
        assert_eq!(default, ["in-1", "dca-1", "out-1"]);
    }

    #[test]
    fn widths_limits_and_auto() {
        let mut set = LayerSet::default();
        assert_eq!(set.effective_width(LEFT), 12);
        set.set_width(LEFT, AUTO_WIDTH);
        assert!(set.is_auto_width(LEFT));
        set.set_fitted_width(LEFT, 1000);
        assert_eq!(set.effective_width(LEFT), MAX_WIDTH);
        set.set_width(CENTER, -3);
        assert!(set.is_auto_width(CENTER));
        set.set_width(RIGHT, 6);
        assert_eq!(set.width(RIGHT), 6);
        assert_eq!(set.width(7), AUTO_WIDTH);

        let mut loaded = LayerSet::default();
        loaded.from_xml(&set.to_xml());
        assert_eq!([loaded.width(LEFT), loaded.width(CENTER), loaded.width(RIGHT)], [0, 0, 6]);
    }

    #[test]
    fn default_keys_can_be_edited() {
        let mut set = LayerSet::default();
        let console = ConsoleRefs::wing(40);
        set.set_default_override(LEFT, "in-1", vec![ch(5), FaderRef::Dca(1)]);
        set.set_default_override(LEFT, "user-1", vec![ch(9)]); // the USER keys show user layers
        set.set_default_override(7, "in-2", vec![ch(9)]);

        let left = set.default_layers_for(LEFT, &console);
        assert_eq!(left[0].name, "CH 1-12");
        assert_eq!(left[0].faders, vec![ch(5), FaderRef::Dca(1)]);
        assert!(left[5].faders.is_empty());
        assert!(set.has_default_override(LEFT, "in-1"));
        assert!(!set.has_default_override(RIGHT, "in-1"));
        assert_eq!(set.default_layers_for(RIGHT, &console)[2].faders.len(), 40);

        let mut loaded = LayerSet::default();
        loaded.from_xml(&set.to_xml());
        assert_eq!(loaded.default_layers_for(LEFT, &console)[0].faders, vec![ch(5), FaderRef::Dca(1)]);

        loaded.clear_default_override(LEFT, "in-1");
        assert_eq!(loaded.default_layers_for(LEFT, &console)[0].faders.len(), 12);
    }

    #[test]
    fn user_layers_round_trip() {
        let mut set = LayerSet::default();
        let a = set.add_user_layer("Drums", vec![ch(1)]);
        let b = set.add_user_layer("Vox", vec![]);
        assert_ne!(a, b);
        let main = FaderRef::Strip(StripRef::new(StripKind::Main, 0));
        set.add_to_user_layer(&b, main);
        set.add_to_user_layer(&b, main);
        set.add_to_user_layer(&b, FaderRef::Dca(2));
        set.move_within_user_layer(&b, 0, 1);
        set.move_user_layer(&b, -1);
        set.rename_user_layer(&b, "Vocals");
        assert_eq!(set.user_layers()[0].name, "Vocals");
        set.select(CENTER, &b);

        let mut loaded = LayerSet::default();
        loaded.from_xml(&set.to_xml());
        assert_eq!(loaded.user_layers(), set.user_layers());
        assert_eq!(loaded.user_layers()[0].faders, vec![FaderRef::Dca(2), main]);
        assert_eq!(loaded.selected(CENTER), b);
        assert_eq!(loaded.selected(LEFT), "in-1");
        assert_eq!(loaded.selected(RIGHT), "out-1");

        loaded.set_user_layer_faders(&b, vec![ch(4), ch(4)]);
        assert_eq!(loaded.user_layer(&b).unwrap().faders, vec![ch(4), ch(4)]);
        loaded.remove_user_layer(&a);
        assert_eq!(loaded.user_layers().len(), 1);
        assert_eq!(FaderRef::parse("dca:15"), FaderRef::Dca(15));
        assert_eq!(faders_from_string("ch:3, dca:1"), vec![ch(3), FaderRef::Dca(1)]);
    }

    #[test]
    fn send_fx_parameter_and_midi_cc_faders_round_trip() {
        let send = FaderRef::Send { from: StripRef::new(StripKind::Channel, 3), to: StripRef::new(StripKind::Bus, 1) };
        let fx = FaderRef::FxParam { slot: 2, param: 5 };
        let midi = FaderRef::midi_cc(15, 74);
        assert_eq!(send.to_string(), "send:ch:3>bus:1");
        assert_eq!(fx.to_string(), "fxp:2.5");
        assert_eq!(midi.to_string(), "midi:15.74");
        assert_eq!(FaderRef::parse("midi:20.300"), FaderRef::MidiCc { channel: 15, cc: 127 });
        let faders = vec![ch(1), send, fx, FaderRef::Dca(4), midi];
        assert_eq!(faders_from_string(&faders_to_string(&faders)), faders);

        let mut set = LayerSet::default();
        let id = set.add_user_layer("Mix", faders.clone());
        let mut loaded = LayerSet::default();
        loaded.from_xml(&set.to_xml());
        assert_eq!(loaded.user_layer(&id).map(|l| l.faders.clone()), Some(faders));
    }

    #[test]
    fn user_layer_slots_are_fixed() {
        let mut set = LayerSet::default();
        let id = set.add_user_layer("Band", vec![ch(1), ch(2), ch(3)]);
        let faders = |set: &LayerSet| set.user_layer(&id).unwrap().faders.clone();
        let e = FaderRef::Empty;

        // Set past the end: blanks fill the gap.
        set.set_user_slot(&id, 6, FaderRef::Dca(0));
        assert_eq!(faders(&set), vec![ch(1), ch(2), ch(3), e, e, e, FaderRef::Dca(0)]);
        // Replace in place.
        set.set_user_slot(&id, 1, ch(9));
        assert_eq!(faders(&set)[..3], [ch(1), ch(9), ch(3)]);
        // Clear: the others stay where they are.
        set.clear_user_slot(&id, 0);
        assert_eq!(faders(&set), vec![e, ch(9), ch(3), e, e, e, FaderRef::Dca(0)]);
        // Swap two filled slots, then move one into a blank slot.
        set.swap_user_slots(&id, 1, 2);
        assert_eq!(faders(&set)[..3], [e, ch(3), ch(9)]);
        set.swap_user_slots(&id, 2, 4);
        assert_eq!(faders(&set), vec![e, ch(3), e, e, ch(9), e, FaderRef::Dca(0)]);
        // Clearing the last slot drops the trailing blanks.
        set.clear_user_slot(&id, 6);
        assert_eq!(faders(&set), vec![e, ch(3), e, e, ch(9)]);
        // Removing a fader blanks its slot; adding fills the first blank.
        set.remove_from_user_layer(&id, &ch(3));
        assert_eq!(faders(&set), vec![e, e, e, e, ch(9)]);
        set.add_to_user_layer(&id, ch(5));
        assert_eq!(faders(&set), vec![ch(5), e, e, e, ch(9)]);
        set.add_to_user_layer(&id, FaderRef::Empty);
        assert_eq!(faders(&set).len(), 5);
        // A stray index doesn't make a huge layer.
        set.set_user_slot(&id, usize::MAX, ch(7));
        set.swap_user_slots(&id, 0, MAX_SLOTS);
        assert_eq!(faders(&set), vec![ch(5), e, e, e, ch(9)]);
        set.move_within_user_layer(&id, 4, -3);
        assert_eq!(faders(&set), vec![ch(5), ch(9)]);
        set.move_within_user_layer(&id, 0, -1);
        assert_eq!(faders(&set), vec![ch(5), ch(9)]);
    }

    #[test]
    fn blank_slots_are_saved() {
        let mut set = LayerSet::default();
        let e = FaderRef::Empty;
        let id = set.add_user_layer("Gaps", vec![e, ch(2), e, FaderRef::Dca(3), e, e]);
        assert_eq!(set.user_layer(&id).unwrap().faders, vec![e, ch(2), e, FaderRef::Dca(3)]);
        set.set_default_override(LEFT, "in-2", vec![ch(4), e, ch(6)]);
        let xml = set.to_xml();
        assert_eq!(xml.children_named("Layer").next().unwrap().str_or("faders", ""), "-,ch:2,-,dca:3");

        let mut loaded = LayerSet::default();
        loaded.from_xml(&xml);
        assert_eq!(loaded.user_layers(), set.user_layers());
        let console = ConsoleRefs::wing(40);
        assert_eq!(loaded.default_layers_for(LEFT, &console)[1].faders, vec![ch(4), e, ch(6)]);
        assert_eq!(FaderRef::parse("-"), e);
        assert_eq!(faders_from_string("-, ch:1 ,-"), vec![e, ch(1), e]);
    }

    #[test]
    fn shows_without_blanks_load_unchanged() {
        // A show saved before blank slots existed.
        let mut old = Element::new("Layers");
        old.set("widths", "12,8,4").set("left", "in-1").set("center", "dca-1").set("right", "out-1");
        old.add_new("Layer").set("id", "user-0123").set("name", "Drums").set("faders", "ch:1,ch:2,dca:0,send:ch:3>bus:1");
        old.add_new("Layer").set("id", "user-4567").set("name", "Empty").set("faders", "");
        let mut set = LayerSet::default();
        set.from_xml(&old);
        let send = FaderRef::Send { from: StripRef::new(StripKind::Channel, 3), to: StripRef::new(StripKind::Bus, 1) };
        assert_eq!(set.user_layers()[0].faders, vec![ch(1), ch(2), FaderRef::Dca(0), send]);
        assert!(set.user_layers()[1].faders.is_empty());
        assert!(set.user_layers().iter().all(|l| !l.faders.iter().any(FaderRef::is_empty)));
        // Saved again, it reads the same.
        assert_eq!(set.to_xml().children_named("Layer").next().unwrap().str_or("faders", ""), "ch:1,ch:2,dca:0,send:ch:3>bus:1");
    }

    #[test]
    fn user_keys_keep_blank_slots() {
        let mut set = LayerSet::default();
        let console = ConsoleRefs::wing(8);
        let id = set.add_user_layer("Drums", vec![]);
        set.set_user_slot(&id, 2, ch(1));
        set.set_user_slot(&id, 5, FaderRef::Dca(0));

        let user1 = set.default_layers_for(CENTER, &console).into_iter().find(|l| l.id == "user-1").unwrap();
        assert_eq!(user1.faders.len(), 6);
        assert_eq!(slots::get(&user1.faders, 2), ch(1));
        assert_eq!(slots::get(&user1.faders, 5), FaderRef::Dca(0));
        for i in [0, 1, 3, 4, 6, 7, 100] {
            assert!(slots::get(&user1.faders, i).is_empty(), "slot {i}");
        }
        assert_eq!(set.find_layer_for(LEFT, &console, &id).faders, user1.faders);
        // A blank slot is in no default layer ("go to strip" finds nothing).
        let e = Engine::new(0);
        assert_eq!(set.find_default_layer_containing(&e, LEFT, &FaderRef::Empty), None);
    }

    #[test]
    fn spill_targets_and_restore() {
        assert_eq!(spill_target(Some(RIGHT)), CENTER);
        assert_eq!(spill_target(Some(CENTER)), LEFT);
        assert_eq!(spill_target(None), LEFT);

        let mut set = LayerSet::default();
        set.select(LEFT, "in-3");
        set.set_offset(LEFT, 8);
        set.start_spill(LEFT, SpillKind::Dca(2));
        assert_eq!((set.spill(LEFT), set.offset(LEFT)), (Some(SpillKind::Dca(2)), 0));
        assert_eq!(set.section_spilling(&SpillKind::Dca(2)), Some(LEFT));
        // A second spill keeps the layer to go back to.
        set.set_offset(LEFT, 4);
        set.start_spill(LEFT, SpillKind::Fx(0));
        assert!(set.end_spill(LEFT));
        assert_eq!((set.selected(LEFT), set.offset(LEFT), set.spill(LEFT)), ("in-3", 8, None));
        assert!(!set.end_spill(LEFT));

        // A layer key ends the spill without going back.
        set.start_spill(CENTER, SpillKind::Automix(1));
        set.select(CENTER, "out-1");
        assert_eq!((set.spill(CENTER), set.selected(CENTER)), (None, "out-1"));
        set.select(CENTER, SPILL_ID);
        assert_eq!(set.selected(CENTER), "out-1");
    }
}
