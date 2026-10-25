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
//! - `L`, `C`, `R` are the three fader sections (`CMPCT`, `RCK`, `EXT` and
//!   `VRT`, the other models' surfaces, answer the WING's factory values and
//!   keep what a client sets). `sel` 1-7
//!   picks a layer key in the console's order (left: CH 1-12, 13-24, 25-36,
//!   37-40 / AUX, BUSES, USER 1, USER 2; center: DCA, MAIN, AUX, BUSES, USER 1,
//!   USER 2; right: MAIN, DCA, CH 1-40, AUX, BUSES, USER 1, USER 2) and reads
//!   22 while the section spills. 8-21 (no-ops and the console's fixed banks)
//!   are ignored; a layer without a WING key (the center's INPUTS, a user
//!   layer selected by itself) reads as 8.
//! - `spidx`: 0 no spill, 1-16 DCA, 17-32 FX slot, 33-48 a bus's sources,
//!   61 / 62 automix X / Y, 63 a strip's sends (read only: which strip is the
//!   console's selection). Sidecar has no main or matrix spill (49-60).
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
//!   Sidecar's layers have no gaps: a fader set past the end is appended and
//!   OFF removes one. Changing a default key edits it (as VIEW > CONFIG does);
//!   the USER keys change the user layers.
//! - `$spill`: the spill a section shows, read only (the console too
//!   replaces what is set there).
//! - `/$ctl/user/1..16/1..4`: the upper part's custom control layers;
//!   `user/1..4` the lower part's USER layer, `daw1..4` its DAW layers and,
//!   a guess (the document lists them for the Compact), `U1..U3` the
//!   4-Channel section's F1-F3 layers. `mode` of `enc`, `bu` and `bd` is the
//!   function; a new function starts at its first target, as on CC EDIT. A
//!   control has one name (`enc/name`, `bu/name` where there is no encoder)
//!   and a colour; `$fname` reads the function's name. `sel`: the upper
//!   part's layer (1-16; the document says the console answers 0-15).

use crate::map::{SetError, WingState};
use crate::value::Value;
use livedaw_engine::params::{NUM_DCAS, NUM_FX};
use livedaw_engine::{Engine, StripKind};
use livedaw_session::custom::{CcFunction, CustomControls, FIRST_DAW_LAYER, NUM_CONTROLS, NUM_LAYERS};
use livedaw_session::layers::{slots, ConsoleRefs, FaderRef, Layer, LayerSet, SpillKind, NUM_SECTIONS};
use livedaw_session::refs::StripRef;
use std::collections::VecDeque;

/// The layer keys of each section in the WING's order, as Sidecar's layer ids.
pub const KEYS: [&[&str]; NUM_SECTIONS] = [
    &["in-1", "in-2", "in-3", "in-4", "bus-1", "user-1", "user-2"],
    &["dca-1", "out-1", "aux", "bus-1", "user-1", "user-2"],
    &["out-1", "dca-1", "inputs", "aux", "bus-1", "user-1", "user-2"],
];
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
    ("INS1", "CH INSERT PRE ON/OFF", None),
    ("INS2", "CH INSERT POST ON/OFF", None),
    ("MGRP", "MUTE GROUP", Some((4, -1))),
    ("DCAMUTE", "DCA MUTE", Some((3, -1))),
    ("SOF", "SENDS ON FDR", Some((5, -1))),
    ("SPILL", "FADER SPILL", Some((12, -1))),
    ("FXPAR", "FX PAR", None),
    ("DAWBTN", "DAW BUTTONS", Some((13, -1))),
    ("DAWENC", "DAW V-POTS", None),
    ("CHPAGE", "CH PAGE", Some((6, -1))),
    ("PAGE", "GLOBAL PAGE", Some((7, -1))),
    ("FDRPAGE", "FADER PAGE", Some((8, -1))),
    ("VIEWPAGE", "OVERVIEW PAGE", None),
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
/// CC BANK (3.0.6) is the WING's OTHER > CCBANK.
const CC_BANK_CATEGORY: i32 = 18;
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
}

/// Binds `/$ctl/layer/<rest>`.
pub fn bind_layer(rest: &[&str]) -> Option<(u8, LayerF)> {
    let (section, rest) = rest.split_first()?;
    let s = ["L", "C", "R"].iter().position(|n| n == section)?;
    let number = |p: &str, max: usize| p.parse::<usize>().ok().filter(|n| (1..=max).contains(n)).map(|n| (n - 1) as u8);
    let field = |f: &str| match f {
        "type" => Some(FaderField::Type),
        "i" => Some(FaderField::Index),
        "dst" => Some(FaderField::Dest),
        _ => None,
    };
    let keys = KEYS[s].len();
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

/// Binds `/$ctl/user/<rest>`.
pub fn bind_user(rest: &[&str]) -> Option<UserF> {
    if rest == ["sel"] {
        return Some(UserF::Bank);
    }
    let [layer, control, tail @ ..] = rest else { return None };
    let layer = match *layer {
        "user" => 16,
        "U1" | "U2" | "U3" => 16 + layer[1..].parse::<usize>().ok()?,
        l if l.starts_with("daw") => FIRST_DAW_LAYER + l[3..].parse::<usize>().ok().filter(|n| (1..=4).contains(n))? - 1,
        l => l.parse::<usize>().ok().filter(|n| (1..=NUM_BANKS as usize).contains(n))? - 1,
    } as u8;
    let control = control.parse::<usize>().ok().filter(|n| (1..=NUM_CONTROLS).contains(n))? as u8 - 1;
    // The name lives on the encoder where there is one (the upper part's layers).
    let name_slot = if (layer as i32) < NUM_BANKS { "enc" } else { "bu" };
    Some(match tail {
        ["col"] if layer != 16 && (layer as usize) < FIRST_DAW_LAYER => UserF::Colour(layer, control),
        [slot, f] => {
            let cs = match *slot {
                "enc" if (layer as i32) < NUM_BANKS => CcSlot::Enc,
                "bu" => CcSlot::Up,
                "bd" if layer < 17 || layer as usize >= FIRST_DAW_LAYER => CcSlot::Down,
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
    pub name: String,
    pub colour: i32,
}

impl Default for SurfaceControl {
    fn default() -> Self {
        Self { modes: ["OFF"; 3], name: String::new(), colour: 0 }
    }
}

/// The show's layers and custom controls in the WING's terms.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Surface {
    /// A host applies the edits (else the nodes are shadow).
    pub attached: bool,
    pub sections: [SurfaceSection; NUM_SECTIONS],
    /// `CustomControls`' layers.
    pub controls: Vec<[SurfaceControl; NUM_CONTROLS]>,
    /// The upper part's layer, 0-15.
    pub cc_bank: i32,
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
    Fader {
        section: usize,
        key: usize,
        pos: usize,
        fader: WingFader,
    },
    Mode {
        layer: usize,
        index: usize,
        slot: CcSlot,
        mode: &'static str,
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
                let key = usize::try_from(int - 1).ok().filter(|&k| k < KEYS[section].len());
                key.map(|key| {
                    sec.sel = int;
                    sec.spill = 0;
                    sec.offset = 0;
                    SurfaceEdit::Select { section, key }
                })
            }
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
            LayerF::Name(k) => user_key(KEYS[section][k as usize]).map(|user| {
                let name = match v {
                    Value::Str(s) => s.clone(),
                    other => other.as_i32().unwrap_or(0).to_string(),
                };
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
            UserF::FName(l, c, slot) => Value::str(mode_long_name(slot, control(l, c)?.modes[cc_slot_index(slot)])),
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
                control.modes[cc_slot_index(slot)] = mode;
                SurfaceEdit::Mode { layer, index, slot, mode }
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

// ---- the show in the WING's terms ----------------------------------------------------

/// The WING's 76 strips numbered as in its strip order: channels 1-40, aux
/// 41-48, buses 49-64, mains 65-68, matrices 69-76.
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
            FaderRef::Send { from, to } => match (self.number(&from), self.number(&to)) {
                (Some(i), Some(d)) if d > 48 => w(T_SEND, i, d - 48),
                _ => WingFader::OFF,
            },
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
        let defaults = layers.default_layers_for(s, &console);
        let keys =
            KEYS[s].iter().map(|id| defaults.iter().find(|l| l.id == *id).map(|l| num.layer(l)).unwrap_or_default()).collect();
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
            spill_layer: layers.spill_layer(engine, s).map(|l| num.layer(&l)).unwrap_or_default(),
        }
    });
    let controls = (0..NUM_LAYERS)
        .map(|l| {
            std::array::from_fn(|i| {
                let c = custom.control(l, i).cloned().unwrap_or_default();
                SurfaceControl {
                    modes: [enc_mode(&c.encoder), btn_mode(&c.upper), btn_mode(&c.lower)],
                    name: c.name,
                    colour: c.colour,
                }
            })
        })
        .collect();
    Surface { attached: true, sections, controls, cc_bank: cc_bank.clamp(0, NUM_BANKS - 1) }
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
fn new_function(engine: &Engine, slot: CcSlot, mode: &str, old: CcFunction) -> Option<CcFunction> {
    let first_strip = |kind: StripKind| engine.strips(kind).first().map_or(0, |s| kind as i32 * 100_000 + s.id());
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
        // MUTE, SOLO, CH PAGE: CH 1; SENDS ON FDR: the first bus; SCENE RECALL: #1.
        1 | 2 | 6 => (first_strip(StripKind::Channel), 0),
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
        SurfaceEdit::Select { section, key } => {
            let id = KEYS[section][key];
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
            if layers.offset(section) != offset {
                layers.set_offset(section, offset);
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
            let id = KEYS[section][key];
            let user = user_key(id);
            let current = match user {
                Some(u) => layers.user_layers().get(u).map(|l| l.faders.clone()).unwrap_or_default(),
                None => {
                    let defaults = layers.default_layers_for(section, &ConsoleRefs::of(engine));
                    let Some(l) = defaults.into_iter().find(|l| l.id == id) else { return };
                    l.faders
                }
            };
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
                None => layers.set_default_override(section, id, faders),
            }
            r.layers = true;
        }
        SurfaceEdit::Mode { layer, index, slot, mode } => {
            let Some(c) = custom.control_mut(layer, index) else { return };
            let f = match slot {
                CcSlot::Enc => &mut c.encoder,
                CcSlot::Up => &mut c.upper,
                CcSlot::Down => &mut c.lower,
            };
            if let Some(new) = new_function(engine, slot, mode, *f) {
                if new != *f {
                    *f = new;
                    r.controls = true;
                }
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
