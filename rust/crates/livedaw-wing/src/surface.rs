//! The surface's fader layers and custom controls for remote clients
//! (`/$ctl/layer`, `/$ctl/user`).
//!
//! They live in the show (livedaw-session's `LayerSet` and `CustomControls`),
//! which belongs to the host's message thread. The host publishes a
//! [`Surface`] (the show in the WING's terms) and applies the [`SurfaceEdit`]s
//! clients queue, both in [`sync`]; until then a client reads back what it
//! set. Without a host the nodes live in the shadow store.
//!
//! What the nodes mean, from the protocol document's `/$ctl` tables and
//! libwing's tree (guesses are marked):
//!
//! - `L`, `C`, `R` are the three fader sections (`CMPCT`, `RCK` and `EXT`,
//!   the other models' surfaces, answer the WING's factory values and keep
//!   what a client sets). `VRT` is the COMPACT's (and RACK's) virtual
//!   section: its eight layers `1`-`8` are Sidecar's virtual layers (names
//!   and faders, on either model); `sel` and `ofs` are the COMPACT's VIRTUAL
//!   section's while the console is a COMPACT (else they read 1 and 0).
//!   `sel` 1-7
//!   picks a layer key in the console's order (left: CH 1-12, 13-24, 25-36,
//!   37-40 / AUX, BUSES, USER 1, USER 2; center: DCA, MAIN, AUX, BUSES, USER 1,
//!   USER 2; right: MAIN, DCA, CH 1-40, AUX, BUSES, USER 1, USER 2) and reads
//!   22 while the section spills. 8-21 (no-ops and the console's fixed banks)
//!   are ignored; a layer without a WING key (the center's INPUTS, a user
//!   layer selected by itself) reads as 8.
//! - `spidx`: 0 no spill, 1-16 DCA, 17-32 FX slot, 33-48 a bus's sources,
//!   57-60 the channels, aux and buses feeding a main, 61 / 62 automix X /
//!   Y, 63 a strip's sends (read only: which strip is the console's
//!   selection). Sidecar has no matrix spill (49-56).
//! - `N/ofs`: the scroll position. Sidecar keeps one per section, so only the
//!   shown layer's is read and set (the others read 0).
//! - `N/name`: the USER keys show the first two user layers, so naming one
//!   names the user layer; the other keys' names are generated.
//! - `N/M/type`, `i`, `dst`: fader M of the layer. The encoding is a guess:
//!   CH and the strip's number among channels and aux (1-48), BUS and its
//!   number among buses, mains and matrices (1-28, as `dst`); a CH number
//!   above 48 counts on through buses, mains and matrices (the 1-76 strip
//!   order the tree uses elsewhere). DCA 1-16; SEND: from strip `i` (1-76) to
//!   `dst` (buses 1-16, mains 17-20, matrices 21-28); FX: slot `i`, parameter
//!   `dst`; MIDI: CC `i` on MIDI channel `dst`. SENDS has no equivalent.
//!   Slots are fixed, as on the console: OFF is a blank slot, a fader set
//!   past the end leaves blanks before it, and nothing else moves. A layer can
//!   hold the same fader in several slots; each reads and sets on its own.
//!   Changing a default key edits it (as VIEW > CONFIG does); the USER keys
//!   change the user layers.
//! - `$spill`: the spill a section shows, read only (the console too
//!   replaces what is set there).
//! - `/$ctl/user/1..16/1..4`: the upper part's custom control layers;
//!   `user/1..4` the lower part's USER layer, `daw1..4` its DAW layers.
//!   `U1..U4`, `MM` and `D1..D4` are the WING COMPACT's key sets (WING
//!   Edit 3.3's CC EDIT on a Compact: USER 1-4 .. USER 13-16, MAIN/MTX 5-8,
//!   DCA 1-4 .. DCA 13-16; the document: "U1..4: User, MM: Main/Matrix,
//!   D1..4: DCA"), four upper keys each with a colour, on either model (WING
//!   Edit lists them on the full-size WING too, greyed; the manual: "all
//!   layers are configurable on any model"). The full-size WING has no
//!   layers of its own for the 4-Channel section: its F1-F3 play layers 1-3
//!   (manual 5.5 p. 48). `mode` of `enc`, `bu` and `bd` is the
//!   function; a new function starts at its first target, as on CC EDIT. The
//!   function's targets and options are the mode's own nodes (`ch`, `fx`,
//!   `par`, `other`, `area`, `bank` ...: the schema's custom control slots),
//!   read and set through [`Surface::get_cc_param`] and
//!   [`Surface::set_cc_param`]; a node Sidecar has no equivalent for keeps
//!   what a client sets. A control has one name (`enc/name`, `bu/name` where
//!   there is no encoder) and a colour; `$fname` reads the function's name.
//!   `sel`: the upper part's layer (1-16; the document says the console
//!   answers 0-15).

use crate::map::{SetError, WingState};
use crate::value::Value;
use livedaw_engine::params::{NUM_DCAS, NUM_FX};
use livedaw_engine::{Engine, StripKind};
use livedaw_session::custom::{
    daw_command_group, other, CcFunction, CustomControls, CC_BANK_CATEGORY, COMPACT_MAIN_MATRIX_LAYER, FADER_SPILL_CATEGORY,
    FIRST_COMPACT_DCA_LAYER, FIRST_COMPACT_USER_LAYER, FIRST_DAW_LAYER, FX_PARAMETER_CATEGORY, INSERT_POST_CATEGORY,
    INSERT_PRE_CATEGORY, NUM_COMPACT_SETS, NUM_CONTROLS, NUM_LAYERS, OTHER_CATEGORY, OVERVIEW_PAGE_CATEGORY, USER_LAYER,
};
use livedaw_session::layers::{
    slots, ConsoleRefs, FaderRef, Layer, LayerSet, SpillKind, NUM_SECTIONS, NUM_VIRTUAL_LAYERS, VIRTUAL_KEYS, VIRTUAL_SECTION,
};
use livedaw_session::refs::StripRef;
use std::collections::VecDeque;

/// The layer keys of each section in the WING's order, as Sidecar's layer ids.
pub const KEYS: [&[&str]; NUM_SECTIONS] = [
    &["in-1", "in-2", "in-3", "in-4", "bus-1", "user-1", "user-2"],
    &["dca-1", "out-1", "aux", "bus-1", "user-1", "user-2"],
    &["out-1", "dca-1", "inputs", "aux", "bus-1", "user-1", "user-2"],
];
/// The names a factory-initialized WING gives the layer keys (`N/name`;
/// the USER keys show the user layers' own names).
const KEY_NAMES: [&[&str]; NUM_SECTIONS] = [
    &["CH1-12", "CH13-24", "CH25-36", "CH37-AUX", "BUSES", "", ""],
    &["DCA", "MAIN", "AUX", "BUSES", "", ""],
    &["MAIN", "DCA", "CH1-40", "AUX", "BUSES", "", ""],
];
/// `/$ctl/layer/VRT`: the virtual section, after L, C and R.
pub const VRT: usize = NUM_SECTIONS;
const NUM_SURFACE_SECTIONS: usize = NUM_SECTIONS + 1;

/// The layer keys of a surface section (`VRT`: the virtual layers).
fn keys(section: usize) -> &'static [&'static str] {
    KEYS.get(section).copied().unwrap_or(&VIRTUAL_KEYS)
}

/// `$spill/name` while a section spills nothing.
const SPILL_NAME: &str = "SPILL";
/// `sel` of a layer without a WING key, and of a spill.
const SEL_NO_KEY: i32 = 8;
const SEL_SPILL: i32 = 22;
/// `spidx` of a strip's sends (SOF).
const SPIDX_SENDS: i32 = 63;
/// The `$spill` positions (the WING's 76 strips).
pub const SPILL_POSITIONS: usize = 76;
/// The edits a host that stopped polling can leave queued (a snapshot import
/// sets every node).
const MAX_EDITS: usize = 8192;

/// A fader node's TYPE items.
pub const FADER_TYPES: [&str; 8] = ["OFF", "CH", "BUS", "DCA", "MIDI", "SEND", "FX", "SENDS"];
const T_OFF: u8 = 0;
const T_CH: u8 = 1;
const T_BUS: u8 = 2;
const T_DCA: u8 = 3;
const T_MIDI: u8 = 4;
const T_SEND: u8 = 5;
const T_FX: u8 = 6;

/// A `mode` item, its long name and Sidecar's function for it, if any.
type ModeRow<F> = (&'static str, &'static str, Option<F>);

/// The encoder functions (`enc/mode`): item, long name, Sidecar's encoder
/// category (CC EDIT's FUNCTION list), if it has one.
const ENC_MODES: [ModeRow<i32>; 13] = [
    ("OFF", "NOT ASSIGNED", Some(0)),
    ("FDR", "FADER", Some(1)),
    ("PAN", "PAN", Some(2)),
    ("DCA", "DCA FADER", Some(3)),
    ("SSND", "SEL BUS SEND", Some(4)),
    ("FSND", "FIXED BUS SEND", Some(5)),
    ("FX", "FX PARAMETER", Some(6)),
    ("DAWMCU", "DAW V-POTS", Some(8)),
    ("MON", "MONITORING", Some(7)),
    ("OTHER", "OTHER", None),
    ("MIDICC", "MIDI CC", Some(9)),
    ("SD A", "SD-1 RECORDER", None),
    ("SD B", "SD-2 RECORDER", None),
];

/// The key functions (`bu/mode`, `bd/mode`): item, long name, Sidecar's key
/// category and, for MIDI keys (category 14), the MIDI key type (the target / 16).
const BTN_MODES: [ModeRow<(i32, i32)>; 33] = [
    ("OFF", "NOT ASSIGNED", Some((0, -1))),
    ("MUTE", "CH MUTE", Some((1, -1))),
    ("SOLO", "CH SOLO", Some((2, -1))),
    ("INS1", "CH INSERT PRE ON/OFF", Some((INSERT_PRE_CATEGORY, -1))),
    ("INS2", "CH INSERT POST ON/OFF", Some((INSERT_POST_CATEGORY, -1))),
    ("MGRP", "MUTE GROUP", Some((4, -1))),
    ("DCAMUTE", "DCA MUTE", Some((3, -1))),
    ("SOF", "SENDS ON FDR", Some((5, -1))),
    ("SPILL", "FADER SPILL", Some((12, -1))),
    ("FXPAR", "FX PAR", Some((FX_PARAMETER_CATEGORY, -1))),
    ("DAWBTN", "DAW BUTTONS", Some((13, -1))),
    ("DAWENC", "DAW V-POTS", None),
    ("CHPAGE", "CH PAGE", Some((6, -1))),
    ("PAGE", "GLOBAL PAGE", Some((7, -1))),
    ("FDRPAGE", "FADER PAGE", Some((8, -1))),
    ("VIEWPAGE", "OVERVIEW PAGE", Some((OVERVIEW_PAGE_CATEGORY, -1))),
    ("OTHER", "OTHER", Some((11, -1))),
    ("GPIO", "GPIO", Some((17, -1))),
    ("FSTART", "FADER START", None),
    ("SHOWCTL", "SHOW CONTROL", Some((9, -1))),
    ("SCENES", "SCENE RECALL", Some((10, -1))),
    ("MIDICCT", "MIDI CC (TOGGLE)", Some((MIDI_CATEGORY, 0))),
    ("MIDICCP", "MIDI CC (PUSH)", Some((MIDI_CATEGORY, 1))),
    ("MIDINT", "MIDI NOTE (TOGGLE)", Some((MIDI_CATEGORY, 2))),
    ("MIDINP", "MIDI NOTE (PUSH)", Some((MIDI_CATEGORY, 3))),
    ("MIDIPGM", "MIDI PGM CHANGE", Some((MIDI_CATEGORY, 4))),
    ("USBPR", "USB PLAY/REC", Some((15, -1))),
    ("SDRECA", "SD-1 RECORDER", Some((16, -1))),
    ("SESSIONA", "SD-1 SESSION", None),
    ("MARKERA", "SD-1 MARKER", None),
    ("SDRECB", "SD-2 RECORDER", None),
    ("SESSIONB", "SD-2 SESSION", None),
    ("MARKERB", "SD-2 MARKER", None),
];
const MIDI_CATEGORY: i32 = 14;
const OTHER_MODE: usize = 16;
/// The upper part's layers (`sel`).
const NUM_BANKS: i32 = 16;

// ---- bindings ------------------------------------------------------------------------

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FaderField {
    Type,
    Index,
    Dest,
}

/// `/$ctl/layer/<L|C|R>/...` (the section is in the binding).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LayerF {
    Sel,
    SpillIndex,
    /// Layer key (0-based).
    Offset(u8),
    Name(u8),
    /// Layer key, position (0-based).
    Fader(u8, u8, FaderField),
    SpillOffset,
    SpillName,
    SpillFader(u8, FaderField),
}

/// A custom control's encoder or one of its keys.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CcSlot {
    Enc,
    Up,
    Down,
}

/// `/$ctl/user/...`: layer (`CustomControls`' numbering) and control, 0-based.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum UserF {
    Bank,
    Colour(u8, u8),
    Mode(u8, u8, CcSlot),
    Name(u8, u8),
    FName(u8, u8, CcSlot),
    /// The `enc`, `bu` or `bd` node itself (its mode's own nodes are the
    /// schema's model parameters).
    Slot(u8, u8, CcSlot),
}

/// Binds `/$ctl/layer/<rest>`.
pub fn bind_layer(rest: &[&str]) -> Option<(u8, LayerF)> {
    let (section, rest) = rest.split_first()?;
    let s = ["L", "C", "R", "VRT"].iter().position(|n| n == section)?;
    let number = |p: &str, max: usize| p.parse::<usize>().ok().filter(|n| (1..=max).contains(n)).map(|n| (n - 1) as u8);
    let field = |f: &str| match f {
        "type" => Some(FaderField::Type),
        "i" => Some(FaderField::Index),
        "dst" => Some(FaderField::Dest),
        _ => None,
    };
    let keys = keys(s).len();
    let f = match rest {
        ["sel"] => LayerF::Sel,
        ["spidx"] => LayerF::SpillIndex,
        ["$spill", "ofs"] => LayerF::SpillOffset,
        ["$spill", "name"] => LayerF::SpillName,
        ["$spill", n, f] => LayerF::SpillFader(number(n, SPILL_POSITIONS)?, field(f)?),
        [k, "ofs"] => LayerF::Offset(number(k, keys)?),
        [k, "name"] => LayerF::Name(number(k, keys)?),
        [k, n, f] => LayerF::Fader(number(k, keys)?, number(n, 255)?, field(f)?),
        _ => return None,
    };
    Some((s as u8, f))
}

/// Binds `/$ctl/user/<rest>`. `U1`-`U4`, `MM` and `D1`-`D4` bind to the
/// COMPACT's key sets; the full-size console reads them as before (see
/// [`UserF::for_model`]).
pub fn bind_user(rest: &[&str]) -> Option<UserF> {
    if rest == ["sel"] {
        return Some(UserF::Bank);
    }
    let [layer, control, tail @ ..] = rest else { return None };
    let set =
        |l: &str, prefix: &str| l.strip_prefix(prefix)?.parse::<usize>().ok().filter(|n| (1..=NUM_COMPACT_SETS).contains(n));
    let layer = match *layer {
        "user" => USER_LAYER,
        "MM" => COMPACT_MAIN_MATRIX_LAYER,
        l if l.starts_with("daw") => FIRST_DAW_LAYER + l[3..].parse::<usize>().ok().filter(|n| (1..=4).contains(n))? - 1,
        l if l.starts_with('U') => FIRST_COMPACT_USER_LAYER + set(l, "U")? - 1,
        l if l.starts_with('D') => FIRST_COMPACT_DCA_LAYER + set(l, "D")? - 1,
        l => l.parse::<usize>().ok().filter(|n| (1..=NUM_BANKS as usize).contains(n))? - 1,
    } as u8;
    let control = control.parse::<usize>().ok().filter(|n| (1..=NUM_CONTROLS).contains(n))? as u8 - 1;
    // The name lives on the encoder where there is one (the upper part's layers).
    let name_slot = if (layer as i32) < NUM_BANKS { "enc" } else { "bu" };
    // The lower part's layers (USER, DAW 1-4) have no colour; only they and
    // the upper part's layers have a lower key.
    let lower_part = layer == 16 || (FIRST_DAW_LAYER..FIRST_COMPACT_USER_LAYER).contains(&(layer as usize));
    let slot_of = |slot: &str| match slot {
        "enc" if (layer as i32) < NUM_BANKS => Some(CcSlot::Enc),
        "bu" => Some(CcSlot::Up),
        "bd" if (layer as i32) < NUM_BANKS || lower_part => Some(CcSlot::Down),
        _ => None,
    };
    Some(match tail {
        ["col"] if !lower_part => UserF::Colour(layer, control),
        [slot] => UserF::Slot(layer, control, slot_of(slot)?),
        [slot, f] => {
            let cs = match *slot {
                "enc" if (layer as i32) < NUM_BANKS => CcSlot::Enc,
                "bu" => CcSlot::Up,
                "bd" if (layer as i32) < NUM_BANKS || lower_part => CcSlot::Down,
                _ => return None,
            };
            match *f {
                "mode" => UserF::Mode(layer, control, cs),
                "$fname" => UserF::FName(layer, control, cs),
                "name" if *slot == name_slot => UserF::Name(layer, control),
                _ => return None,
            }
        }
        _ => return None,
    })
}

// ---- the published surface -----------------------------------------------------------

/// A fader position as the WING describes it: TYPE (index in [`FADER_TYPES`]),
/// FDR INDEX, DEST INDEX.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct WingFader {
    pub ty: u8,
    pub i: i32,
    pub dst: i32,
}

impl WingFader {
    pub const OFF: Self = Self { ty: T_OFF, i: 0, dst: 1 };
}

impl Default for WingFader {
    fn default() -> Self {
        Self::OFF
    }
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct SurfaceLayer {
    pub name: String,
    /// Every fader of Sidecar's layer (the tree shows the first 16, 24 or 40).
    pub faders: Vec<WingFader>,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct SurfaceSection {
    /// `sel` and `spidx`.
    pub sel: i32,
    pub spill: i32,
    /// The scroll position of the shown layer (or spill).
    pub offset: i32,
    /// The WING's layer keys.
    pub keys: Vec<SurfaceLayer>,
    pub spill_layer: SurfaceLayer,
}

#[derive(Clone, Debug, PartialEq)]
pub struct SurfaceControl {
    /// The encoder's, upper and lower key's functions as `mode` items.
    pub modes: [&'static str; 3],
    /// ... and as Sidecar's functions (their targets and options).
    pub functions: [CcFunction; 3],
    pub name: String,
    pub colour: i32,
}

impl Default for SurfaceControl {
    fn default() -> Self {
        Self { modes: ["OFF"; 3], functions: Default::default(), name: String::new(), colour: 0 }
    }
}

/// The show's layers and custom controls in the WING's terms.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Surface {
    /// A host applies the edits (else the nodes are shadow).
    pub attached: bool,
    /// L, C, R and VRT.
    pub sections: [SurfaceSection; NUM_SURFACE_SECTIONS],
    /// `CustomControls`' layers.
    pub controls: Vec<[SurfaceControl; NUM_CONTROLS]>,
    /// The upper part's layer, 0-15.
    pub cc_bank: i32,
    /// The strips' WING numbers (custom control functions name strips 1-76).
    num: Numbering,
}

/// A change a client made, for the host to apply to the show.
#[derive(Clone, Debug, PartialEq)]
pub enum SurfaceEdit {
    Select {
        section: usize,
        key: usize,
    },
    Spill {
        section: usize,
        spill: i32,
    },
    Offset {
        section: usize,
        offset: i32,
    },
    /// USER 1 / USER 2 (0, 1): the user layer's name.
    RenameUser {
        user: usize,
        name: String,
    },
    /// A virtual layer's (0-7) name.
    RenameVirtual {
        index: usize,
        name: String,
    },
    Fader {
        section: usize,
        key: usize,
        pos: usize,
        fader: WingFader,
    },
    /// A key's or encoder's function: a new mode or a new target or option.
    Function {
        layer: usize,
        index: usize,
        slot: CcSlot,
        function: CcFunction,
    },
    Name {
        layer: usize,
        index: usize,
        name: String,
    },
    Colour {
        layer: usize,
        index: usize,
        colour: i32,
    },
    Bank(i32),
}

impl WingState {
    pub(crate) fn push_surface_edit(&self, e: SurfaceEdit) {
        let mut q = self.surface_edits.lock().unwrap_or_else(|e| e.into_inner());
        if q.len() < MAX_EDITS {
            q.push_back(e);
        }
    }

    fn take_surface_edits(&self) -> VecDeque<SurfaceEdit> {
        std::mem::take(&mut *self.surface_edits.lock().unwrap_or_else(|e| e.into_inner()))
    }

    /// The surface as published (with the edits clients made since).
    pub fn surface(&self) -> std::sync::MutexGuard<'_, Surface> {
        self.surface.lock().unwrap_or_else(|e| e.into_inner())
    }
}

fn user_key(id: &str) -> Option<usize> {
    livedaw_session::layers::USER_KEYS.iter().position(|k| *k == id)
}

fn cc_slot_index(slot: CcSlot) -> usize {
    match slot {
        CcSlot::Enc => 0,
        CcSlot::Up => 1,
        CcSlot::Down => 2,
    }
}

fn mode_long_name(slot: CcSlot, mode: &str) -> &'static str {
    let found = if slot == CcSlot::Enc {
        ENC_MODES.iter().find(|m| m.0 == mode).map(|m| m.1)
    } else {
        BTN_MODES.iter().find(|m| m.0 == mode).map(|m| m.1)
    };
    found.unwrap_or("NOT ASSIGNED")
}

impl Surface {
    fn section(&self, s: u8) -> Option<&SurfaceSection> {
        self.sections.get(s as usize)
    }

    /// A `/$ctl/layer` node's value.
    pub fn get_layer(&self, s: u8, f: LayerF) -> Option<Value> {
        let sec = self.section(s)?;
        let fader = |l: &SurfaceLayer, pos: u8, field: FaderField| {
            let w = l.faders.get(pos as usize).copied().unwrap_or_default();
            match field {
                FaderField::Type => Value::str(FADER_TYPES[w.ty as usize]),
                FaderField::Index => Value::Int(w.i),
                FaderField::Dest => Value::Int(w.dst),
            }
        };
        let shown = |k: u8| sec.spill == 0 && sec.sel == k as i32 + 1;
        Some(match f {
            LayerF::Sel => Value::Int(sec.sel),
            LayerF::SpillIndex => Value::Int(sec.spill),
            LayerF::Offset(k) => Value::Int(if shown(k) { sec.offset } else { 0 }),
            LayerF::Name(k) => Value::Str(sec.keys.get(k as usize)?.name.clone()),
            LayerF::Fader(k, pos, field) => fader(sec.keys.get(k as usize)?, pos, field),
            LayerF::SpillOffset => Value::Int(if sec.spill != 0 { sec.offset } else { 0 }),
            LayerF::SpillName => Value::Str(sec.spill_layer.name.clone()),
            LayerF::SpillFader(pos, field) => fader(&sec.spill_layer, pos, field),
        })
    }

    /// A client sets a `/$ctl/layer` node (`v` coerced): what to apply, if
    /// anything. The surface shows it at once.
    pub fn set_layer(&mut self, s: u8, f: LayerF, v: &Value) -> Result<Option<SurfaceEdit>, SetError> {
        let section = s as usize;
        let sec = self.sections.get_mut(section).ok_or(SetError::NotFound)?;
        let int = v.as_i32().unwrap_or(0);
        Ok(match f {
            LayerF::Sel => {
                let key = usize::try_from(int - 1).ok().filter(|&k| k < keys(section).len());
                key.map(|key| {
                    sec.sel = int;
                    sec.spill = 0;
                    sec.offset = 0;
                    SurfaceEdit::Select { section, key }
                })
            }
            // The virtual section doesn't spill here.
            LayerF::SpillIndex if section == VRT => None,
            LayerF::SpillIndex => spill_kind(int, None).map(|_| {
                sec.spill = int;
                sec.sel = if int == 0 { sec.sel } else { SEL_SPILL };
                SurfaceEdit::Spill { section, spill: int }
            }),
            LayerF::Offset(k) => (sec.spill == 0 && sec.sel == k as i32 + 1).then(|| {
                sec.offset = int.max(0);
                SurfaceEdit::Offset { section, offset: int.max(0) }
            }),
            LayerF::SpillOffset => (sec.spill != 0).then(|| {
                sec.offset = int.max(0);
                SurfaceEdit::Offset { section, offset: int.max(0) }
            }),
            LayerF::Name(k) if section == VRT => {
                let name = text(v);
                sec.keys[k as usize].name.clone_from(&name);
                Some(SurfaceEdit::RenameVirtual { index: k as usize, name })
            }
            LayerF::Name(k) => user_key(KEYS[section][k as usize]).map(|user| {
                let name = text(v);
                sec.keys[k as usize].name.clone_from(&name);
                SurfaceEdit::RenameUser { user, name }
            }),
            LayerF::Fader(k, pos, field) => {
                let layer = sec.keys.get_mut(k as usize).ok_or(SetError::NotFound)?;
                let pos = pos as usize;
                if layer.faders.len() <= pos {
                    layer.faders.resize(pos + 1, WingFader::OFF);
                }
                let w = &mut layer.faders[pos];
                match field {
                    FaderField::Type => {
                        let name = match v {
                            Value::Str(s) => s.as_str(),
                            _ => "",
                        };
                        w.ty = FADER_TYPES.iter().position(|t| *t == name).ok_or(SetError::BadValue)? as u8;
                    }
                    FaderField::Index => w.i = int,
                    FaderField::Dest => w.dst = int,
                }
                Some(SurfaceEdit::Fader { section, key: k as usize, pos, fader: *w })
            }
            // The spill shows what it spills.
            LayerF::SpillName | LayerF::SpillFader(..) => None,
        })
    }

    /// A `/$ctl/user` node's value.
    pub fn get_user(&self, f: UserF) -> Option<Value> {
        let control = |l: u8, c: u8| self.controls.get(l as usize).and_then(|l| l.get(c as usize));
        Some(match f {
            UserF::Bank => Value::Int(self.cc_bank + 1),
            UserF::Colour(l, c) => Value::Int(control(l, c)?.colour.max(1)),
            UserF::Mode(l, c, slot) => Value::str(control(l, c)?.modes[cc_slot_index(slot)]),
            UserF::Name(l, c) => Value::Str(control(l, c)?.name.clone()),
            UserF::FName(l, c, slot) => {
                // A control not assigned has no function name, as on the console.
                let mode = control(l, c)?.modes[cc_slot_index(slot)];
                Value::str(if mode == "OFF" { "" } else { mode_long_name(slot, mode) })
            }
            UserF::Slot(..) => return None,
        })
    }

    /// A client sets a `/$ctl/user` node (`v` coerced).
    pub fn set_user(&mut self, f: UserF, v: &Value) -> Result<Option<SurfaceEdit>, SetError> {
        if f == UserF::Bank {
            let bank = (v.as_i32().unwrap_or(1) - 1).clamp(0, NUM_BANKS - 1);
            self.cc_bank = bank;
            return Ok(Some(SurfaceEdit::Bank(bank)));
        }
        let (UserF::Colour(l, c) | UserF::Mode(l, c, _) | UserF::Name(l, c) | UserF::FName(l, c, _)) = f else {
            return Err(SetError::NotFound);
        };
        let (layer, index) = (l as usize, c as usize);
        let num = &self.num;
        let control = self.controls.get_mut(layer).and_then(|l| l.get_mut(index)).ok_or(SetError::NotFound)?;
        Ok(Some(match f {
            UserF::Colour(..) => {
                control.colour = v.as_i32().unwrap_or(1).clamp(1, 18);
                SurfaceEdit::Colour { layer, index, colour: control.colour }
            }
            UserF::Mode(_, _, slot) => {
                let name = match v {
                    Value::Str(s) => s.as_str(),
                    _ => "",
                };
                let mode = if slot == CcSlot::Enc {
                    ENC_MODES.iter().find(|m| m.0 == name).map(|m| (m.0, m.2.is_some()))
                } else {
                    BTN_MODES.iter().find(|m| m.0 == name).map(|m| (m.0, m.2.is_some()))
                };
                let mode = match mode {
                    Some((mode, true)) => mode,
                    Some((mode, false)) => return Err(SetError::Unsupported(format!("no {mode} function here"))),
                    None => return Err(SetError::BadValue),
                };
                // Shown at once: the mode's own nodes follow it.
                let i = cc_slot_index(slot);
                let function = new_function(num, slot, mode, control.functions[i]).ok_or(SetError::BadValue)?;
                control.modes[i] = mode;
                control.functions[i] = function;
                SurfaceEdit::Function { layer, index, slot, function }
            }
            UserF::Name(..) => {
                control.name = match v {
                    Value::Str(s) => s.clone(),
                    other => other.as_i32().unwrap_or(0).to_string(),
                };
                SurfaceEdit::Name { layer, index, name: control.name.clone() }
            }
            _ => return Err(SetError::ReadOnly),
        }))
    }
}

impl Surface {
    /// A custom control function node's value: position `index` (1-based)
    /// of the mode of `slot` of control `c` of layer `l` (None: one Sidecar
    /// has no equivalent for).
    pub fn get_cc_param(&self, l: u8, c: u8, slot: CcSlot, index: u8) -> Option<Value> {
        let control = self.controls.get(l as usize)?.get(c as usize)?;
        let i = cc_slot_index(slot);
        let (mode, f) = (control.modes[i], control.functions[i]);
        if slot == CcSlot::Enc {
            encoder_param(&self.num, mode, &f, index)
        } else {
            key_param(&self.num, mode, &f, index)
        }
    }

    /// A client sets a custom control function node (`v` coerced): the
    /// function's new target or option. Ok(None): kept as a value only.
    pub fn set_cc_param(&mut self, l: u8, c: u8, slot: CcSlot, index: u8, v: &Value) -> Result<Option<SurfaceEdit>, SetError> {
        let num = &self.num;
        let control = self.controls.get_mut(l as usize).and_then(|l| l.get_mut(c as usize)).ok_or(SetError::NotFound)?;
        let i = cc_slot_index(slot);
        let (mode, f) = (control.modes[i], control.functions[i]);
        let new =
            if slot == CcSlot::Enc { set_encoder_param(num, mode, f, index, v)? } else { set_key_param(num, mode, f, index, v)? };
        let Some(function) = new else { return Ok(None) };
        control.functions[i] = function;
        Ok(Some(SurfaceEdit::Function { layer: l as usize, index: c as usize, slot, function }))
    }
}

// ---- custom control function nodes -----------------------------------------------------

fn text(v: &Value) -> String {
    match v {
        Value::Str(s) => s.clone(),
        other => other.as_i32().unwrap_or(0).to_string(),
    }
}

/// "FX3" -> 3 (the number after `prefix`).
fn numbered(v: &Value, prefix: &str) -> Option<i32> {
    text(v).strip_prefix(prefix)?.parse().ok()
}

fn strip_ref(target: i32) -> StripRef {
    StripRef::new(StripKind::from_i32(target / 100_000).unwrap_or(StripKind::Channel), target % 100_000)
}

/// A function's strip as the WING's number (1-76).
fn strip_number(num: &Numbering, target: i32) -> i32 {
    num.number(&strip_ref(target)).unwrap_or(1)
}

/// The WING's strip number as a function's strip.
fn strip_target(num: &Numbering, n: i32) -> Result<i32, SetError> {
    num.strip(n).map(|r| r.kind as i32 * 100_000 + r.id).ok_or_else(|| SetError::Unsupported(format!("no strip {n} here")))
}

/// CH PAGE: Sidecar's pages (HOME, INPUT, FILTER, GATE, EQ, DYNAMICS,
/// INSERT PRE, INSERT POST, MAIN, SENDS) as the WING's `pg` items.
const CH_PAGES: [&str; 10] = ["HOME", "INPUT", "FILT", "GATE", "EQ", "DYN", "INS1", "INS2", "MAIN", "SEND"];
/// GLOBAL PAGE: Sidecar's screens (HOME, EFFECTS, METERS, ROUTING, SETUP,
/// LIBRARY, MIXER, FADER and SENDS VIEW, FADER LAYER, MUTE GROUPS, WING
/// LIVE, CUSTOM CONTROLS, MONITOR) as `pg` items; HOME has none (answers FX).
const GLOBAL_PAGES: [&str; 14] =
    ["FX", "FX", "MTRS", "CHINS", "SETUP", "LIB", "MIXV", "FDRV", "SENDV", "LAYER", "MGRP", "WLIVE", "CUSTCTL", "MON"];
const SHOW_ACTIONS: [&str; 5] = ["GO", "PREV", "NEXT", "GOPREV", "GONEXT"];
/// USB PLAY/REC: Sidecar's keys (PLAY, PAUSE, STOP, PREV, NEXT, REC, REC
/// PAUSE, REC STOP, NEW FILE, REPEAT) as `usbpr` items (REPEAT has none).
const USB_KEYS: [&str; 10] = ["PLAY", "PPAUSE", "PSTOP", "PPREV", "PNEXT", "RECORD", "RPAUSE", "RSTOP", "RNEW", "PLAY"];
const AREAS: [&str; 7] = ["L", "C", "R", "CMPCT", "RCK", "EXT", "VRT"];
/// OTHER: Sidecar's functions (custom.rs `other`) as `other` items (SOF
/// OFF has none: TBA).
const OTHER_ITEMS: [&str; other::COUNT as usize] = [
    "MONDIM", "MONMONO", "MONSWAP", "MONMUTE", "FDROFF", "FDR-10DB", "FDR0DB", "AUTOX", "AUTOY", "CHPREV", "CHNEXT", "CCBANK",
    "CCBANK", "TBA", "TBA", "TBB", "ALTSRC", "DAWSW", "MONA", "MONB", "MONAB", "CHSOLO", "BUSSOLO", "MAINSOLO", "MTXSOLO",
    "CHMTR", "BUSMTR", "MAINMTR", "MTXMTR", "DCAMTR",
];
/// MON: Sidecar's options (MONITOR LEVEL, DIM AMOUNT, PHONES LEVEL) as `mon`.
const MONITOR_ITEMS: [&str; 3] = ["B", "B", "A"];

fn midi_type(mode: &str) -> Option<i32> {
    BTN_MODES.iter().find(|m| m.0 == mode).and_then(|m| m.2).filter(|(cat, _)| *cat == MIDI_CATEGORY).map(|(_, sub)| sub)
}

fn key_param(num: &Numbering, mode: &str, f: &CcFunction, index: u8) -> Option<Value> {
    let item = |s: &str| Some(Value::str(s));
    Some(match (mode, index) {
        ("MUTE" | "SOLO" | "INS1" | "INS2" | "SOF", 1) => Value::Int(strip_number(num, f.target)),
        ("MGRP", 1) => Value::Str(format!("MGRP.{}", f.target + 1)),
        ("DCAMUTE", 1) => Value::Int(f.target + 1),
        // Sidecar spills to the left section (as the screens' SPILL keys).
        ("SPILL", 1) => return item("L"),
        ("SPILL", 2) => {
            let (kind, i) = (f.target / 100, f.target % 100 + 1);
            Value::Str(match kind {
                0 => format!("DCA{i}"),
                1 => format!("BUS{i}"),
                2 => format!("FX{i}"),
                3 => (if i == 1 { "AUTOX" } else { "AUTOY" }).into(),
                4 => "SELCH".into(),
                _ => format!("MAIN{i}"),
            })
        }
        ("FXPAR", 1) => Value::Str(format!("FX{}", f.target + 1)),
        ("FXPAR", 2) => Value::Int(if f.option < 0 { 41 } else { f.option + 1 }),
        ("DAWBTN", 1) => return Some(Value::Int(f.option)).and_then(|_| daw_button_item(f.option)),
        ("CHPAGE", 1) => Value::Int(strip_number(num, f.target)),
        ("CHPAGE", 2) => return item(CH_PAGES.get(f.option as usize).copied().unwrap_or("HOME")),
        ("PAGE", 1) => return item(GLOBAL_PAGES.get(f.option as usize).copied().unwrap_or("FX")),
        ("FDRPAGE", 1) => return item(AREAS.get(f.target as usize).copied().unwrap_or("L")),
        ("VIEWPAGE", 1) => return item(AREAS.get(f.target as usize).copied().unwrap_or("L")),
        ("FDRPAGE" | "VIEWPAGE", 2) => Value::Int(f.option + 1),
        ("OTHER", 1) if f.category == CC_BANK_CATEGORY => return item("CCBANK"),
        ("OTHER", 1) => return item(OTHER_ITEMS.get(f.option as usize).copied().unwrap_or("TBA")),
        ("OTHER", 2) => Value::Str(match (f.category, f.option) {
            (CC_BANK_CATEGORY, bank) => (bank + 1).to_string(),
            (_, other::CC_BANK_DOWN) => "-1".into(),
            _ => "+1".into(),
        }),
        ("SHOWCTL", 1) => return item(SHOW_ACTIONS.get(f.option as usize).copied().unwrap_or("GO")),
        ("SCENES", 1) => Value::Int(f.target.max(1)),
        ("MIDICCT" | "MIDICCP" | "MIDINT" | "MIDINP" | "MIDIPGM", 1) => Value::Int(f.target % 16 + 1),
        ("MIDIPGM", 2) => Value::Int(f.option.clamp(0, 127) + 1),
        ("MIDICCT" | "MIDICCP" | "MIDINT" | "MIDINP", 2) => Value::Int(f.option.clamp(0, 127)),
        // Sidecar's MIDI keys send 127 (and 0).
        ("MIDICCT" | "MIDICCP" | "MIDINT" | "MIDINP", 3) => Value::Int(127),
        ("USBPR", 1) => return item(USB_KEYS.get(f.option as usize).copied().unwrap_or("PLAY")),
        _ => return None,
    })
}

/// The `btn` item of DAW BUTTONS command `option` (T1 .. SP6, in order).
fn daw_button_item(option: i32) -> Option<Value> {
    const GROUPS: [(&str, i32); 10] =
        [("T", 20), ("N", 9), ("A", 16), ("F", 8), ("V", 15), ("AU", 12), ("SY", 10), ("OT", 12), ("E", 10), ("SP", 6)];
    let mut first = 0;
    for (prefix, count) in GROUPS {
        if option < first + count {
            return (option >= first).then(|| Value::Str(format!("{prefix}{}", option - first + 1)));
        }
        first += count;
    }
    None
}

fn set_key_param(num: &Numbering, mode: &str, f: CcFunction, index: u8, v: &Value) -> Result<Option<CcFunction>, SetError> {
    let int = v.as_i32().unwrap_or(0);
    let unsupported = || SetError::Unsupported(format!("no {mode} {} here", text(v)));
    let with_target = |target| Ok(Some(CcFunction { target, ..f }));
    let with_option = |option| Ok(Some(CcFunction { option, ..f }));
    let position = |list: &[&str]| list.iter().position(|i| *i == text(v)).map(|i| i as i32);
    match (mode, index) {
        ("MUTE" | "SOLO" | "INS1" | "INS2" | "SOF" | "CHPAGE", 1) => with_target(strip_target(num, int)?),
        ("MGRP", 1) => with_target(numbered(v, "MGRP.").filter(|n| (1..=8).contains(n)).ok_or(SetError::BadValue)? - 1),
        ("DCAMUTE", 1) => with_target(int.clamp(1, 16) - 1),
        ("SPILL", 2) => {
            let t = text(v);
            let n = |p| numbered(v, p).map(|n| n - 1);
            let target = match t.as_str() {
                "AUTOX" => 300,
                "AUTOY" => 301,
                "SELCH" => 400,
                _ if t.starts_with("DCA") => n("DCA").ok_or(SetError::BadValue)?,
                _ if t.starts_with("FX") => 200 + n("FX").ok_or(SetError::BadValue)?,
                _ if t.starts_with("BUS") => 100 + n("BUS").ok_or(SetError::BadValue)?,
                _ if t.starts_with("MAIN") => 500 + n("MAIN").ok_or(SetError::BadValue)?,
                // Sidecar has no matrix spill, and no spill of the active channel.
                _ => return Err(unsupported()),
            };
            Ok(Some(CcFunction { category: FADER_SPILL_CATEGORY, target, option: 0 }))
        }
        ("FXPAR", 1) => with_target(numbered(v, "FX").filter(|n| (1..=16).contains(n)).ok_or(SetError::BadValue)? - 1),
        ("FXPAR", 2) => with_option(if int >= 41 { -1 } else { int.max(1) - 1 }),
        ("DAWBTN", 1) => {
            let option =
                (0..118).find(|&o| daw_button_item(o).as_ref() == Some(&Value::Str(text(v)))).ok_or(SetError::BadValue)?;
            Ok(Some(CcFunction { target: daw_command_group(option), option, ..f }))
        }
        ("CHPAGE", 2) => with_option(if text(v) == "SND.EQ" { 9 } else { position(&CH_PAGES).ok_or(SetError::BadValue)? }),
        ("PAGE", 1) => match text(v).as_str() {
            "SRC" | "OUTS" => with_option(3),
            "2TRK" => Err(unsupported()),
            _ => with_option(position(&GLOBAL_PAGES[1..]).ok_or(SetError::BadValue)? + 1),
        },
        ("FDRPAGE", 1) => match text(v).as_str() {
            "L" | "CMPCT" => with_target(0),
            "C" | "VRT" => with_target(1),
            "R" => with_target(2),
            _ => Err(unsupported()),
        },
        ("VIEWPAGE", 1) => with_target(position(&AREAS).ok_or(SetError::BadValue)?),
        ("FDRPAGE" | "VIEWPAGE", 2) => with_option(int.clamp(1, 21) - 1),
        ("OTHER", 1) => {
            let t = text(v);
            if t == "CCBANK" {
                return Ok(Some(match f.category {
                    CC_BANK_CATEGORY => f,
                    _ => CcFunction { category: OTHER_CATEGORY, target: 0, option: other::CC_BANK_UP },
                }));
            }
            // The first of Sidecar's functions with that item (TBA: TALKBACK A, not SOF OFF).
            let option = OTHER_ITEMS
                .iter()
                .enumerate()
                .position(|(i, o)| *o == t && i as i32 != other::SOF_OFF)
                .ok_or(SetError::BadValue)?;
            Ok(Some(CcFunction { category: OTHER_CATEGORY, target: 0, option: option as i32 }))
        }
        ("OTHER", 2) => {
            if f.category == OTHER_CATEGORY && !matches!(f.option, other::CC_BANK_UP | other::CC_BANK_DOWN) {
                return Ok(None); // CCBK counts only with CCBANK
            }
            Ok(Some(match text(v).as_str() {
                "-1" => CcFunction { category: OTHER_CATEGORY, target: 0, option: other::CC_BANK_DOWN },
                "+1" => CcFunction { category: OTHER_CATEGORY, target: 0, option: other::CC_BANK_UP },
                n => CcFunction {
                    category: CC_BANK_CATEGORY,
                    target: 0,
                    option: n.parse::<i32>().map_err(|_| SetError::BadValue)?.clamp(1, 16) - 1,
                },
            }))
        }
        ("SHOWCTL", 1) => with_option(position(&SHOW_ACTIONS).ok_or(SetError::BadValue)?),
        ("SCENES", 1) => with_target(int.clamp(1, 128)),
        ("MIDICCT" | "MIDICCP" | "MIDINT" | "MIDINP" | "MIDIPGM", 1) => {
            with_target(midi_type(mode).unwrap_or(0) * 16 + int.clamp(1, 16) - 1)
        }
        ("MIDIPGM", 2) => with_option(int.clamp(1, 128) - 1),
        ("MIDICCT" | "MIDICCP" | "MIDINT" | "MIDINP", 2) => with_option(int.clamp(0, 127)),
        ("USBPR", 1) => with_option(position(&USB_KEYS).ok_or(SetError::BadValue)?),
        _ => Ok(None),
    }
}

fn encoder_param(num: &Numbering, mode: &str, f: &CcFunction, index: u8) -> Option<Value> {
    Some(match (mode, index) {
        ("FDR" | "PAN" | "FSND", 1) => Value::Int(strip_number(num, f.target)),
        ("DCA", 1) => Value::Int(f.target + 1),
        ("SSND", 1) => Value::Str(format!("BUS{}", f.target + 1)),
        ("FSND", 2) => Value::Str(format!("BUS{}", f.option + 1)),
        ("FX", 1) => Value::Str(format!("FX{}", f.target + 1)),
        ("FX", 2) => Value::Int(if f.option < 0 { 41 } else { f.option + 1 }),
        ("DAWMCU", 1) => Value::Str(match f.target {
            t @ 0..=7 => format!("M{}", t + 1),
            t @ 8..=23 => format!("E{}", t - 7),
            _ => "JOG".into(),
        }),
        ("MON", 1) => Value::str(MONITOR_ITEMS.get(f.option as usize).copied().unwrap_or("B")),
        ("MIDICC", 1) => Value::Int(f.target + 1),
        ("MIDICC", 2) => Value::Int(f.option),
        _ => return None,
    })
}

fn set_encoder_param(num: &Numbering, mode: &str, f: CcFunction, index: u8, v: &Value) -> Result<Option<CcFunction>, SetError> {
    let int = v.as_i32().unwrap_or(0);
    let with_target = |target| Ok(Some(CcFunction { target, ..f }));
    let with_option = |option| Ok(Some(CcFunction { option, ..f }));
    let bus = || match numbered(v, "BUS") {
        Some(n) if (1..=16).contains(&n) => Ok(n - 1),
        _ => Err(SetError::Unsupported(format!("no send to {} here (buses only)", text(v)))),
    };
    match (mode, index) {
        ("FDR" | "PAN" | "FSND", 1) => with_target(strip_target(num, int)?),
        ("DCA", 1) => with_target(int.clamp(1, 16) - 1),
        ("SSND", 1) => with_target(bus()?),
        ("FSND", 2) => with_option(bus()?),
        ("FX", 1) => with_target(numbered(v, "FX").filter(|n| (1..=16).contains(n)).ok_or(SetError::BadValue)? - 1),
        ("FX", 2) => with_option(if int >= 41 { -1 } else { int.max(1) - 1 }),
        ("DAWMCU", 1) => {
            let t = text(v);
            with_target(match (numbered(v, "M"), numbered(v, "E")) {
                _ if t == "JOG" => 24,
                (Some(m), _) => m.clamp(1, 8) - 1,
                (_, Some(e)) => e.clamp(1, 16) + 7,
                _ => return Err(SetError::BadValue),
            })
        }
        ("MON", 1) => with_option(if text(v) == "A" { 2 } else { 0 }),
        ("MIDICC", 1) => with_target(int.clamp(1, 16) - 1),
        ("MIDICC", 2) => with_option(int.clamp(0, 127)),
        _ => Ok(None),
    }
}

// ---- the show in the WING's terms ----------------------------------------------------

/// The WING's 76 strips numbered as in its strip order: channels 1-40, aux
/// 41-48, buses 49-64, mains 65-68, matrices 69-76.
#[derive(Clone, Debug, Default, PartialEq)]
struct Numbering {
    strips: Vec<(StripRef, i32)>,
}

impl Numbering {
    fn of(engine: &Engine) -> Self {
        let mut strips = Vec::with_capacity(SPILL_POSITIONS);
        for (kind, base, max) in
            [(StripKind::Channel, 0, 40), (StripKind::Aux, 40, 8), (StripKind::Main, 64, 4), (StripKind::Matrix, 68, 8)]
        {
            for (i, s) in engine.strips(kind).iter().take(max).enumerate() {
                strips.push((StripRef::of(s), base + i as i32 + 1));
            }
        }
        // Buses by slot.
        for b in engine.strips(StripKind::Bus) {
            if (0..16).contains(&b.id()) {
                strips.push((StripRef::of(&b), 49 + b.id()));
            }
        }
        Self { strips }
    }

    fn number(&self, r: &StripRef) -> Option<i32> {
        self.strips.iter().find(|(s, _)| s == r).map(|(_, n)| *n)
    }

    fn strip(&self, n: i32) -> Option<StripRef> {
        self.strips.iter().find(|(_, k)| *k == n).map(|(s, _)| *s)
    }

    fn encode(&self, f: &FaderRef) -> WingFader {
        let w = |ty, i, dst| WingFader { ty, i, dst };
        match *f {
            FaderRef::Empty => WingFader::OFF,
            FaderRef::Strip(r) => match self.number(&r) {
                Some(n) if n <= 48 => w(T_CH, n, 1),
                Some(n) => w(T_BUS, n - 48, 1),
                None => WingFader::OFF,
            },
            FaderRef::Dca(i) => w(T_DCA, i + 1, 1),
            FaderRef::Send { from, to } | FaderRef::BusSend { from, to } => match (self.number(&from), self.number(&to)) {
                (Some(i), Some(d)) if d > 48 => w(T_SEND, i, d - 48),
                _ => WingFader::OFF,
            },
            FaderRef::SelSend { .. } => WingFader::OFF,
            FaderRef::FxParam { slot, param } => w(T_FX, slot + 1, param + 1),
            FaderRef::MidiCc { channel, cc } => w(T_MIDI, cc, channel + 1),
        }
    }

    /// Ok(None) for OFF, Err for what Sidecar has no fader for.
    fn decode(&self, w: WingFader) -> Result<Option<FaderRef>, ()> {
        let strip = |n: i32| self.strip(n).ok_or(());
        Ok(Some(match w.ty {
            T_OFF => return Ok(None),
            T_CH => FaderRef::Strip(strip(w.i)?),
            T_BUS if (1..=28).contains(&w.i) => FaderRef::Strip(strip(48 + w.i)?),
            T_DCA if (1..=NUM_DCAS as i32).contains(&w.i) => FaderRef::Dca(w.i - 1),
            T_MIDI => FaderRef::midi_cc(w.dst - 1, w.i),
            T_SEND if (1..=28).contains(&w.dst) => FaderRef::Send { from: strip(w.i)?, to: strip(48 + w.dst)? },
            T_FX if (1..=NUM_FX as i32).contains(&w.i) && w.dst >= 1 => FaderRef::FxParam { slot: w.i - 1, param: w.dst - 1 },
            _ => return Err(()),
        }))
    }

    fn layer(&self, l: &Layer) -> SurfaceLayer {
        SurfaceLayer { name: l.name.clone(), faders: l.faders.iter().map(|f| self.encode(f)).collect() }
    }
}

/// `spidx` as a spill (None: not one Sidecar can start; Some(None): no spill).
fn spill_kind(spidx: i32, sends: Option<StripRef>) -> Option<Option<SpillKind>> {
    Some(Some(match spidx {
        0 => return Some(None),
        1..=16 => SpillKind::Dca(spidx - 1),
        17..=32 => SpillKind::Fx(spidx - 17),
        33..=48 => SpillKind::Bus(spidx - 33),
        57..=60 => SpillKind::Main(spidx - 57),
        61 | 62 => SpillKind::Automix(spidx - 61),
        SPIDX_SENDS => SpillKind::Sends(sends?),
        _ => return None,
    }))
}

fn spill_index(kind: &SpillKind) -> i32 {
    match *kind {
        SpillKind::Dca(i) => 1 + i,
        SpillKind::Fx(i) => 17 + i,
        SpillKind::Bus(i) => 33 + i,
        SpillKind::Main(i) => 57 + i,
        SpillKind::Automix(g) => 61 + g,
        SpillKind::Sends(_) => SPIDX_SENDS,
    }
}

fn enc_mode(f: &CcFunction) -> &'static str {
    ENC_MODES.iter().find(|m| m.2 == Some(f.category)).map_or("OFF", |m| m.0)
}

fn btn_mode(f: &CcFunction) -> &'static str {
    if f.category == CC_BANK_CATEGORY {
        return BTN_MODES[OTHER_MODE].0;
    }
    BTN_MODES
        .iter()
        .find(|m| m.2.is_some_and(|(cat, sub)| cat == f.category && (sub < 0 || sub == f.target / 16)))
        .map_or("OFF", |m| m.0)
}

/// The show's layers and custom controls as clients see them.
pub fn publish(engine: &Engine, layers: &LayerSet, custom: &CustomControls, cc_bank: i32) -> Surface {
    let num = Numbering::of(engine);
    let console = ConsoleRefs::of(engine);
    let sections = std::array::from_fn(|s| {
        if s == VRT {
            return publish_virtual(layers, &num);
        }
        let defaults = layers.default_layers_for(s, &console);
        let keys = KEYS[s]
            .iter()
            .zip(KEY_NAMES[s])
            .map(|(id, wing_name)| {
                let mut key = defaults.iter().find(|l| l.id == *id).map(|l| num.layer(l)).unwrap_or_default();
                if user_key(id).is_none() {
                    // The default layers' names are generated: answer the console's.
                    (*wing_name).clone_into(&mut key.name);
                }
                key
            })
            .collect();
        let spill = layers.spill(s);
        let selected = layers.selected(s);
        let sel = if spill.is_some() {
            SEL_SPILL
        } else {
            // A user layer selected by its id shows on its USER key.
            let as_key = layers.user_layers().iter().take(2).position(|l| l.id == selected).map(|u| USER_KEY_IDS[u]);
            let id = as_key.unwrap_or(selected);
            KEYS[s].iter().position(|k| *k == id).map_or(SEL_NO_KEY, |k| k as i32 + 1)
        };
        SurfaceSection {
            sel,
            spill: spill.as_ref().map_or(0, spill_index),
            offset: layers.offset(s),
            keys,
            spill_layer: layers
                .spill_layer(engine, s)
                .map(|l| num.layer(&l))
                .unwrap_or_else(|| SurfaceLayer { name: SPILL_NAME.into(), ..SurfaceLayer::default() }),
        }
    });
    let controls = (0..NUM_LAYERS)
        .map(|l| {
            std::array::from_fn(|i| {
                let c = custom.control(l, i).cloned().unwrap_or_default();
                SurfaceControl {
                    modes: [enc_mode(&c.encoder), btn_mode(&c.upper), btn_mode(&c.lower)],
                    functions: [c.encoder, c.upper, c.lower],
                    name: c.name,
                    colour: c.colour,
                }
            })
        })
        .collect();
    Surface { attached: true, sections, controls, cc_bank: cc_bank.clamp(0, NUM_BANKS - 1), num }
}

/// `/$ctl/layer/VRT`: the virtual layers, and on a COMPACT its VIRTUAL
/// section's selection and scroll position.
fn publish_virtual(layers: &LayerSet, num: &Numbering) -> SurfaceSection {
    let compact = layers.model().is_compact();
    let selected = layers.selected(VIRTUAL_SECTION);
    SurfaceSection {
        sel: if compact { VIRTUAL_KEYS.iter().position(|k| *k == selected).map_or(1, |k| k as i32 + 1) } else { 1 },
        spill: 0,
        offset: if compact { layers.offset(VIRTUAL_SECTION) } else { 0 },
        keys: (0..NUM_VIRTUAL_LAYERS).map(|i| layers.virtual_layer(i).map(|l| num.layer(&l)).unwrap_or_default()).collect(),
        spill_layer: SurfaceLayer { name: SPILL_NAME.into(), ..SurfaceLayer::default() },
    }
}

const USER_KEY_IDS: [&str; 2] = livedaw_session::layers::USER_KEYS;

/// What [`sync`] changed in the show.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SyncResult {
    pub layers: bool,
    pub controls: bool,
    /// The upper custom control layer a client chose.
    pub cc_bank: Option<i32>,
}

/// The id of user layer `user` (0, 1: USER 1 / USER 2), made if missing.
fn ensure_user_layer(layers: &mut LayerSet, user: usize) -> String {
    while layers.user_layers().len() <= user {
        let n = layers.user_layers().len() + 1;
        layers.add_user_layer(&format!("USER {n}"), Vec::new());
    }
    layers.user_layers()[user].id.clone()
}

/// A function chosen by its WING `mode`: the current one if it is that
/// function, else the function at its first target (as CC EDIT starts it).
fn new_function(num: &Numbering, slot: CcSlot, mode: &str, old: CcFunction) -> Option<CcFunction> {
    // CH 1 and BUS 1 by the WING's numbers.
    let first_strip =
        |kind: StripKind| num.strip(if kind == StripKind::Bus { 49 } else { 1 }).map_or(0, |r| r.kind as i32 * 100_000 + r.id);
    if slot == CcSlot::Enc {
        let cat = ENC_MODES.iter().find(|m| m.0 == mode)?.2?;
        if cat == old.category {
            return Some(old);
        }
        let (target, option) = match cat {
            // FADER, PAN, FIXED BUS SEND: CH 1; FX PARAMETER: no parameter yet.
            1 | 2 | 5 => (first_strip(StripKind::Channel), 0),
            6 => (0, -1),
            _ => (0, 0),
        };
        return Some(CcFunction { category: cat, target, option });
    }
    let (cat, sub) = BTN_MODES.iter().find(|m| m.0 == mode)?.2?;
    if cat == old.category && (sub < 0 || sub == old.target / 16) || cat == 11 && old.category == CC_BANK_CATEGORY {
        return Some(old);
    }
    let (target, option) = match cat {
        // MUTE, SOLO, CH PAGE, INSERT PRE / POST: CH 1; SENDS ON FDR: the
        // first bus; SCENE RECALL: #1; FX PAR: FX 1's MIX.
        1 | 2 | 6 | INSERT_PRE_CATEGORY | INSERT_POST_CATEGORY => (first_strip(StripKind::Channel), 0),
        FX_PARAMETER_CATEGORY => (0, -1),
        5 => (first_strip(StripKind::Bus), 0),
        10 => (1, 0),
        // A MIDI key keeps its channel and number.
        MIDI_CATEGORY if old.category == MIDI_CATEGORY => (sub * 16 + old.target % 16, old.option),
        MIDI_CATEGORY => (sub * 16, 0),
        _ => (0, 0),
    };
    Some(CcFunction { category: cat, target, option })
}

/// Applies a client's edit to the show.
fn apply(engine: &Engine, layers: &mut LayerSet, custom: &mut CustomControls, edit: SurfaceEdit, r: &mut SyncResult) {
    match edit {
        // The virtual section's selection and scroll position are the
        // COMPACT's VIRTUAL section's (the full-size console has none).
        SurfaceEdit::Select { section: VRT, .. } | SurfaceEdit::Offset { section: VRT, .. } if !layers.model().is_compact() => {}
        SurfaceEdit::Select { section, key } => {
            let (section, id) = if section == VRT { (VIRTUAL_SECTION, VIRTUAL_KEYS[key]) } else { (section, KEYS[section][key]) };
            if layers.shown(section) != id {
                layers.select(section, id);
                r.layers = true;
            }
        }
        SurfaceEdit::Spill { section, spill } => match spill_kind(spill, None) {
            Some(None) => r.layers |= layers.end_spill(section),
            Some(Some(kind)) if layers.spill(section) != Some(kind) => {
                layers.start_spill(section, kind);
                r.layers = true;
            }
            _ => {}
        },
        SurfaceEdit::Offset { section, offset } => {
            let section = if section == VRT { VIRTUAL_SECTION } else { section };
            if layers.offset(section) != offset {
                layers.set_offset(section, offset);
                r.layers = true;
            }
        }
        SurfaceEdit::RenameVirtual { index, name } => {
            if layers.virtual_layer(index).is_some_and(|l| l.name != name) {
                layers.rename_user_layer(VIRTUAL_KEYS[index], &name);
                r.layers = true;
            }
        }
        SurfaceEdit::RenameUser { user, name } => {
            let exists = layers.user_layers().get(user).is_some_and(|l| l.name == name);
            if !exists {
                let id = ensure_user_layer(layers, user);
                layers.rename_user_layer(&id, &name);
                r.layers = true;
            }
        }
        SurfaceEdit::Fader { section, key, pos, fader } => {
            let num = Numbering::of(engine);
            let Ok(new) = num.decode(fader) else { return };
            let id = keys(section)[key];
            let user = user_key(id);
            let current = match user {
                Some(u) => layers.user_layers().get(u).map(|l| l.faders.clone()).unwrap_or_default(),
                None if section == VRT => layers.virtual_layer(key).map(|l| l.faders).unwrap_or_default(),
                None => {
                    let defaults = layers.default_layers_for(section, &ConsoleRefs::of(engine));
                    let Some(l) = defaults.into_iter().find(|l| l.id == id) else { return };
                    l.faders
                }
            };
            // What the slot already reads changes nothing: a selected
            // channel's send (read as OFF) or a send with BUS CUSTOMIZATION
            // (read as its SEND) written back as read stays what it is.
            if num.encode(&slots::get(&current, pos)) == fader {
                return;
            }
            // Fixed slots: OFF blanks the slot, a fader past the end leaves
            // blanks before it; the other slots stay where they are.
            let mut faders = current.clone();
            slots::set(&mut faders, pos, new.unwrap_or(FaderRef::Empty));
            if faders == current {
                return;
            }
            match user {
                Some(u) => {
                    let id = ensure_user_layer(layers, u);
                    layers.set_user_layer_faders(&id, faders);
                }
                None => layers.set_default_override(if section == VRT { VIRTUAL_SECTION } else { section }, id, faders),
            }
            r.layers = true;
        }
        SurfaceEdit::Function { layer, index, slot, function } => {
            let Some(c) = custom.control_mut(layer, index) else { return };
            let f = match slot {
                CcSlot::Enc => &mut c.encoder,
                CcSlot::Up => &mut c.upper,
                CcSlot::Down => &mut c.lower,
            };
            if function != *f {
                *f = function;
                r.controls = true;
            }
        }
        SurfaceEdit::Name { layer, index, name } => {
            if let Some(c) = custom.control_mut(layer, index).filter(|c| c.name != name) {
                c.name = name;
                r.controls = true;
            }
        }
        SurfaceEdit::Colour { layer, index, colour } => {
            if let Some(c) = custom.control_mut(layer, index).filter(|c| c.colour != colour) {
                c.colour = colour;
                r.controls = true;
            }
        }
        SurfaceEdit::Bank(b) => r.cc_bank = Some(b),
    }
}

/// Host (message thread): applies the edits clients made to the show's
/// layers and custom controls, then publishes them with the upper custom
/// control layer `cc_bank` (0-15, or the one a client chose).
pub fn sync(state: &WingState, engine: &Engine, layers: &mut LayerSet, custom: &mut CustomControls, cc_bank: i32) -> SyncResult {
    // Held throughout, so that no client edit lands between the two steps.
    let mut surface = state.surface();
    let mut r = SyncResult::default();
    for edit in state.take_surface_edits() {
        apply(engine, layers, custom, edit, &mut r);
    }
    *surface = publish(engine, layers, custom, r.cc_bank.unwrap_or(cc_bank));
    r
}

/// No host any more: the nodes go back to the shadow store.
pub fn detach(state: &WingState) {
    let mut surface = state.surface();
    *surface = Surface::default();
    state.take_surface_edits();
}
