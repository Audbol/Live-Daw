//! Fader layers in the style of X32/Wing.
//!
//! Default layers are generated from the console whenever they're asked for, so
//! they always cover every strip: input banks (Ch 1-16, Ch 17-32, ...), aux
//! banks, group banks and "All". User layers hold any strips of the board
//! (inputs, buses, master) in any order and are saved with the show.

use crate::refs::{refs_from_string, refs_to_string, StripRef};
use crate::xml::Element;
use livedaw_engine::{Engine, StripKind};
use std::collections::hash_map::RandomState;
use std::hash::{BuildHasher, Hasher};
use std::sync::atomic::{AtomicU64, Ordering::Relaxed};
use std::time::{SystemTime, UNIX_EPOCH};

#[derive(Debug, Clone, PartialEq)]
pub struct Layer {
    /// Stable: "in-1", "aux-1", "groups-1", "all", "user-<uuid>".
    pub id: String,
    pub name: String,
    pub strips: Vec<StripRef>,
    pub user: bool,
}

/// Bank size 0 = Auto: each default layer holds as many strips as fit the mixer.
pub const AUTO_BANK_SIZE: i32 = 0;
pub const FALLBACK_BANK_SIZE: i32 = 16;
pub const DEFAULT_LAYER_ID: &str = "in-1";

#[derive(Debug, Clone)]
pub struct LayerSet {
    bank_size: i32,
    fitted_bank_size: i32,
    user_layers: Vec<Layer>,
    pub selected_layer_id: String,
}

impl Default for LayerSet {
    fn default() -> Self {
        Self {
            bank_size: AUTO_BANK_SIZE,
            fitted_bank_size: FALLBACK_BANK_SIZE,
            user_layers: Vec::new(),
            selected_layer_id: DEFAULT_LAYER_ID.into(),
        }
    }
}

/// The strips of the console in layer order, independent of the engine (for tests).
#[derive(Debug, Default, Clone)]
pub struct ConsoleRefs {
    pub inputs: Vec<StripRef>,
    pub auxes: Vec<StripRef>,
    pub groups: Vec<StripRef>,
}

impl ConsoleRefs {
    pub fn of(engine: &Engine) -> Self {
        let mut c = ConsoleRefs { inputs: engine.inputs().iter().map(|s| StripRef::of(s)).collect(), ..Default::default() };
        for b in engine.buses() {
            match b.kind() {
                StripKind::Aux => c.auxes.push(StripRef::of(&b)),
                _ => c.groups.push(StripRef::of(&b)),
            }
        }
        c
    }
}

fn add_banks(layers: &mut Vec<Layer>, refs: &[StripRef], bank_size: usize, id_prefix: &str, name_prefix: &str) {
    let count = refs.len();
    for (bank, chunk) in refs.chunks(bank_size.max(1)).enumerate() {
        let start = bank * bank_size;
        let end = start + chunk.len();
        layers.push(Layer {
            id: format!("{id_prefix}-{}", bank + 1),
            name: if count <= bank_size { name_prefix.to_owned() } else { format!("{name_prefix} {}-{end}", start + 1) },
            strips: chunk.to_vec(),
            user: false,
        });
    }
}

impl LayerSet {
    pub fn bank_size(&self) -> i32 {
        self.bank_size
    }

    pub fn set_bank_size(&mut self, size: i32) {
        self.bank_size = if size <= 0 { AUTO_BANK_SIZE } else { size.clamp(4, 256) };
    }

    pub fn is_auto_bank_size(&self) -> bool {
        self.bank_size == AUTO_BANK_SIZE
    }

    /// How many strips fit the mixer right now (used when the bank size is Auto).
    pub fn set_fitted_bank_size(&mut self, size: i32) {
        self.fitted_bank_size = size.clamp(4, 256);
    }

    pub fn effective_bank_size(&self) -> i32 {
        if self.is_auto_bank_size() {
            self.fitted_bank_size
        } else {
            self.bank_size
        }
    }

    pub fn default_layers_for(&self, console: &ConsoleRefs) -> Vec<Layer> {
        let bank = self.effective_bank_size() as usize;
        let mut layers = Vec::new();

        if console.inputs.is_empty() {
            layers.push(Layer { id: DEFAULT_LAYER_ID.into(), name: "Inputs".into(), strips: Vec::new(), user: false });
        } else {
            add_banks(&mut layers, &console.inputs, bank, "in", "Ch");
        }
        add_banks(&mut layers, &console.auxes, bank, "aux", "Aux");
        add_banks(&mut layers, &console.groups, bank, "groups", "Groups");

        let all = console.inputs.iter().chain(&console.auxes).chain(&console.groups).copied().collect();
        layers.push(Layer { id: "all".into(), name: "All".into(), strips: all, user: false });
        layers
    }

    pub fn default_layers(&self, engine: &Engine) -> Vec<Layer> {
        self.default_layers_for(&ConsoleRefs::of(engine))
    }

    /// Default layers followed by user layers.
    pub fn all_layers(&self, engine: &Engine) -> Vec<Layer> {
        let mut layers = self.default_layers(engine);
        layers.extend(self.user_layers.iter().cloned());
        layers
    }

    /// Finds a layer by id (falls back to the first layer if it no longer exists).
    pub fn find_layer(&self, engine: &Engine, id: &str) -> Layer {
        let mut layers = self.all_layers(engine);
        match layers.iter().position(|l| l.id == id) {
            Some(i) => layers.swap_remove(i),
            None => layers.swap_remove(0),
        }
    }

    /// The default layer of the same family that contains `r` (not "All").
    pub fn find_default_layer_containing(&self, engine: &Engine, r: &StripRef) -> Option<String> {
        self.default_layers(engine).into_iter().find(|l| l.id != "all" && l.strips.contains(r)).map(|l| l.id)
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
    pub fn add_user_layer(&mut self, name: &str, strips: Vec<StripRef>) -> String {
        let id = format!("user-{}", new_uuid());
        self.user_layers.push(Layer { id: id.clone(), name: name.into(), strips, user: true });
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

    /// Adds a strip to a user layer (no duplicates).
    pub fn add_to_user_layer(&mut self, id: &str, r: StripRef) {
        if let Some(l) = self.user_layer_mut(id) {
            if !l.strips.contains(&r) {
                l.strips.push(r);
            }
        }
    }

    pub fn remove_from_user_layer(&mut self, id: &str, r: &StripRef) {
        if let Some(l) = self.user_layer_mut(id) {
            l.strips.retain(|x| x != r);
        }
    }

    pub fn move_within_user_layer(&mut self, id: &str, index: i32, delta: i32) {
        if let Some(l) = self.user_layer_mut(id) {
            let size = l.strips.len() as i64;
            let (i, t) = (i64::from(index), i64::from(index) + i64::from(delta));
            if (0..size).contains(&i) && (0..size).contains(&t) {
                l.strips.swap(i as usize, t as usize);
            }
        }
    }

    // ---- persistence ---------------------------------------------------------------

    pub fn to_xml(&self) -> Element {
        let mut xml = Element::new("Layers");
        xml.set("bankSize", self.bank_size).set("selected", &self.selected_layer_id);
        for l in &self.user_layers {
            xml.add_new("Layer").set("id", &l.id).set("name", &l.name).set("strips", refs_to_string(&l.strips));
        }
        xml
    }

    pub fn from_xml(&mut self, xml: &Element) {
        self.reset();
        self.set_bank_size(xml.int_or("bankSize", AUTO_BANK_SIZE));
        self.selected_layer_id = xml.str_or("selected", DEFAULT_LAYER_ID).to_owned();

        for e in xml.children_named("Layer") {
            let mut id = e.str_or("id", "").to_owned();
            if !id.starts_with("user-") {
                id = format!("user-{}", new_uuid());
            }
            self.user_layers.push(Layer {
                id,
                name: e.str_or("name", "User").to_owned(),
                strips: refs_from_string(e.str_or("strips", "")),
                user: true,
            });
        }
    }

    pub fn reset(&mut self) {
        let fitted = self.fitted_bank_size; // a property of the window, not the show
        *self = Self { fitted_bank_size: fitted, ..Self::default() };
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

    fn console(inputs: i32, auxes: i32, groups: i32) -> ConsoleRefs {
        ConsoleRefs {
            inputs: (1..=inputs).map(|i| StripRef::new(StripKind::Input, i)).collect(),
            auxes: (0..auxes).map(|i| StripRef::new(StripKind::Aux, i)).collect(),
            groups: (auxes..auxes + groups).map(|i| StripRef::new(StripKind::Group, i)).collect(),
        }
    }

    #[test]
    fn default_banks_cover_everything() {
        let mut set = LayerSet::default();
        set.set_bank_size(16);
        let layers = set.default_layers_for(&console(40, 3, 0));
        let ids: Vec<_> = layers.iter().map(|l| l.id.as_str()).collect();
        assert_eq!(ids, ["in-1", "in-2", "in-3", "aux-1", "all"]);
        assert_eq!(layers[0].name, "Ch 1-16");
        assert_eq!(layers[2].name, "Ch 33-40");
        assert_eq!(layers[2].strips.len(), 8);
        assert_eq!(layers[3].name, "Aux");
        assert_eq!(layers[4].strips.len(), 43);

        let empty = set.default_layers_for(&ConsoleRefs::default());
        assert_eq!(empty[0].id, "in-1");
        assert_eq!(empty[0].name, "Inputs");
    }

    #[test]
    fn bank_size_limits_and_auto() {
        let mut set = LayerSet::default();
        assert!(set.is_auto_bank_size());
        assert_eq!(set.effective_bank_size(), FALLBACK_BANK_SIZE);
        set.set_fitted_bank_size(1000);
        assert_eq!(set.effective_bank_size(), 256);
        set.set_bank_size(2);
        assert_eq!(set.bank_size(), 4);
        set.set_bank_size(-1);
        assert!(set.is_auto_bank_size());
    }

    #[test]
    fn user_layers_round_trip() {
        let mut set = LayerSet::default();
        let a = set.add_user_layer("Drums", vec![StripRef::new(StripKind::Input, 1)]);
        let b = set.add_user_layer("Vox", vec![]);
        assert_ne!(a, b);
        set.add_to_user_layer(&b, StripRef::new(StripKind::Master, 0));
        set.add_to_user_layer(&b, StripRef::new(StripKind::Master, 0));
        set.add_to_user_layer(&b, StripRef::new(StripKind::Aux, 2));
        set.move_within_user_layer(&b, 0, 1);
        set.move_user_layer(&b, -1);
        set.rename_user_layer(&b, "Vocals");
        assert_eq!(set.user_layers()[0].name, "Vocals");
        set.selected_layer_id = b.clone();

        let mut loaded = LayerSet::default();
        loaded.from_xml(&set.to_xml());
        assert_eq!(loaded.user_layers(), set.user_layers());
        assert_eq!(loaded.user_layers()[0].strips, vec![StripRef::new(StripKind::Aux, 2), StripRef::new(StripKind::Master, 0)]);
        assert_eq!(loaded.selected_layer_id, b);

        loaded.remove_user_layer(&a);
        assert_eq!(loaded.user_layers().len(), 1);
    }
}
