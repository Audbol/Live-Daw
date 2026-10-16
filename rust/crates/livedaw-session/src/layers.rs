//! Fader layers like the WING's three fader sections.
//!
//! The surface has a left section (12 faders), a center section (8) and a
//! right section (4), each with its own layer selection. Default layers are
//! generated from the console whenever they're asked for, so they always cover
//! every strip: input banks (CH 1-12, ..., CH 37-40 · AUX 1-8), bus banks, DCA
//! banks, main/matrix banks and ALL, cut to the width of the section. User
//! layers hold any faders (strips and DCAs) in any order and are saved with
//! the show. A section can also fit as many faders as its space allows (width
//! "Auto"), for big channel counts on wide screens.

use crate::refs::StripRef;
use crate::xml::{parse_int_prefix, Element};
use livedaw_engine::params::NUM_DCAS;
use livedaw_engine::{Engine, StripKind};
use std::collections::hash_map::RandomState;
use std::fmt;
use std::hash::{BuildHasher, Hasher};
use std::sync::atomic::{AtomicU64, Ordering::Relaxed};
use std::time::{SystemTime, UNIX_EPOCH};

/// A fader on the surface: a strip or a DCA ("dca:0".."dca:15").
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum FaderRef {
    Strip(StripRef),
    Dca(i32),
}

impl FaderRef {
    pub fn parse(s: &str) -> Self {
        match s.strip_prefix("dca:") {
            Some(i) => FaderRef::Dca(parse_int_prefix(i)),
            None => FaderRef::Strip(StripRef::parse(s)),
        }
    }
}

impl fmt::Display for FaderRef {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            FaderRef::Strip(r) => r.fmt(f),
            FaderRef::Dca(i) => write!(f, "dca:{i}"),
        }
    }
}

impl From<StripRef> for FaderRef {
    fn from(r: StripRef) -> Self {
        FaderRef::Strip(r)
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
    /// Stable: "in-1", "bus-1", "dca-1", "out-1", "all", "user-<uuid>".
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
pub const DEFAULT_SELECTION: [&str; NUM_SECTIONS] = ["in-1", "bus-1", "out-1"];

#[derive(Debug, Clone)]
pub struct LayerSet {
    widths: [i32; NUM_SECTIONS],
    fitted: [i32; NUM_SECTIONS],
    user_layers: Vec<Layer>,
    selected: [String; NUM_SECTIONS],
}

impl Default for LayerSet {
    fn default() -> Self {
        Self { widths: WING_WIDTHS, fitted: WING_WIDTHS, user_layers: Vec::new(), selected: DEFAULT_SELECTION.map(String::from) }
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

fn add_banks(layers: &mut Vec<Layer>, faders: &[Labelled], width: usize, id_prefix: &str) {
    for (bank, chunk) in faders.chunks(width.max(1)).enumerate() {
        layers.push(Layer {
            id: format!("{id_prefix}-{}", bank + 1),
            name: bank_name(chunk),
            faders: chunk.iter().map(|l| l.fader).collect(),
            user: false,
        });
    }
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

    pub fn default_layers_for(&self, section: usize, console: &ConsoleRefs) -> Vec<Layer> {
        let width = self.effective_width(section) as usize;
        let mut layers = Vec::new();

        let inputs: Vec<Labelled> = labelled(&console.channels, "CH").chain(labelled(&console.auxes, "AUX")).collect();
        if inputs.is_empty() {
            layers.push(Layer { id: "in-1".into(), name: "CH".into(), faders: Vec::new(), user: false });
        } else {
            add_banks(&mut layers, &inputs, width, "in");
        }
        let buses: Vec<Labelled> = labelled(&console.buses, "BUS").collect();
        add_banks(&mut layers, &buses, width, "bus");
        let dcas: Vec<Labelled> =
            (0..console.num_dcas).map(|i| Labelled { fader: FaderRef::Dca(i as i32), label: "DCA", number: i + 1 }).collect();
        add_banks(&mut layers, &dcas, width, "dca");
        let outputs: Vec<Labelled> = labelled(&console.mains, "MAIN").chain(labelled(&console.matrices, "MTX")).collect();
        add_banks(&mut layers, &outputs, width, "out");

        let all = inputs.iter().chain(&buses).chain(&outputs).chain(&dcas).map(|l| l.fader).collect();
        layers.push(Layer { id: "all".into(), name: "ALL".into(), faders: all, user: false });
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

    /// Finds a layer by id (falls back to the section's first layer if it no longer exists).
    pub fn find_layer(&self, engine: &Engine, section: usize, id: &str) -> Layer {
        let mut layers = self.all_layers(engine, section);
        match layers.iter().position(|l| l.id == id) {
            Some(i) => layers.swap_remove(i),
            None => layers.swap_remove(0),
        }
    }

    /// The layer a section shows now.
    pub fn selected_layer(&self, engine: &Engine, section: usize) -> Layer {
        self.find_layer(engine, section, self.selected(section))
    }

    /// The default bank of a section that contains `fader` (not ALL).
    pub fn find_default_layer_containing(&self, engine: &Engine, section: usize, fader: &FaderRef) -> Option<String> {
        self.default_layers(engine, section).into_iter().find(|l| l.id != "all" && l.faders.contains(fader)).map(|l| l.id)
    }

    pub fn selected(&self, section: usize) -> &str {
        self.selected.get(section).map_or(DEFAULT_SELECTION[0], String::as_str)
    }

    pub fn select(&mut self, section: usize, id: &str) {
        if let Some(s) = self.selected.get_mut(section) {
            *s = id.to_owned();
        }
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
    pub fn add_user_layer(&mut self, name: &str, faders: Vec<FaderRef>) -> String {
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

    /// Adds a fader to a user layer (no duplicates).
    pub fn add_to_user_layer(&mut self, id: &str, f: FaderRef) {
        if let Some(l) = self.user_layer_mut(id) {
            if !l.faders.contains(&f) {
                l.faders.push(f);
            }
        }
    }

    pub fn remove_from_user_layer(&mut self, id: &str, f: &FaderRef) {
        if let Some(l) = self.user_layer_mut(id) {
            l.faders.retain(|x| x != f);
        }
    }

    pub fn move_within_user_layer(&mut self, id: &str, index: i32, delta: i32) {
        if let Some(l) = self.user_layer_mut(id) {
            let size = l.faders.len() as i64;
            let (i, t) = (i64::from(index), i64::from(index) + i64::from(delta));
            if (0..size).contains(&i) && (0..size).contains(&t) {
                l.faders.swap(i as usize, t as usize);
            }
        }
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
            self.user_layers.push(Layer {
                id,
                name: e.str_or("name", "User").to_owned(),
                faders: faders_from_string(e.str_or("faders", "")),
                user: true,
            });
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

    #[test]
    fn wing_default_banks() {
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
                ("bus-1", "BUS 1-12"),
                ("bus-2", "BUS 13-16"),
                ("dca-1", "DCA 1-12"),
                ("dca-2", "DCA 13-16"),
                ("out-1", "MAIN 1-4 · MTX 1-8"),
                ("all", "ALL"),
            ]
        );
        assert_eq!(left[3].faders[4], FaderRef::Strip(StripRef::new(StripKind::Aux, 0)));
        assert_eq!(left.last().unwrap().faders.len(), 40 + 8 + 16 + 4 + 8 + 16);

        let center = set.default_layers_for(CENTER, &console);
        assert_eq!(center.iter().find(|l| l.id == "bus-1").unwrap().name, "BUS 1-8");
        assert_eq!(center.iter().find(|l| l.id == "dca-2").unwrap().faders[0], FaderRef::Dca(8));

        let right = set.default_layers_for(RIGHT, &console);
        let out: Vec<_> = right.iter().filter(|l| l.id.starts_with("out")).map(|l| l.name.as_str()).collect();
        assert_eq!(out, ["MAIN 1-4", "MTX 1-4", "MTX 5-8"]);

        let empty = set.default_layers_for(LEFT, &ConsoleRefs::default());
        assert_eq!(empty[0].id, "in-1");
        assert_eq!(empty[0].name, "CH");
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

        loaded.remove_user_layer(&a);
        assert_eq!(loaded.user_layers().len(), 1);
        assert_eq!(FaderRef::parse("dca:15"), FaderRef::Dca(15));
        assert_eq!(faders_from_string("ch:3, dca:1"), vec![ch(3), FaderRef::Dca(1)]);
    }
}
