//! The mapping layer: WING parameter paths and ids resolved to Live DAW's
//! engine state, in both directions, with the WING's units and ranges.
//!
//! Every static node of the tree gets a [`Binding`] computed once from its
//! path; model parameters bind through their slot (a strip's GATE, EQ,
//! DYNAMICS or filter tool, or an FX slot) and the active model's parameter
//! of the same name (our model tables come from the same sweep, so names and
//! ranges line up). Nodes without a Live DAW equivalent are kept in a shadow
//! store ([`WingState`]), so a client reads back what it wrote and snapshot
//! files keep them, but they don't affect the audio.

use crate::schema::{schema, Def, ModelSource, NoModels, Node, NodeType};
use crate::value::{self, Value, FADER_MAX, FADER_MIN};
use livedaw_dsp::util::MINUS_INF_DB;
use livedaw_dsp::wing::fx::allowed_in_slot;
use livedaw_dsp::wing::{model_index, ModelSpec, ParamKind, DYN_MODELS, EQ_MODELS_BUS, EQ_MODELS_CH, FILTER_TOOLS, FX_MODELS};
use livedaw_engine::atomic::AtomicF32;
use livedaw_engine::monitor::{MonitorDspParams, MONITOR_EQ_BANDS};
use livedaw_engine::params::{
    DynSlotParams, EngineControls, InsertParams, MonitorSourceParams, StripParams, FX_EXT, NUM_DCAS, NUM_FX, NUM_MUTE_GROUPS,
};
use livedaw_engine::sends::send_mode;
use livedaw_engine::solo::solo_mode;
use livedaw_engine::sources::{self, source_mode, SourceParams};
use livedaw_engine::talk::TALK_OFF;
use livedaw_engine::{Engine, Strip, StripKind, PLAYBACK_SOURCE_BASE};
use std::collections::{BTreeMap, HashMap};
use std::sync::atomic::{AtomicBool, AtomicI32, Ordering::Relaxed};
use std::sync::{Arc, Mutex, OnceLock};

// ---- identity and shadow store ------------------------------------------------------

/// What the console says about itself (discovery, `/?`, `$syscfg`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Identity {
    pub name: String,
    pub model: String,
    pub serial: String,
    pub firmware: String,
}

impl Default for Identity {
    fn default() -> Self {
        Self {
            name: "LIVE-DAW".into(),
            model: "live-daw".into(),
            serial: "LIVEDAW0001".into(),
            // Our tree is the WING 3.1 tree (libwing compares the leading numbers).
            firmware: "3.1-0-livedaw:release".into(),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum ShadowKey {
    Row(u16),
    /// slot, model (index in the family), position.
    Param(u16, u16, u8),
}

/// State of the virtual console beyond the engine: values of nodes Live DAW
/// has no equivalent for, and the console identity.
#[derive(Default)]
pub struct WingState {
    pub shadow: Mutex<HashMap<ShadowKey, Value>>,
    pub identity: Mutex<Identity>,
}

impl WingState {
    pub fn new(identity: Identity) -> Self {
        Self { shadow: Mutex::default(), identity: Mutex::new(identity) }
    }

    fn shadow_get(&self, key: ShadowKey) -> Option<Value> {
        self.shadow.lock().unwrap_or_else(|e| e.into_inner()).get(&key).cloned()
    }

    fn shadow_set(&self, key: ShadowKey, v: Value) {
        self.shadow.lock().unwrap_or_else(|e| e.into_inner()).insert(key, v);
    }

    pub fn identity(&self) -> Identity {
        self.identity.lock().unwrap_or_else(|e| e.into_inner()).clone()
    }

    pub fn clear_shadow(&self) {
        self.shadow.lock().unwrap_or_else(|e| e.into_inner()).clear();
    }
}

// ---- ports -------------------------------------------------------------------------

/// How Live DAW's inputs and outputs appear on the WING's patch.
///
/// Hardware ports are laid out as the AES50 groups: A 1-48 = ports 1-48, B
/// 1-48 = ports 49-96, C = 97-144. The recorder's 64 playback channels are the
/// WING-LIVE card (CRD). On import, other input groups (LCL, AUX, SC, USB,
/// MOD, AES, USR) land on the port of the same number; PLAY, OSC and the
/// internal BUS/MAIN/MTX sources have no equivalent.
pub mod port {
    use super::PLAYBACK_SOURCE_BASE;

    pub const BLOCK: i32 = 48;
    pub const BLOCKS: [&str; 3] = ["A", "B", "C"];

    /// A strip input (-1 none, a hardware port, or a playback channel) as (group, number).
    pub fn source_to_wing(src: i32) -> Option<(&'static str, i32)> {
        if src < 0 {
            return Some(("OFF", 1));
        }
        if src >= PLAYBACK_SOURCE_BASE {
            let n = src - PLAYBACK_SOURCE_BASE;
            return (n < 64).then_some(("CRD", n + 1));
        }
        let block = (src / BLOCK) as usize;
        BLOCKS.get(block).map(|g| (*g, src % BLOCK + 1))
    }

    /// (group, number) to a strip input. None: no Live DAW equivalent.
    pub fn wing_to_source(group: &str, n: i32) -> Option<i32> {
        let n = n.max(1);
        match group {
            "OFF" => Some(-1),
            "A" => Some(n - 1),
            "B" => Some(BLOCK + n - 1),
            "C" => Some(2 * BLOCK + n - 1),
            "CRD" => (n <= 64).then_some(PLAYBACK_SOURCE_BASE + n - 1),
            "LCL" | "AUX" | "SC" | "USB" | "MOD" | "AES" | "USR" => Some(n - 1),
            _ => None,
        }
    }

    /// A WING output (group, number) as a hardware output port.
    pub fn output_port(block: usize, n: i32) -> i32 {
        block as i32 * BLOCK + n - 1
    }

    /// The input groups in the WING's order (`/cfg/solo/$srcsgrp` 1-13).
    pub const IN_GROUPS: [&str; 13] = ["LCL", "AUX", "A", "B", "C", "SC", "USB", "CRD", "MOD", "PLAY", "AES", "USR", "OSC"];

    /// Input groups whose sources are Live DAW's sources (ROUTING > SOURCES):
    /// the AES50 ports and the WING-LIVE card. The other groups share port
    /// numbers with them here, so their source settings stay in the shadow store.
    pub const SOURCE_GROUPS: [&str; 4] = ["A", "B", "C", "CRD"];
}

// ---- bindings ----------------------------------------------------------------------

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Blk {
    Gate,
    Eq,
    Dyn,
    Tool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ScF {
    Type,
    F,
    Q,
    Src,
    Tap,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum XoF {
    Depth,
    Type,
    F,
    Q,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum InsF {
    On,
    Fx,
    Mode,
    W,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SendF {
    On,
    Lvl,
    Pon,
    Mode,
    Plink,
    Pan,
    /// Bus, main and matrix-side sends: PRE / POST switch.
    Pre,
}

/// A strip parameter.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SF {
    Name,
    ActName,
    Colour,
    ActColour,
    Icon,
    ActIcon,
    Led,
    Mute,
    Fader,
    Pan,
    Width,
    Solo,
    SoloLed,
    SoloSafe,
    Mon,
    EffFader,
    EffMute,
    Proc,
    Tap,
    Trim,
    Bal,
    Inv,
    DlyOn,
    DlyMode,
    Dly,
    AltSel,
    Mode,
    /// Input patch: ALT input, and number (else group).
    Conn {
        alt: bool,
        number: bool,
    },
    LowCut,
    LowCutF,
    LowCutS,
    HighCut,
    HighCutF,
    HighCutS,
    Tool,
    /// A model slot node (its parameters bind through it).
    Slot(Blk),
    BlkOn(Blk),
    BlkMdl(Blk),
    BlkMix(Blk),
    BlkGain(Blk),
    Sc(Blk, ScF),
    Xo(XoF),
    Ins(bool, InsF),
    Main(u8, MainF),
    Send(u16, SendF),
    Mtx(u8, SendF),
    Tags,
    BusMono,
    DirOn,
    DirLvl,
    DirInv,
    /// TAP WIDTH and TAP EQ (the WING's PRE EQ, `/ch/N/peq`): band 0-2.
    TapWidth,
    TapEqOn,
    TapEq(u8, TapF),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TapF {
    Gain,
    Freq,
    Q,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MainF {
    On,
    Lvl,
    Pre,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DcaF {
    Name,
    Colour,
    Mute,
    Fader,
    Solo,
    SoloLed,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CtlF {
    MainLink,
    DcaMuteGroups,
    SoloAfl(StripKind),
    MonitorLevel,
    MonitorDimDb,
    MonitorDim,
    MonitorMono,
    MonitorSwap,
    AutomixX,
    AutomixY,
    GlobalAlt,
    InputSelectGlobal,
    StartupMainMute,
    AnySolo,
    /// `/cfg/solo/mode`, `mon` (the solo bus), `mute`; `/$ctl/cfg/muteovr`; `/$stat/sip`.
    SoloMode,
    SoloBus,
    MonitorMute,
    MuteOverride,
    SipActive,
    /// SOURCE SOLO: `/cfg/solo/srcsolo`, `$srcsolo`, `$srcsgrp`, `$srcsin`.
    SourceSolo,
    SourceSoloOn,
    SourceSoloGroup,
    SourceSoloIn,
}

/// `/cfg/talk/assign`, `lvl`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TalkF {
    Assign,
    Level,
}

/// `/cfg/talk/A|B/...`: the key, SWITCH MODE, AUTO DIM, USE SEND LEVELS and
/// the TALK TO grids (0-based bus, matrix, main).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TalkPathF {
    On,
    Mode,
    MonDim,
    BusDim,
    Individual,
    Bus(u8),
    Mtx(u8),
    Main(u8),
}

/// `/cfg/mon/1|2/...` (PHONES, SPEAKERS). EQ bands 0 (low shelf) .. 7 (high shelf).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MonF {
    Level,
    Inv,
    Pan,
    Width,
    EqOn,
    EqGain(u8),
    EqFreq(u8),
    EqQ(u8),
    Limiter,
    DelayOn,
    DelayM,
    Dim,
    PflDim,
    BandSolo,
    SrcLevel,
    SrcMix,
    Src,
    DirectIn,
}

/// `/cfg/rta/rta*` (HOME's RTA SETTINGS) and `eq*` (EQ/RTA SETTINGS).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RtaF {
    Decay,
    Detector,
    Range,
    Gain,
    AutoGain,
}

/// `/io/in/<group>/<n>/...`: a source's settings.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SrcF {
    Gain,
    Phantom,
    Mute,
    Pol,
    Colour,
    Name,
    Icon,
    Tags,
    MuteState,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum InfoF {
    Name,
    Firmware,
    Serial,
    Model,
}

/// What a node of the tree is in Live DAW.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Binding {
    /// No equivalent: the shadow store.
    None,
    /// Strip kind, WING number (1-based), parameter.
    Strip(StripKind, u16, SF),
    Dca(u8, DcaF),
    MuteGroup(u8, bool),
    /// FX slot (0-based): the slot node itself, its model, its FX MIX.
    FxSlot(u8),
    FxModel(u8),
    FxMix(u8),
    Ctl(CtlF),
    Info(InfoF),
    /// /io/out/<A|B|C>/<n>/grp (false) or /in (true).
    Out(u8, u16, bool),
    /// /io/in/<group>/<n>/mode: the source's MONO / STEREO.
    SourceMode(&'static str, u16),
    Talk(TalkF),
    /// Talk path 0 = A, 1 = B.
    TalkPath(u8, TalkPathF),
    /// Monitor path as the engine numbers them: 0 = SPEAKERS (`/cfg/mon/2`), 1 = PHONES (`/cfg/mon/1`).
    Mon(u8, MonF),
    /// A source of `port::SOURCE_GROUPS`.
    Source(&'static str, u16, SrcF),
    /// RTA settings: false = HOME (`rta*`), true = EQ (`eq*`).
    Rta(bool, RtaF),
}

fn strip_kind(name: &str) -> Option<StripKind> {
    Some(match name {
        "ch" => StripKind::Channel,
        "aux" => StripKind::Aux,
        "bus" => StripKind::Bus,
        "main" => StripKind::Main,
        "mtx" => StripKind::Matrix,
        _ => return None,
    })
}

fn strip_field(kind: StripKind, n: u16, rest: &[&str]) -> Option<SF> {
    use StripKind::*;
    let input = kind.is_input();
    let ch = kind == Channel;
    let out = kind.is_output();
    Some(match rest {
        ["name"] => SF::Name,
        ["$name"] => SF::ActName,
        ["col"] => SF::Colour,
        ["$col"] => SF::ActColour,
        ["icon"] => SF::Icon,
        ["$icon"] => SF::ActIcon,
        ["led"] => SF::Led,
        ["mute"] => SF::Mute,
        ["fdr"] => SF::Fader,
        ["pan"] => SF::Pan,
        ["wid"] => SF::Width,
        ["$solo"] => SF::Solo,
        ["$sololed"] => SF::SoloLed,
        ["solosafe"] => SF::SoloSafe,
        ["mon"] => SF::Mon,
        ["$fdr"] => SF::EffFader,
        ["$mute"] => SF::EffMute,
        ["proc"] if ch => SF::Proc,
        ["ptap"] if ch => SF::Tap,
        ["tags"] => SF::Tags,
        ["busmono"] if out => SF::BusMono,
        ["in", "set", "trim"] => SF::Trim,
        ["in", "set", "bal"] => SF::Bal,
        ["in", "set", "inv"] => SF::Inv,
        ["in", "set", "dlyon"] if input => SF::DlyOn,
        ["in", "set", "dlymode"] if input => SF::DlyMode,
        ["in", "set", "dly"] if input => SF::Dly,
        ["in", "set", "altsrc"] if input => SF::AltSel,
        ["in", "set", "$mode"] if input => SF::Mode,
        ["in", "conn", f] if input => match *f {
            "grp" => SF::Conn { alt: false, number: false },
            "in" => SF::Conn { alt: false, number: true },
            "altgrp" => SF::Conn { alt: true, number: false },
            "altin" => SF::Conn { alt: true, number: true },
            _ => return None,
        },
        ["dly", "on"] if out => SF::DlyOn,
        ["dly", "mode"] if out => SF::DlyMode,
        ["dly", "dly"] if out => SF::Dly,
        ["flt"] if ch => SF::Slot(Blk::Tool),
        ["flt", f] if ch => match *f {
            "lc" => SF::LowCut,
            "lcf" => SF::LowCutF,
            "lcs" => SF::LowCutS,
            "hc" => SF::HighCut,
            "hcf" => SF::HighCutF,
            "hcs" => SF::HighCutS,
            "tf" => SF::Tool,
            "mdl" => SF::BlkMdl(Blk::Tool),
            _ => return None,
        },
        [b] if *b == "gate" && ch || *b == "dyn" || *b == "eq" => SF::Slot(blk_of(b)?),
        [b, f] if *b == "gate" && ch || *b == "dyn" || *b == "eq" => {
            let blk = blk_of(b)?;
            match *f {
                "on" => SF::BlkOn(blk),
                "mdl" => SF::BlkMdl(blk),
                "mix" => SF::BlkMix(blk),
                "gain" if blk != Blk::Eq => SF::BlkGain(blk),
                _ => return None,
            }
        }
        [b, f] if *b == "gatesc" && ch || *b == "dynsc" => {
            let blk = if *b == "gatesc" { Blk::Gate } else { Blk::Dyn };
            SF::Sc(
                blk,
                match *f {
                    "type" => ScF::Type,
                    "f" => ScF::F,
                    "q" => ScF::Q,
                    "src" => ScF::Src,
                    "tap" if ch => ScF::Tap,
                    _ => return None,
                },
            )
        }
        ["dynxo", f] if kind != Aux => SF::Xo(match *f {
            "depth" => XoF::Depth,
            "type" => XoF::Type,
            "f" => XoF::F,
            "q" => XoF::Q,
            _ => return None,
        }),
        ["preins", "on"] => SF::Ins(false, InsF::On),
        ["preins", "ins"] => SF::Ins(false, InsF::Fx),
        ["postins", "on"] if kind != Aux => SF::Ins(true, InsF::On),
        ["postins", "ins"] if kind != Aux => SF::Ins(true, InsF::Fx),
        ["postins", "mode"] if ch => SF::Ins(true, InsF::Mode),
        ["postins", "w"] if ch => SF::Ins(true, InsF::W),
        ["main", i, f] if !matches!(kind, Main | Matrix) => {
            let i: u8 = i.parse().ok().filter(|i| (1..=4).contains(i))?;
            SF::Main(
                i - 1,
                match *f {
                    "on" => MainF::On,
                    "lvl" => MainF::Lvl,
                    "pre" => MainF::Pre,
                    _ => return None,
                },
            )
        }
        ["send", t, f] => {
            if let Some(m) = t.strip_prefix("MX") {
                let m: u8 = m.parse().ok().filter(|m| (1..=8).contains(m))?;
                let field = match *f {
                    "on" => SendF::On,
                    "lvl" => SendF::Lvl,
                    "pon" if input => SendF::Pon,
                    "mode" if input => SendF::Mode,
                    "pan" if input => SendF::Pan,
                    "pre" if !input => SendF::Pre,
                    _ => return None,
                };
                if kind == Matrix {
                    return None;
                }
                SF::Mtx(m - 1, field)
            } else {
                let b: u16 = t.parse().ok().filter(|b| (1..=16).contains(b))?;
                if input {
                    SF::Send(
                        b - 1,
                        match *f {
                            "on" => SendF::On,
                            "lvl" => SendF::Lvl,
                            "pon" => SendF::Pon,
                            "mode" => SendF::Mode,
                            "plink" => SendF::Plink,
                            "pan" => SendF::Pan,
                            _ => return None,
                        },
                    )
                } else if kind == Bus && n >= 9 && b <= 8 {
                    // Buses 9+ feed buses 1-8 (the engine's bus-to-bus sends).
                    SF::Send(
                        b - 1,
                        match *f {
                            "on" => SendF::On,
                            "lvl" => SendF::Lvl,
                            "pre" => SendF::Pre,
                            _ => return None,
                        },
                    )
                } else {
                    return None;
                }
            }
        }
        ["tapwid"] if ch => SF::TapWidth,
        ["peq", "on"] if ch => SF::TapEqOn,
        ["peq", f] if ch => {
            let (band, which) = f.split_at(f.len().min(1));
            let band: u8 = band.parse().ok().filter(|b| (1..=3).contains(b))?;
            SF::TapEq(
                band - 1,
                match which {
                    "g" => TapF::Gain,
                    "f" => TapF::Freq,
                    "q" => TapF::Q,
                    _ => return None,
                },
            )
        }
        ["dir", "on"] if kind == Matrix => SF::DirOn,
        ["dir", "lvl"] if kind == Matrix => SF::DirLvl,
        ["dir", "inv"] if kind == Matrix => SF::DirInv,
        _ => return None,
    })
}

fn blk_of(name: &str) -> Option<Blk> {
    Some(match name {
        "gate" => Blk::Gate,
        "dyn" => Blk::Dyn,
        "eq" => Blk::Eq,
        _ => return None,
    })
}

/// The binding of a static node, from its path.
pub fn bind(parts: &[&str]) -> Binding {
    let number = |s: &str, max: u16| s.parse::<u16>().ok().filter(|n| (1..=max).contains(n));
    match parts {
        [k, n, rest @ ..] if strip_kind(k).is_some() => {
            let kind = strip_kind(k).unwrap_or(StripKind::Channel);
            let max = match kind {
                StripKind::Channel => 40,
                StripKind::Aux | StripKind::Matrix => 8,
                StripKind::Bus => 16,
                StripKind::Main => 4,
            };
            match number(n, max).and_then(|n| strip_field(kind, n, rest).map(|f| (n, f))) {
                Some((n, f)) => Binding::Strip(kind, n, f),
                None => Binding::None,
            }
        }
        ["dca", n, f] => match number(n, NUM_DCAS as u16) {
            Some(n) => {
                let i = n as u8 - 1;
                match *f {
                    "name" => Binding::Dca(i, DcaF::Name),
                    "col" => Binding::Dca(i, DcaF::Colour),
                    "mute" => Binding::Dca(i, DcaF::Mute),
                    "fdr" => Binding::Dca(i, DcaF::Fader),
                    "$solo" => Binding::Dca(i, DcaF::Solo),
                    "$sololed" => Binding::Dca(i, DcaF::SoloLed),
                    _ => Binding::None,
                }
            }
            None => Binding::None,
        },
        ["mgrp", n, f] => match (number(n, NUM_MUTE_GROUPS as u16), *f) {
            (Some(n), "name") => Binding::MuteGroup(n as u8 - 1, false),
            (Some(n), "mute") => Binding::MuteGroup(n as u8 - 1, true),
            _ => Binding::None,
        },
        ["fx", n] => number(n, NUM_FX as u16).map_or(Binding::None, |n| Binding::FxSlot(n as u8 - 1)),
        ["fx", n, "mdl"] => number(n, NUM_FX as u16).map_or(Binding::None, |n| Binding::FxModel(n as u8 - 1)),
        ["fx", n, "fxmix"] => number(n, NUM_FX as u16).map_or(Binding::None, |n| Binding::FxMix(n as u8 - 1)),
        ["cfg", "mainlink"] => Binding::Ctl(CtlF::MainLink),
        ["cfg", "dcamgrp"] => Binding::Ctl(CtlF::DcaMuteGroups),
        ["cfg", "solo", "chtap"] => Binding::Ctl(CtlF::SoloAfl(StripKind::Channel)),
        ["cfg", "solo", "bustap"] => Binding::Ctl(CtlF::SoloAfl(StripKind::Bus)),
        ["cfg", "solo", "maintap"] => Binding::Ctl(CtlF::SoloAfl(StripKind::Main)),
        ["cfg", "solo", "mtxtap"] => Binding::Ctl(CtlF::SoloAfl(StripKind::Matrix)),
        ["cfg", "solo", "$dim"] => Binding::Ctl(CtlF::MonitorDim),
        ["cfg", "solo", "$mono"] => Binding::Ctl(CtlF::MonitorMono),
        ["cfg", "solo", "$flip"] => Binding::Ctl(CtlF::MonitorSwap),
        ["cfg", "mon", "2", "lvl"] => Binding::Ctl(CtlF::MonitorLevel),
        ["cfg", "mon", "2", "dim"] => Binding::Ctl(CtlF::MonitorDimDb),
        ["cfg", "mon", n, rest @ ..] => match (*n, mon_field(rest)) {
            ("1", Some(f)) => Binding::Mon(1, f),
            ("2", Some(f)) => Binding::Mon(0, f),
            _ => Binding::None,
        },
        ["cfg", "solo", "mode"] => Binding::Ctl(CtlF::SoloMode),
        ["cfg", "solo", "mon"] => Binding::Ctl(CtlF::SoloBus),
        ["cfg", "solo", "mute"] => Binding::Ctl(CtlF::MonitorMute),
        ["cfg", "solo", "srcsolo"] => Binding::Ctl(CtlF::SourceSolo),
        ["cfg", "solo", "$srcsolo"] => Binding::Ctl(CtlF::SourceSoloOn),
        ["cfg", "solo", "$srcsgrp"] => Binding::Ctl(CtlF::SourceSoloGroup),
        ["cfg", "solo", "$srcsin"] => Binding::Ctl(CtlF::SourceSoloIn),
        ["$ctl", "cfg", "muteovr"] => Binding::Ctl(CtlF::MuteOverride),
        ["$stat", "sip"] => Binding::Ctl(CtlF::SipActive),
        ["cfg", "rta", f] => {
            let (eq, rest) = match (f.strip_prefix("rta"), f.strip_prefix("eq")) {
                (Some(r), _) => (false, r),
                (_, Some(r)) => (true, r),
                _ => (false, ""),
            };
            match rest {
                "decay" => Binding::Rta(eq, RtaF::Decay),
                "det" => Binding::Rta(eq, RtaF::Detector),
                "range" => Binding::Rta(eq, RtaF::Range),
                "gain" => Binding::Rta(eq, RtaF::Gain),
                "auto" => Binding::Rta(eq, RtaF::AutoGain),
                _ => Binding::None,
            }
        }
        ["cfg", "talk", "assign"] => Binding::Talk(TalkF::Assign),
        ["cfg", "talk", "lvl"] => Binding::Talk(TalkF::Level),
        ["cfg", "talk", p, f] => match (*p, talk_path_field(f)) {
            ("A", Some(f)) => Binding::TalkPath(0, f),
            ("B", Some(f)) => Binding::TalkPath(1, f),
            _ => Binding::None,
        },
        ["cfg", "amix", "x"] => Binding::Ctl(CtlF::AutomixX),
        ["cfg", "amix", "y"] => Binding::Ctl(CtlF::AutomixY),
        ["io", "altsw"] => Binding::Ctl(CtlF::GlobalAlt),
        ["io", "autoaltovr"] => Binding::Ctl(CtlF::InputSelectGlobal),
        ["io", "out", g, n, f] => match (port::BLOCKS.iter().position(|b| b == g), number(n, 64)) {
            (Some(b), Some(n)) if n as i32 <= port::BLOCK && (*f == "grp" || *f == "in") => Binding::Out(b as u8, n, *f == "in"),
            _ => Binding::None,
        },
        ["io", "in", g, n, "mode"] => match (port_group(g), number(n, 64)) {
            (Some(g), Some(n)) => Binding::SourceMode(g, n),
            _ => Binding::None,
        },
        ["io", "in", g, n, f] => match (port::SOURCE_GROUPS.iter().find(|x| *x == g), number(n, 64), source_field(f)) {
            (Some(g), Some(n), Some(f)) if port::wing_to_source(g, n as i32).and_then(sources::source_index).is_some() => {
                Binding::Source(g, n, f)
            }
            _ => Binding::None,
        },
        ["$globals", "startmute"] => Binding::Ctl(CtlF::StartupMainMute),
        ["$stat", "solo"] => Binding::Ctl(CtlF::AnySolo),
        ["$syscfg", "consolename"] => Binding::Info(InfoF::Name),
        ["$syscfg", "$firmware"] => Binding::Info(InfoF::Firmware),
        ["$syscfg", "$serial"] => Binding::Info(InfoF::Serial),
        ["$syscfg", "$cnsmdl"] => Binding::Info(InfoF::Model),
        _ => Binding::None,
    }
}

fn mon_field(rest: &[&str]) -> Option<MonF> {
    let last = MONITOR_EQ_BANDS as u8 - 1;
    Some(match rest {
        ["$lvl"] | ["lvl"] => MonF::Level,
        ["inv"] => MonF::Inv,
        ["pan"] => MonF::Pan,
        ["wid"] => MonF::Width,
        ["eq", "on"] => MonF::EqOn,
        ["eq", "lsg"] => MonF::EqGain(0),
        ["eq", "lsf"] => MonF::EqFreq(0),
        ["eq", "hsg"] => MonF::EqGain(last),
        ["eq", "hsf"] => MonF::EqFreq(last),
        ["eq", f] => {
            let (band, which) = f.split_at(f.len().min(1));
            let band: u8 = band.parse().ok().filter(|b| (1..=6).contains(b))?;
            match which {
                "g" => MonF::EqGain(band),
                "f" => MonF::EqFreq(band),
                "q" => MonF::EqQ(band),
                _ => return None,
            }
        }
        ["lim"] => MonF::Limiter,
        ["dly", "on"] => MonF::DelayOn,
        ["dly", "m"] => MonF::DelayM,
        ["dim"] => MonF::Dim,
        ["pfldim"] => MonF::PflDim,
        ["eqbdtrim"] => MonF::BandSolo,
        ["srclvl"] => MonF::SrcLevel,
        ["srcmix"] => MonF::SrcMix,
        ["src"] => MonF::Src,
        ["dirin"] => MonF::DirectIn,
        _ => return None,
    })
}

fn talk_path_field(f: &str) -> Option<TalkPathF> {
    let n = |prefix: &str, max: u8| f.strip_prefix(prefix).and_then(|k| k.parse::<u8>().ok()).filter(|k| (1..=max).contains(k));
    Some(match f {
        "$on" => TalkPathF::On,
        "mode" => TalkPathF::Mode,
        "mondim" => TalkPathF::MonDim,
        "busdim" => TalkPathF::BusDim,
        "indiv" => TalkPathF::Individual,
        _ => {
            if let Some(k) = n("MX", 8) {
                TalkPathF::Mtx(k - 1)
            } else if let Some(k) = n("M", 4) {
                TalkPathF::Main(k - 1)
            } else {
                TalkPathF::Bus(n("B", 16)? - 1)
            }
        }
    })
}

fn source_field(f: &str) -> Option<SrcF> {
    Some(match f {
        "g" => SrcF::Gain,
        "vph" => SrcF::Phantom,
        "mute" => SrcF::Mute,
        "pol" => SrcF::Pol,
        "col" => SrcF::Colour,
        "name" => SrcF::Name,
        "icon" => SrcF::Icon,
        "tags" => SrcF::Tags,
        "$mute" => SrcF::MuteState,
        _ => return None,
    })
}

/// The source table index of a WING source (`port::SOURCE_GROUPS` only).
fn source_of(group: &str, n: u16) -> Option<usize> {
    if !port::SOURCE_GROUPS.contains(&group) {
        return None;
    }
    port::wing_to_source(group, n as i32).and_then(sources::source_index)
}

/// A monitor path's SOURCE settings (0 = SPEAKERS, 1 = PHONES).
fn mon_source(c: &EngineControls, path: u8) -> &MonitorSourceParams {
    if path == 0 {
        &c.speakers
    } else {
        &c.phones.source
    }
}

fn mon_dsp(c: &EngineControls, path: u8) -> &MonitorDspParams {
    &c.monitor.paths[(path as usize).min(1)]
}

/// A source's `$mute`: 0 off, 1 muted, 2 muted by a mute group.
fn source_mute_state(p: &SourceParams, mute_groups: u32) -> i32 {
    if p.mute.load(Relaxed) {
        1
    } else if p.is_muted(mute_groups) {
        2
    } else {
        0
    }
}

fn port_group(g: &str) -> Option<&'static str> {
    ["LCL", "AUX", "A", "B", "C", "SC", "USB", "CRD", "MOD", "AES", "USR"].into_iter().find(|x| *x == g)
}

/// Bindings of every static row (computed once).
pub fn bindings() -> &'static [Binding] {
    static B: OnceLock<Vec<Binding>> = OnceLock::new();
    B.get_or_init(|| {
        let s = schema();
        (0..s.num_rows() as u16)
            .map(|r| {
                let parts = s.row_parts(r);
                let refs: Vec<&str> = parts.iter().map(|p| p.as_ref()).collect();
                bind(&refs)
            })
            .collect()
    })
}

/// The row of each input strip's main patch group (`/ch/N/in/conn/grp`).
fn conn_rows() -> &'static HashMap<(StripKind, u16), u16> {
    static R: OnceLock<HashMap<(StripKind, u16), u16>> = OnceLock::new();
    R.get_or_init(|| {
        bindings()
            .iter()
            .enumerate()
            .filter_map(|(r, b)| match b {
                Binding::Strip(kind, n, SF::Conn { alt: false, number: false }) => Some(((*kind, *n), r as u16)),
                _ => None,
            })
            .collect()
    })
}

// ---- level conversions ---------------------------------------------------------------

/// Engine dB (<= -100 is silence) to WING dB (-144 = -oo).
pub fn db_to_wing(db: f32) -> f32 {
    if db <= MINUS_INF_DB {
        FADER_MIN
    } else {
        db.min(FADER_MAX)
    }
}

pub fn db_from_wing(db: f32) -> f32 {
    db.clamp(MINUS_INF_DB, FADER_MAX)
}

// ---- errors -----------------------------------------------------------------------

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SetError {
    NotFound,
    IsNode,
    ReadOnly,
    BadValue,
    /// The value is valid on the WING but Live DAW can't do it.
    Unsupported(String),
}

impl std::fmt::Display for SetError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            SetError::NotFound => write!(f, "node not found"),
            SetError::IsNode => write!(f, "node is not a parameter"),
            SetError::ReadOnly => write!(f, "read-only"),
            SetError::BadValue => write!(f, "value error"),
            SetError::Unsupported(why) => write!(f, "not supported: {why}"),
        }
    }
}

// ---- the console view ------------------------------------------------------------------

/// The engine seen as a WING: get, set and enumerate by node. Holds a snapshot
/// of the strip list, so build one per request (or poll) and drop it.
pub struct Console<'a> {
    pub engine: &'a Engine,
    pub state: &'a WingState,
    channels: Vec<Arc<Strip>>,
    aux: Vec<Arc<Strip>>,
    buses: Vec<Option<Arc<Strip>>>,
    mains: Vec<Arc<Strip>>,
    matrices: Vec<Arc<Strip>>,
}

impl ModelSource for Console<'_> {
    fn active_model(&self, slot: u16) -> Option<usize> {
        let s = schema();
        let row = s.slots[slot as usize].row;
        let token = match bindings()[row as usize] {
            Binding::Strip(kind, n, SF::Slot(blk)) => {
                let st = self.strip(kind, n)?;
                block_spec(&st.params, kind, blk).0.token
            }
            Binding::FxSlot(i) => FX_MODELS[self.engine.controls.fx.get(i as usize)?.model_index()].token,
            _ => return None,
        };
        s.model_index(slot, token)
    }
}

/// The model table and current spec of a strip block.
fn block_spec(p: &StripParams, kind: StripKind, blk: Blk) -> (&'static ModelSpec, &'static [ModelSpec]) {
    match blk {
        Blk::Gate => (&DYN_MODELS[p.gate.model_index()], DYN_MODELS),
        Blk::Dyn => (&DYN_MODELS[p.dynamics.model_index()], DYN_MODELS),
        Blk::Eq => {
            let bus = kind.is_output();
            let table = if bus { EQ_MODELS_BUS } else { EQ_MODELS_CH };
            (&table[p.eq.model_index(bus)], table)
        }
        Blk::Tool => {
            let i = (p.filter.tool_model.load(Relaxed).max(0) as usize).min(FILTER_TOOLS.len() - 1);
            (&FILTER_TOOLS[i], FILTER_TOOLS)
        }
    }
}

fn block_values(p: &StripParams, blk: Blk) -> &[AtomicF32] {
    match blk {
        Blk::Gate => &p.gate.values,
        Blk::Dyn => &p.dynamics.values,
        Blk::Eq => &p.eq.values,
        Blk::Tool => &p.filter.tool_params,
    }
}

fn dyn_slot(p: &StripParams, blk: Blk) -> &DynSlotParams {
    if blk == Blk::Gate {
        &p.gate
    } else {
        &p.dynamics
    }
}

fn b(v: &AtomicBool) -> Value {
    Value::Int(v.load(Relaxed) as i32)
}
fn f(v: &AtomicF32) -> Value {
    Value::Float(v.load())
}
fn lvl(v: &AtomicF32) -> Value {
    Value::Float(db_to_wing(v.load()))
}
fn int(v: &AtomicI32) -> Value {
    Value::Int(v.load(Relaxed))
}

/// An engine index as an item of the node's enum.
fn item(def: &Def, index: i32) -> Value {
    usize::try_from(index)
        .ok()
        .and_then(|i| value::item_value(def, i))
        .unwrap_or_else(|| value::item_value(def, 0).unwrap_or(Value::Int(0)))
}

fn set_b(a: &AtomicBool, v: &Value) {
    a.store(v.as_bool(), Relaxed);
}
fn set_f(a: &AtomicF32, v: &Value) {
    if let Some(x) = v.as_f32() {
        a.store(x);
    }
}
fn set_lvl(a: &AtomicF32, v: &Value) {
    if let Some(x) = v.as_f32() {
        a.store(db_from_wing(x));
    }
}
fn set_item(a: &AtomicI32, def: &Def, v: &Value) {
    if let Some(i) = value::enum_index(def, v) {
        a.store(i as i32, Relaxed);
    }
}

fn fx_item(fx: i32) -> Value {
    if fx < 0 {
        Value::str("NONE")
    } else {
        Value::Str(format!("FX{}", fx + 1))
    }
}

fn fx_from_item(v: &Value) -> Option<i32> {
    let s = match v {
        Value::Str(s) => s.clone(),
        other => return other.as_i32().map(|i| i - 1),
    };
    if s.eq_ignore_ascii_case("NONE") {
        return Some(-1);
    }
    let n: i32 = s.trim_start_matches(|c: char| c.is_ascii_alphabetic()).parse().ok()?;
    (1..=NUM_FX as i32).contains(&n).then_some(n - 1)
}

/// "#D1,#M3": DCA and mute group memberships plus other tags.
fn compose_tags(dca: u32, mgrp: u32, extra: &str) -> String {
    let mut tags: Vec<String> = Vec::new();
    for i in 0..NUM_DCAS {
        if dca & (1 << i) != 0 {
            tags.push(format!("#D{}", i + 1));
        }
    }
    for i in 0..NUM_MUTE_GROUPS {
        if mgrp & (1 << i) != 0 {
            tags.push(format!("#M{}", i + 1));
        }
    }
    tags.extend(extra.split(',').map(str::trim).filter(|t| !t.is_empty()).map(str::to_owned));
    tags.join(",")
}

/// Splits tags into (DCA mask, mute group mask, the other tags).
fn parse_tags(s: &str) -> (u32, u32, String) {
    let (mut dca, mut mgrp, mut extra) = (0u32, 0u32, Vec::new());
    for t in s.split(',').map(str::trim).filter(|t| !t.is_empty()) {
        let num = |p: &str| t.strip_prefix(p).and_then(|n| n.parse::<u32>().ok());
        match (num("#D"), num("#M")) {
            (Some(n), _) if (1..=NUM_DCAS as u32).contains(&n) => dca |= 1 << (n - 1),
            (_, Some(n)) if (1..=NUM_MUTE_GROUPS as u32).contains(&n) => mgrp |= 1 << (n - 1),
            _ => extra.push(t),
        }
    }
    (dca, mgrp, extra.join(","))
}

/// A model parameter between our table (choices stored as indices) and the WING (items, values).
fn param_to_wing(spec_param: &livedaw_dsp::wing::ParamSpec, wdef: &Def, v: f32) -> Value {
    match spec_param.kind {
        ParamKind::Choice(items) => {
            let text = items.get(v.max(0.0) as usize).copied().unwrap_or("");
            match wdef.ty {
                NodeType::FloatEnum => Value::Float(value::parse_number(text).unwrap_or(0.0)),
                _ => Value::str(text),
            }
        }
        ParamKind::Values(items) => {
            let x = items.get(v.max(0.0) as usize).copied().unwrap_or(0.0);
            match wdef.ty {
                NodeType::StrEnum => {
                    value::enum_index(wdef, &Value::Float(x)).and_then(|i| value::item_value(wdef, i)).unwrap_or(Value::Float(x))
                }
                _ => Value::Float(x),
            }
        }
        ParamKind::Toggle | ParamKind::Int => Value::Int(v.round() as i32),
        ParamKind::Linear | ParamKind::Log => {
            if wdef.ty == NodeType::Fader && v <= spec_param.lo() {
                Value::Float(FADER_MIN)
            } else if wdef.ty == NodeType::Int {
                Value::Int(v.round() as i32)
            } else {
                Value::Float(v)
            }
        }
    }
}

fn param_from_wing(spec_param: &livedaw_dsp::wing::ParamSpec, wdef: &Def, v: &Value) -> Option<f32> {
    let x = match spec_param.kind {
        ParamKind::Choice(items) => {
            let text = match v {
                Value::Str(s) => s.clone(),
                other => value::display(wdef, other),
            };
            // None for a choice our model doesn't have (e.g. an aux EQ's CUT band type).
            items.iter().position(|i| i.eq_ignore_ascii_case(text.trim()))? as f32
        }
        ParamKind::Values(items) => {
            let x = match v {
                Value::Str(s) => value::parse_number(s)?,
                other => other.as_f32()?,
            };
            items.iter().enumerate().min_by(|a, b| (a.1 - x).abs().total_cmp(&(b.1 - x).abs()))?.0 as f32
        }
        _ => v.as_f32()?,
    };
    Some(spec_param.sanitise(x))
}

impl<'a> Console<'a> {
    pub fn new(engine: &'a Engine, state: &'a WingState) -> Self {
        let mut buses: Vec<Option<Arc<Strip>>> = vec![None; 16];
        for b in engine.strips(StripKind::Bus) {
            if let Some(slot) = buses.get_mut(b.id() as usize) {
                *slot = Some(b);
            }
        }
        Self {
            engine,
            state,
            channels: engine.strips(StripKind::Channel),
            aux: engine.strips(StripKind::Aux),
            buses,
            mains: engine.strips(StripKind::Main),
            matrices: engine.strips(StripKind::Matrix),
        }
    }

    /// A strip by kind and WING number.
    pub fn strip(&self, kind: StripKind, n: u16) -> Option<&Arc<Strip>> {
        let i = (n as usize).checked_sub(1)?;
        match kind {
            StripKind::Channel => self.channels.get(i),
            StripKind::Aux => self.aux.get(i),
            StripKind::Bus => self.buses.get(i)?.as_ref(),
            StripKind::Main => self.mains.get(i),
            StripKind::Matrix => self.matrices.get(i),
        }
    }

    pub fn num_channels(&self) -> usize {
        self.channels.len()
    }

    pub fn binding(&self, node: Node) -> Binding {
        match node {
            Node::Row(r) => bindings()[r as usize],
            _ => Binding::None,
        }
    }

    /// True if the node is backed by the engine (else the shadow store).
    pub fn is_mapped(&self, node: Node) -> bool {
        match node {
            Node::Root => false,
            Node::Row(r) => match bindings()[r as usize] {
                Binding::None => false,
                Binding::Strip(kind, n, _) => self.strip(kind, n).is_some(),
                _ => true,
            },
            Node::Param { slot, index } => self.engine_param(slot, index).is_some(),
        }
    }

    pub fn def(&self, node: Node) -> Option<&'static Def> {
        schema().def(node, self)
    }

    fn shadow_key(&self, node: Node) -> Option<ShadowKey> {
        match node {
            Node::Root => None,
            Node::Row(r) => Some(ShadowKey::Row(r)),
            Node::Param { slot, index } => Some(ShadowKey::Param(slot, self.active_model(slot)? as u16, index)),
        }
    }

    /// The engine block behind a model parameter: (values, spec, param index).
    fn engine_param(&self, slot: u16, index: u8) -> Option<(&[AtomicF32], &'static ModelSpec, usize)> {
        let s = schema();
        let name = s.name(Node::Param { slot, index }, self);
        let row = s.slots[slot as usize].row;
        let (values, spec): (&[AtomicF32], &'static ModelSpec) = match bindings()[row as usize] {
            Binding::Strip(kind, n, SF::Slot(blk)) => {
                let st = self.strip(kind, n)?;
                (block_values(&st.params, blk), block_spec(&st.params, kind, blk).0)
            }
            Binding::FxSlot(i) => {
                let p = self.engine.controls.fx.get(i as usize)?;
                (&p.values, &FX_MODELS[p.model_index()])
            }
            _ => return None,
        };
        let i = spec.param_index(&name)?;
        values.get(i)?;
        Some((values, spec, i))
    }

    /// The current value of a parameter (None for nodes and unknown nodes).
    pub fn get(&self, node: Node) -> Option<Value> {
        let def = self.def(node)?;
        if !def.is_leaf() {
            return None;
        }
        let mapped = match node {
            Node::Root => None,
            Node::Row(r) => self.get_row(r, def),
            Node::Param { slot, index } => {
                self.engine_param(slot, index).map(|(values, spec, i)| param_to_wing(&spec.params[i], def, values[i].load()))
            }
        };
        if let Some(v) = mapped {
            return Some(v);
        }
        Some(self.shadow_key(node).and_then(|k| self.state.shadow_get(k)).unwrap_or_else(|| value::default_value(def)))
    }

    fn get_row(&self, row: u16, def: &Def) -> Option<Value> {
        let e = self.engine;
        let c = &e.controls;
        Some(match bindings()[row as usize] {
            Binding::None | Binding::FxSlot(_) => return None,
            Binding::Strip(kind, n, field) => return self.get_strip(row, def, kind, n, field),
            Binding::Dca(i, field) => {
                let d = c.dcas.get(i as usize)?;
                match field {
                    DcaF::Name => Value::Str(e.dca_name(i as usize)),
                    DcaF::Colour => int(&d.colour),
                    DcaF::Mute => b(&d.mute),
                    DcaF::Fader => lvl(&d.fader_db),
                    DcaF::Solo | DcaF::SoloLed => b(&d.solo),
                }
            }
            Binding::MuteGroup(i, true) => Value::bool(c.mute_groups.load(Relaxed) & (1 << i) != 0),
            Binding::MuteGroup(i, false) => Value::Str(e.mute_group_name(i as usize)),
            Binding::FxModel(i) => Value::str(FX_MODELS[c.fx.get(i as usize)?.model_index()].token),
            Binding::FxMix(i) => f(&c.fx.get(i as usize)?.mix),
            Binding::Ctl(ctl) => match ctl {
                CtlF::MainLink => item(def, c.main_link.load(Relaxed)),
                CtlF::DcaMuteGroups => b(&c.dca_mute_groups),
                CtlF::SoloAfl(kind) => item(def, c.solo_is_afl(kind) as i32),
                CtlF::MonitorLevel => lvl(&c.monitor_level_db),
                CtlF::MonitorDimDb => f(&c.monitor_dim_db),
                CtlF::MonitorDim => b(&c.monitor_dim),
                CtlF::MonitorMono => b(&c.monitor_mono),
                CtlF::MonitorSwap => b(&c.monitor_swap),
                CtlF::AutomixX => b(&c.automix_enable[0]),
                CtlF::AutomixY => b(&c.automix_enable[1]),
                CtlF::GlobalAlt => b(&c.global_alt),
                CtlF::InputSelectGlobal => b(&c.input_select_global),
                CtlF::StartupMainMute => b(&c.startup_main_mute),
                CtlF::AnySolo => b(&c.any_solo_active),
                CtlF::SoloMode => item(def, c.solo_mode.load(Relaxed)),
                CtlF::SoloBus => item(def, c.solo_bus.load(Relaxed)),
                CtlF::MonitorMute => b(&c.monitor_mute),
                CtlF::MuteOverride => b(&c.mute_override),
                CtlF::SipActive => b(&c.sip_active),
                CtlF::SourceSolo => item(def, c.monitor.source_solo.load(Relaxed)),
                CtlF::SourceSoloOn => Value::bool(self.source_solo_on()),
                CtlF::SourceSoloGroup | CtlF::SourceSoloIn => {
                    let (g, n) = port::source_to_wing(c.monitor.source_solo_port.load(Relaxed)).filter(|(g, _)| *g != "OFF")?;
                    if ctl == CtlF::SourceSoloIn {
                        Value::Int(n)
                    } else {
                        Value::Int(port::IN_GROUPS.iter().position(|x| *x == g).map_or(1, |i| i as i32 + 1))
                    }
                }
            },
            Binding::Talk(f) => {
                let t = &c.talk;
                match f {
                    TalkF::Assign => item(
                        def,
                        match t.assignment() {
                            Some((StripKind::Channel, 39)) => 1,
                            Some((StripKind::Aux, 7)) => 2,
                            _ => 0, // OFF, or a channel the WING can't host talkback on
                        },
                    ),
                    TalkF::Level => lvl(&t.level_db),
                }
            }
            Binding::TalkPath(i, tf) => {
                let p = c.talk.paths.get(i as usize)?;
                let bit = |mask: &std::sync::atomic::AtomicU32, k: u8| Value::bool(mask.load(Relaxed) & (1 << k) != 0);
                match tf {
                    TalkPathF::On => b(&p.on),
                    TalkPathF::Mode => item(def, p.mode.load(Relaxed)),
                    TalkPathF::MonDim => f(&p.mon_dim_db),
                    TalkPathF::BusDim => f(&p.bus_dim_db),
                    TalkPathF::Individual => b(&p.individual),
                    TalkPathF::Bus(k) => bit(&p.buses, k),
                    TalkPathF::Mtx(k) => bit(&p.matrices, k),
                    TalkPathF::Main(k) => bit(&p.mains, k),
                }
            }
            Binding::Mon(path, mf) => {
                let d = mon_dsp(c, path);
                let src = mon_source(c, path);
                match mf {
                    MonF::Level => lvl(if path == 0 { &c.monitor_level_db } else { &c.phones.level_db }),
                    MonF::Dim => f(if path == 0 { &c.monitor_dim_db } else { &c.phones.dim_db }),
                    MonF::Inv => b(&d.invert),
                    MonF::Pan => f(&d.pan),
                    MonF::Width => f(&d.width),
                    MonF::EqOn => b(&d.eq_on),
                    MonF::EqGain(k) => f(&d.eq.get(k as usize)?.gain_db),
                    MonF::EqFreq(k) => f(&d.eq.get(k as usize)?.freq),
                    MonF::EqQ(k) => f(&d.eq.get(k as usize)?.q),
                    MonF::Limiter => f(&d.limiter_db),
                    MonF::DelayOn => b(&d.delay_on),
                    MonF::DelayM => f(&d.delay_m),
                    MonF::PflDim => f(&src.pfl_dim_db),
                    MonF::BandSolo => f(&d.band_solo_db),
                    MonF::SrcLevel => lvl(&src.source_level_db),
                    MonF::SrcMix => lvl(&src.source_mix_db),
                    // OFF is item 0; MAIN.1 (our source 0) item 1.
                    MonF::Src => item(def, src.source.load(Relaxed).max(-1) + 1),
                    MonF::DirectIn => item(def, d.direct_source.load(Relaxed).max(-1) + 1),
                }
            }
            Binding::Rta(eq, rf) => {
                let a = e.analysis().controls();
                let v = if eq { &a.eq } else { &a.home };
                match rf {
                    RtaF::Decay => item(def, v.decay.load(Relaxed)),
                    RtaF::Detector => item(def, v.detector.load(Relaxed)),
                    RtaF::Range => item(def, v.range.load(Relaxed)),
                    RtaF::Gain => f(&v.gain_db),
                    RtaF::AutoGain => b(&v.autogain),
                }
            }
            Binding::Source(group, n, sf) => {
                let index = source_of(group, n)?;
                let table = e.sources();
                let p = table.get(index)?;
                match sf {
                    SrcF::Gain => f(&p.gain_db),
                    SrcF::Phantom => b(&p.phantom),
                    SrcF::Mute => b(&p.mute),
                    SrcF::Pol => b(&p.invert),
                    SrcF::Colour => int(&p.colour),
                    SrcF::Name => Value::Str(table.name(index)),
                    SrcF::Icon => int(&p.icon),
                    SrcF::Tags => {
                        let (dca, extra) = match self.state.shadow_get(ShadowKey::Row(row)) {
                            Some(Value::Str(t)) => {
                                let (dca, _, extra) = parse_tags(&t);
                                (dca, extra)
                            }
                            _ => (0, String::new()),
                        };
                        Value::Str(compose_tags(dca, p.mute_groups.load(Relaxed), &extra))
                    }
                    SrcF::MuteState => Value::Int(source_mute_state(p, c.mute_groups.load(Relaxed))),
                }
            }
            Binding::Info(info) => {
                let id = self.state.identity();
                Value::Str(match info {
                    InfoF::Name => id.name,
                    InfoF::Firmware => id.firmware,
                    InfoF::Serial => id.serial,
                    InfoF::Model => id.model,
                })
            }
            Binding::Out(block, n, number) => {
                let port = port::output_port(block as usize, n as i32);
                let (g, k) = match self.output_source(port) {
                    Some(gk) => gk,
                    None => match self.shadow_pair(row, "grp", "in") {
                        // A source Live DAW can't route, as set; else OFF with the number set.
                        Some((sg, sk)) => (unroutable_group(row, &sg).unwrap_or("OFF"), sk),
                        None => return None,
                    },
                };
                if number {
                    Value::Int(k)
                } else {
                    Value::str(g)
                }
            }
            Binding::SourceMode(group, n) if source_of(group, n).is_some() => {
                // Live DAW's sources pair like the WING's (ROUTING > SOURCES).
                let p = e.sources().get(source_of(group, n)?)?;
                item(def, p.mode.load(Relaxed))
            }
            Binding::SourceMode(group, n) => {
                let src = port::wing_to_source(group, n as i32)?;
                // A stereo strip patched to this source (as the WING shows the patch).
                let rows = conn_rows();
                let stereo =
                    [(StripKind::Channel, &self.channels), (StripKind::Aux, &self.aux)].into_iter().any(|(kind, list)| {
                        list.iter().enumerate().any(|(i, s)| {
                            let p = &s.params;
                            p.input_left.load(Relaxed) == src
                                && p.stereo.load(Relaxed)
                                && rows
                                    .get(&(kind, i as u16 + 1))
                                    .is_some_and(|&r| self.conn_pair(r, p, false) == (group, n as i32))
                        })
                    });
                if !stereo {
                    return None; // the source's own setting (shadow)
                }
                match self.state.shadow_get(ShadowKey::Row(row)) {
                    Some(Value::Str(m)) if m == "M/S" => Value::str("M/S"),
                    _ => Value::str("ST"),
                }
            }
        })
    }

    /// What feeds hardware output `port`: ("MAIN", 1) etc. None: nothing Live DAW knows of.
    fn output_source(&self, port: i32) -> Option<(&'static str, i32)> {
        let lists: [(&'static str, Vec<&Arc<Strip>>); 3] = [
            ("BUS", self.buses.iter().flatten().collect()),
            ("MAIN", self.mains.iter().collect()),
            ("MTX", self.matrices.iter().collect()),
        ];
        for (g, list) in &lists {
            for (i, s) in list.iter().enumerate() {
                let number = if *g == "BUS" { s.id() } else { i as i32 };
                if s.params.output_left.load(Relaxed) == port {
                    return Some((g, 2 * number + 1));
                }
                if s.params.output_right.load(Relaxed) == port {
                    return Some((g, 2 * number + 2));
                }
            }
        }
        let c = &self.engine.controls;
        if c.monitor_output_left.load(Relaxed) == port {
            return Some(("MON", 1));
        }
        if c.monitor_output_right.load(Relaxed) == port {
            return Some(("MON", 2));
        }
        None
    }

    /// The input patch of a strip as the WING shows it. Live DAW stores one
    /// port per input; the (group, number) a client or snapshot set is kept, and
    /// shown while it still names that port (LCL 5 and A 5 are the same port
    /// here), or when Live DAW can't patch it at all (a bus as a source).
    fn conn_pair(&self, row: u16, p: &StripParams, alt: bool) -> (&'static str, i32) {
        let src = if alt { p.alt_input_left.load(Relaxed) } else { p.input_left.load(Relaxed) };
        let (gname, nname) = if alt { ("altgrp", "altin") } else { ("grp", "in") };
        let grp_row = self.sibling(row, gname);
        let shadow = match (
            grp_row.and_then(|r| self.state.shadow_get(ShadowKey::Row(r))),
            self.sibling(row, nname).and_then(|r| self.state.shadow_get(ShadowKey::Row(r))),
        ) {
            (Some(Value::Str(g)), Some(n)) => def_items_find(grp_row, &g).map(|g| (g, n.as_i32().unwrap_or(1))),
            _ => None,
        };
        match (shadow, port::source_to_wing(src)) {
            (Some((g, k)), _) if port::wing_to_source(g, k) == Some(src) => (g, k),
            (Some((g, k)), _) if src < 0 && port::wing_to_source(g, k).is_none() => (g, k),
            (Some((_, k)), _) if src < 0 => ("OFF", k),
            (_, Some(gk)) => gk,
            _ => ("OFF", 1),
        }
    }

    fn sibling(&self, row: u16, name: &str) -> Option<u16> {
        let s = schema();
        match s.child(s.parent(Node::Row(row)), name, &NoModels)? {
            Node::Row(r) => Some(r),
            _ => None,
        }
    }

    fn get_strip(&self, row: u16, def: &Def, kind: StripKind, n: u16, field: SF) -> Option<Value> {
        let s = self.strip(kind, n)?;
        let p = &s.params;
        Some(match field {
            SF::Name | SF::ActName => Value::Str(s.name()),
            SF::Colour | SF::ActColour => int(&p.colour),
            SF::Icon | SF::ActIcon => int(&p.icon),
            SF::Led => b(&p.led),
            SF::Mute => b(&p.mute),
            SF::Fader | SF::EffFader => lvl(&p.fader_db),
            SF::Pan => f(&p.pan),
            SF::Width => f(&p.width),
            SF::Solo => b(&p.solo),
            SF::SoloLed => b(&p.solo),
            SF::SoloSafe => b(&p.solo_safe),
            SF::Mon => item(def, p.monitor_bus.load(Relaxed)),
            SF::EffMute => {
                let c = &self.engine.controls;
                let group = p.mute_group_mask.load(Relaxed) & c.mute_groups.load(Relaxed) != 0;
                let dca = c.dca_mute_groups.load(Relaxed)
                    && c.dcas.iter().enumerate().any(|(i, d)| p.dca_mask.load(Relaxed) & (1 << i) != 0 && d.mute.load(Relaxed));
                Value::Int(if p.mute.load(Relaxed) {
                    1
                } else if group || dca {
                    2
                } else {
                    0
                })
            }
            SF::Proc => item(def, p.process_order.load(Relaxed)),
            SF::Tap => item(def, p.tap.load(Relaxed)),
            SF::Trim => f(&p.input.trim_db),
            SF::Bal => f(&p.input.balance_db),
            SF::Inv => b(&p.input.invert),
            SF::DlyOn => b(&p.input.delay_on),
            SF::DlyMode => item(def, p.input.delay_mode.load(Relaxed)),
            SF::Dly => f(&p.input.delay),
            SF::AltSel => b(&p.alt_selected),
            SF::Mode => Value::str(if p.stereo.load(Relaxed) { "ST" } else { "M" }),
            SF::Conn { alt, number } => {
                let (g, k) = self.conn_pair(row, p, alt);
                if number {
                    Value::Int(k)
                } else {
                    Value::str(g)
                }
            }
            SF::LowCut => b(&p.filter.low_cut),
            SF::LowCutF => f(&p.filter.low_cut_freq),
            SF::LowCutS => item(def, p.filter.low_cut_slope.load(Relaxed)),
            SF::HighCut => b(&p.filter.high_cut),
            SF::HighCutF => f(&p.filter.high_cut_freq),
            SF::HighCutS => item(def, p.filter.high_cut_slope.load(Relaxed)),
            SF::Tool => b(&p.filter.tool),
            SF::Slot(_) => return None,
            SF::BlkOn(blk) => match blk {
                Blk::Eq => b(&p.eq.on),
                Blk::Tool => b(&p.filter.tool),
                _ => b(&dyn_slot(p, blk).on),
            },
            SF::BlkMdl(blk) => Value::str(block_spec(p, kind, blk).0.token),
            SF::BlkMix(blk) => match blk {
                Blk::Eq => f(&p.eq.mix),
                _ => f(&dyn_slot(p, blk).mix),
            },
            SF::BlkGain(blk) => f(&dyn_slot(p, blk).gain),
            SF::Sc(blk, sc) => {
                let d = dyn_slot(p, blk);
                match sc {
                    ScF::Type => item(def, d.sc_filter.load(Relaxed)),
                    ScF::F => f(&d.sc_freq),
                    ScF::Q => f(&d.sc_q),
                    ScF::Src => {
                        let src = d.sc_source.load(Relaxed);
                        if src >= 0 && kind == StripKind::Channel {
                            value::coerce(def, &Value::Str(format!("CH.{}", src + 1))).unwrap_or(Value::str("SELF"))
                        } else {
                            Value::str("SELF")
                        }
                    }
                    ScF::Tap => item(def, d.sc_tap.load(Relaxed)),
                }
            }
            SF::Xo(xo) => {
                let d = &p.dynamics;
                match xo {
                    XoF::Depth => f(&d.xo_depth),
                    XoF::Type => item(def, d.xo_kind.load(Relaxed)),
                    XoF::F => f(&d.xo_freq),
                    XoF::Q => f(&d.xo_q),
                }
            }
            SF::Ins(post, insf) => {
                let ins: &InsertParams = if post { &p.post_insert } else { &p.pre_insert };
                match insf {
                    InsF::On => b(&ins.on),
                    InsF::Fx => fx_item(ins.fx.load(Relaxed)),
                    InsF::Mode => item(def, ins.mode.load(Relaxed)),
                    InsF::W => f(&ins.weight),
                }
            }
            SF::Main(i, mf) => {
                let m = p.mains.get(i as usize)?;
                match mf {
                    MainF::On => b(&m.on),
                    MainF::Lvl => lvl(&m.level_db),
                    MainF::Pre => b(&m.pre),
                }
            }
            SF::Send(slot, sf) => match s.sends.find(slot as usize) {
                Some(send) => match sf {
                    SendF::On => b(&send.enabled),
                    SendF::Lvl => lvl(&send.level_db),
                    SendF::Pon => b(&send.ignore_mute),
                    SendF::Mode => item(def, send.mode.load(Relaxed)),
                    SendF::Plink => Value::Int(match (send.pan_link.load(Relaxed), send.pan_reversed.load(Relaxed)) {
                        (false, _) => 0,
                        (true, false) => 1,
                        (true, true) => 2,
                    }),
                    SendF::Pan => f(&send.pan),
                    SendF::Pre => Value::bool(send.mode.load(Relaxed) == send_mode::PRE),
                },
                // Never configured: the engine's defaults.
                None => match sf {
                    SendF::On | SendF::Pon => Value::Int(0),
                    SendF::Lvl => Value::Float(0.0),
                    SendF::Mode => item(def, 0),
                    SendF::Plink => Value::Int(1),
                    SendF::Pan => Value::Float(0.0),
                    SendF::Pre => Value::Int(1),
                },
            },
            SF::Mtx(i, sf) => {
                let m = p.matrix.get(i as usize)?;
                match sf {
                    SendF::On => b(&m.on),
                    SendF::Lvl => lvl(&m.level_db),
                    SendF::Pon => b(&m.ignore_mute),
                    SendF::Mode => Value::str(if m.pre.load(Relaxed) { "PRE" } else { "POST" }),
                    SendF::Pan => f(&m.pan),
                    SendF::Pre => b(&m.pre),
                    SendF::Plink => return None,
                }
            }
            SF::Tags => {
                let extra = match self.state.shadow_get(ShadowKey::Row(row)) {
                    Some(Value::Str(t)) => parse_tags(&t).2,
                    _ => String::new(),
                };
                Value::Str(compose_tags(p.dca_mask.load(Relaxed), p.mute_group_mask.load(Relaxed), &extra))
            }
            SF::BusMono => b(&p.mono),
            SF::DirOn => b(&p.direct_on),
            SF::DirLvl => lvl(&p.direct_level_db),
            SF::DirInv => b(&p.direct_invert),
            SF::TapWidth => f(&p.tap_width),
            SF::TapEqOn => b(&p.tap_eq.on),
            SF::TapEq(band, tf) => {
                let b = p.tap_eq.bands.get(band as usize)?;
                match tf {
                    TapF::Gain => f(&b.gain_db),
                    TapF::Freq => f(&b.freq),
                    TapF::Q => f(&b.q),
                }
            }
        })
    }

    /// SOURCE SOLO is on: `$srcsolo`, or the host's own SOLO (as the engine sees it).
    fn source_solo_on(&self) -> bool {
        let m = &self.engine.controls.monitor;
        m.host().is_some_and(|(kind, i)| {
            m.source_solo_on.load(Relaxed) || self.strip(kind, i as u16 + 1).is_some_and(|s| s.params.solo.load(Relaxed))
        })
    }

    /// Sets a parameter. The value is coerced to the parameter's type and range
    /// first; returns the value now in effect.
    pub fn set(&self, node: Node, v: &Value) -> Result<Value, SetError> {
        let def = self.def(node).ok_or(SetError::NotFound)?;
        if !def.is_leaf() {
            return Err(SetError::IsNode);
        }
        if def.read_only {
            return Err(SetError::ReadOnly);
        }
        let cv = match (node, self.binding(node)) {
            // The delay's range depends on its unit (meters, feet, ms, samples).
            (_, Binding::Strip(_, _, SF::Dly)) => Value::Float(v.as_f32().ok_or(SetError::BadValue)?.clamp(0.0, 1000.0)),
            _ => value::coerce(def, v).ok_or(SetError::BadValue)?,
        };
        let applied = match node {
            Node::Root => return Err(SetError::NotFound),
            Node::Row(r) => self.set_row(r, def, &cv)?,
            Node::Param { slot, index } => match self.engine_param(slot, index) {
                Some((values, spec, i)) => {
                    let x = param_from_wing(&spec.params[i], def, &cv).ok_or(SetError::BadValue)?;
                    values[i].store(x);
                    true
                }
                None => false,
            },
        };
        if !applied {
            if let Some(k) = self.shadow_key(node) {
                self.state.shadow_set(k, cv.clone());
            }
            return Ok(cv);
        }
        Ok(self.get(node).unwrap_or(cv))
    }

    /// Toggles a 0/1 parameter (or steps through an enum/int range).
    pub fn toggle(&self, node: Node) -> Result<Value, SetError> {
        let def = self.def(node).ok_or(SetError::NotFound)?;
        let cur = self.get(node).ok_or(SetError::IsNode)?;
        let next = match def.ty {
            NodeType::Int => {
                let i = cur.as_i32().unwrap_or(0);
                Value::Int(if i >= def.max_int { def.min_int } else { i + 1 })
            }
            NodeType::StrEnum | NodeType::FloatEnum => {
                let n = def.items().len().max(1);
                let i = value::enum_index(def, &cur).unwrap_or(0);
                value::item_value(def, (i + 1) % n).ok_or(SetError::BadValue)?
            }
            _ => return Err(SetError::BadValue),
        };
        self.set(node, &next)
    }

    /// Moves a parameter by `amount` steps.
    pub fn step(&self, node: Node, amount: i32) -> Result<Value, SetError> {
        let def = self.def(node).ok_or(SetError::NotFound)?;
        let cur = self.get(node).ok_or(SetError::IsNode)?;
        let next = match def.ty {
            NodeType::Int => Value::Int(cur.as_i32().unwrap_or(0) + amount),
            NodeType::StrEnum | NodeType::FloatEnum => {
                let n = def.items().len() as i32;
                let i = value::enum_index(def, &cur).unwrap_or(0) as i32;
                value::item_value(def, (i + amount).clamp(0, (n - 1).max(0)) as usize).ok_or(SetError::BadValue)?
            }
            NodeType::Fader | NodeType::LinFloat | NodeType::LogFloat => {
                let raw = value::to_raw(def, &cur);
                let steps = if def.steps > 1 { def.steps - 1 } else { 1023 } as f32;
                value::from_raw(def, raw + amount as f32 / steps).ok_or(SetError::BadValue)?
            }
            _ => return Err(SetError::BadValue),
        };
        self.set(node, &next)
    }

    /// Applies a coerced value to a static row; false if it isn't mapped.
    fn set_row(&self, row: u16, def: &Def, v: &Value) -> Result<bool, SetError> {
        let e = self.engine;
        let c = &e.controls;
        match bindings()[row as usize] {
            Binding::None | Binding::FxSlot(_) => return Ok(false),
            Binding::Strip(kind, n, field) => return self.set_strip(row, def, kind, n, field, v),
            Binding::Dca(i, field) => {
                let d = c.dcas.get(i as usize).ok_or(SetError::NotFound)?;
                match field {
                    DcaF::Name => e.set_dca_name(i as usize, &str_of(v)),
                    DcaF::Colour => d.colour.store(v.as_i32().unwrap_or(1), Relaxed),
                    DcaF::Mute => set_b(&d.mute, v),
                    DcaF::Fader => set_lvl(&d.fader_db, v),
                    DcaF::Solo => set_b(&d.solo, v),
                    DcaF::SoloLed => return Err(SetError::ReadOnly),
                }
            }
            Binding::MuteGroup(i, true) => {
                let bit = 1u32 << i;
                if v.as_bool() {
                    c.mute_groups.fetch_or(bit, Relaxed);
                } else {
                    c.mute_groups.fetch_and(!bit, Relaxed);
                }
            }
            Binding::MuteGroup(i, false) => e.set_mute_group_name(i as usize, &str_of(v)),
            Binding::FxModel(i) => self.set_fx_model(i as usize, v)?,
            Binding::FxMix(i) => set_f(&c.fx.get(i as usize).ok_or(SetError::NotFound)?.mix, v),
            Binding::Ctl(ctl) => match ctl {
                CtlF::MainLink => set_item(&c.main_link, def, v),
                CtlF::DcaMuteGroups => set_b(&c.dca_mute_groups, v),
                CtlF::SoloAfl(kind) => {
                    let afl = value::enum_index(def, v) == Some(1);
                    match kind {
                        StripKind::Channel | StripKind::Aux => c.solo_afl.store(afl, Relaxed),
                        StripKind::Bus => c.bus_solo_afl.store(afl, Relaxed),
                        StripKind::Main => c.main_solo_afl.store(afl, Relaxed),
                        StripKind::Matrix => c.matrix_solo_afl.store(afl, Relaxed),
                    }
                }
                CtlF::MonitorLevel => set_lvl(&c.monitor_level_db, v),
                CtlF::MonitorDimDb => set_f(&c.monitor_dim_db, v),
                CtlF::MonitorDim => set_b(&c.monitor_dim, v),
                CtlF::MonitorMono => set_b(&c.monitor_mono, v),
                CtlF::MonitorSwap => set_b(&c.monitor_swap, v),
                CtlF::AutomixX => set_b(&c.automix_enable[0], v),
                CtlF::AutomixY => set_b(&c.automix_enable[1], v),
                CtlF::GlobalAlt => set_b(&c.global_alt, v),
                CtlF::InputSelectGlobal => set_b(&c.input_select_global, v),
                CtlF::StartupMainMute => set_b(&c.startup_main_mute, v),
                CtlF::AnySolo | CtlF::SipActive => return Err(SetError::ReadOnly),
                CtlF::SoloMode => set_item(&c.solo_mode, def, v),
                CtlF::SoloBus => {
                    set_item(&c.solo_bus, def, v);
                    // As the MONITORS keys: in LIVE while soloing, the pick overrides the strips'.
                    if c.solo_mode.load(Relaxed) == solo_mode::LIVE && c.any_solo_active.load(Relaxed) {
                        c.solo_bus_override.store(true, Relaxed);
                    }
                }
                CtlF::MonitorMute => set_b(&c.monitor_mute, v),
                CtlF::MuteOverride => set_b(&c.mute_override, v),
                CtlF::SourceSolo => set_item(&c.monitor.source_solo, def, v),
                CtlF::SourceSoloOn => {
                    let m = &c.monitor;
                    m.source_solo_on.store(v.as_bool(), Relaxed);
                    if !v.as_bool() {
                        // The host's SOLO is the other way in: release it too.
                        if let Some(s) = m.host().and_then(|(kind, i)| self.strip(kind, i as u16 + 1)) {
                            s.params.solo.store(false, Relaxed);
                        }
                    }
                }
                CtlF::SourceSoloGroup | CtlF::SourceSoloIn => {
                    let m = &c.monitor;
                    let (cur_g, cur_n) =
                        port::source_to_wing(m.source_solo_port.load(Relaxed)).filter(|(g, _)| *g != "OFF").unwrap_or(("A", 1));
                    let k = v.as_i32().unwrap_or(1);
                    let (g, n) = if ctl == CtlF::SourceSoloIn {
                        (cur_g, k)
                    } else {
                        (port::IN_GROUPS.get((k - 1).max(0) as usize).copied().unwrap_or("A"), cur_n)
                    };
                    match port::wing_to_source(g, n).filter(|p| *p >= 0) {
                        Some(p) => m.source_solo_port.store(p, Relaxed),
                        None => return Ok(false), // PLAY, OSC: no Live DAW source
                    }
                }
            },
            Binding::Talk(f) => {
                let t = &c.talk;
                match f {
                    TalkF::Assign => match value::enum_index(def, v) {
                        Some(1) => {
                            t.assign_index.store(39, Relaxed);
                            t.assign_kind.store(StripKind::Channel as i32, Relaxed);
                        }
                        Some(2) => {
                            t.assign_index.store(7, Relaxed);
                            t.assign_kind.store(StripKind::Aux as i32, Relaxed);
                        }
                        _ => t.assign_kind.store(TALK_OFF, Relaxed),
                    },
                    TalkF::Level => set_lvl(&t.level_db, v),
                }
            }
            Binding::TalkPath(i, f) => {
                let p = c.talk.paths.get(i as usize).ok_or(SetError::NotFound)?;
                let set_bit = |mask: &std::sync::atomic::AtomicU32, k: u8| {
                    if v.as_bool() {
                        mask.fetch_or(1 << k, Relaxed);
                    } else {
                        mask.fetch_and(!(1 << k), Relaxed);
                    }
                };
                match f {
                    TalkPathF::On => set_b(&p.on, v),
                    TalkPathF::Mode => set_item(&p.mode, def, v),
                    TalkPathF::MonDim => set_f(&p.mon_dim_db, v),
                    TalkPathF::BusDim => set_f(&p.bus_dim_db, v),
                    TalkPathF::Individual => set_b(&p.individual, v),
                    TalkPathF::Bus(k) => set_bit(&p.buses, k),
                    TalkPathF::Mtx(k) => set_bit(&p.matrices, k),
                    TalkPathF::Main(k) => set_bit(&p.mains, k),
                }
            }
            Binding::Mon(path, mf) => {
                let d = mon_dsp(c, path);
                let src = mon_source(c, path);
                let band = |k: u8| d.eq.get(k as usize).ok_or(SetError::NotFound);
                match mf {
                    MonF::Level => set_lvl(if path == 0 { &c.monitor_level_db } else { &c.phones.level_db }, v),
                    MonF::Dim => set_f(if path == 0 { &c.monitor_dim_db } else { &c.phones.dim_db }, v),
                    MonF::Inv => set_b(&d.invert, v),
                    MonF::Pan => set_f(&d.pan, v),
                    MonF::Width => set_f(&d.width, v),
                    MonF::EqOn => set_b(&d.eq_on, v),
                    MonF::EqGain(k) => set_f(&band(k)?.gain_db, v),
                    MonF::EqFreq(k) => set_f(&band(k)?.freq, v),
                    MonF::EqQ(k) => set_f(&band(k)?.q, v),
                    MonF::Limiter => set_f(&d.limiter_db, v),
                    MonF::DelayOn => set_b(&d.delay_on, v),
                    MonF::DelayM => set_f(&d.delay_m, v),
                    MonF::PflDim => set_f(&src.pfl_dim_db, v),
                    MonF::BandSolo => set_f(&d.band_solo_db, v),
                    MonF::SrcLevel => set_lvl(&src.source_level_db, v),
                    MonF::SrcMix => set_lvl(&src.source_mix_db, v),
                    MonF::Src => {
                        let i = value::enum_index(def, v).ok_or(SetError::BadValue)? as i32;
                        src.source.store(i - 1, Relaxed);
                    }
                    MonF::DirectIn => {
                        let i = value::enum_index(def, v).ok_or(SetError::BadValue)? as i32;
                        d.direct_source.store(i - 1, Relaxed);
                    }
                }
            }
            Binding::Rta(eq, rf) => {
                let a = e.analysis().controls();
                let rv = if eq { &a.eq } else { &a.home };
                match rf {
                    RtaF::Decay => set_item(&rv.decay, def, v),
                    RtaF::Detector => set_item(&rv.detector, def, v),
                    RtaF::Range => set_item(&rv.range, def, v),
                    RtaF::Gain => set_f(&rv.gain_db, v),
                    RtaF::AutoGain => set_b(&rv.autogain, v),
                }
            }
            Binding::Source(group, n, sf) => {
                let index = source_of(group, n).ok_or(SetError::NotFound)?;
                let table = e.sources();
                let p = table.get(index).ok_or(SetError::NotFound)?;
                match sf {
                    SrcF::Gain => set_f(&p.gain_db, v),
                    SrcF::Phantom => set_b(&p.phantom, v),
                    SrcF::Mute => set_b(&p.mute, v),
                    SrcF::Pol => set_b(&p.invert, v),
                    SrcF::Colour => p.colour.store(v.as_i32().unwrap_or(1).clamp(1, 18), Relaxed),
                    SrcF::Name => table.set_name(index, &str_of(v)),
                    SrcF::Icon => p.icon.store(v.as_i32().unwrap_or(0), Relaxed),
                    SrcF::Tags => {
                        // #M1..#M8 are the source's mute groups; the rest is kept as set.
                        let (dca, mgrp, extra) = parse_tags(&str_of(v));
                        p.mute_groups.store(mgrp, Relaxed);
                        // Sources have no DCAs here: their #D tags are kept with the rest.
                        self.state.shadow_set(ShadowKey::Row(row), Value::Str(compose_tags(dca, 0, &extra)));
                    }
                    SrcF::MuteState => return Err(SetError::ReadOnly),
                }
            }
            Binding::Info(InfoF::Name) => {
                self.state.identity.lock().unwrap_or_else(|e| e.into_inner()).name = str_of(v);
            }
            Binding::Info(_) => return Err(SetError::ReadOnly),
            Binding::Out(block, n, number) => {
                let port = port::output_port(block as usize, n as i32);
                let (cur_g, cur_n) = self
                    .output_source(port)
                    .map(|(g, k)| (g.to_owned(), k))
                    .or_else(|| self.shadow_pair(row, "grp", "in"))
                    .unwrap_or(("OFF".into(), 1));
                let (g, k) = if number { (cur_g, v.as_i32().unwrap_or(1)) } else { (str_of(v), cur_n) };
                self.route_output(port, &g, k);
                // Remember the pair as set: the number of an unrouted output, and
                // sources Live DAW can't route.
                for (name, val) in [("grp", Value::Str(g)), ("in", Value::Int(k))] {
                    if let Some(r) = self.sibling(row, name) {
                        self.state.shadow_set(ShadowKey::Row(r), val);
                    }
                }
            }
            Binding::SourceMode(group, n) => {
                self.state.shadow_set(ShadowKey::Row(row), v.clone());
                if let Some(index) = source_of(group, n) {
                    let mode = match value::enum_index(def, v) {
                        Some(1) => source_mode::STEREO,
                        Some(2) => source_mode::MID_SIDE,
                        _ => source_mode::MONO,
                    };
                    e.sources().set_mode(index, mode);
                }
                if let Some(src) = port::wing_to_source(group, n as i32).filter(|s| *s >= 0) {
                    let stereo = !matches!(v, Value::Str(m) if m == "M");
                    let rows = conn_rows();
                    let strips = self
                        .channels
                        .iter()
                        .enumerate()
                        .map(|(i, s)| (StripKind::Channel, i, s))
                        .chain(self.aux.iter().enumerate().map(|(i, s)| (StripKind::Aux, i, s)));
                    for (kind, i, s) in strips {
                        let p = &s.params;
                        // Strips patched to this source as the WING shows it (LCL 5 and
                        // A 5 share a port here but are different sources there).
                        let shown = rows.get(&(kind, i as u16 + 1)).map(|&r| self.conn_pair(r, p, false));
                        if p.input_left.load(Relaxed) == src && shown.is_none_or(|gk| gk == (group, n as i32)) {
                            p.stereo.store(stereo, Relaxed);
                            p.input_right.store(if stereo { src + 1 } else { -1 }, Relaxed);
                        }
                        if p.alt_input_left.load(Relaxed) == src {
                            p.alt_input_right.store(if stereo { src + 1 } else { -1 }, Relaxed);
                        }
                    }
                }
            }
        }
        Ok(true)
    }

    fn shadow_pair(&self, row: u16, gname: &str, nname: &str) -> Option<(String, i32)> {
        let g = self.sibling(row, gname).and_then(|r| self.state.shadow_get(ShadowKey::Row(r)))?;
        let n = self.sibling(row, nname).and_then(|r| self.state.shadow_get(ShadowKey::Row(r)))?;
        Some((str_of(&g), n.as_i32().unwrap_or(1)))
    }

    /// Routes `group`/`n` (BUS, MAIN, MTX: 2k-1 = left of strip k, 2k = right;
    /// MON 1/2) to hardware output `port`. False if Live DAW can't.
    fn route_output(&self, port: i32, group: &str, n: i32) -> bool {
        let c = &self.engine.controls;
        let target: Option<(&Arc<Strip>, bool)> = match group {
            "BUS" => self.buses.get(((n - 1) / 2) as usize).and_then(|b| b.as_ref()).map(|s| (s, n % 2 == 0)),
            "MAIN" => self.mains.get(((n - 1) / 2) as usize).map(|s| (s, n % 2 == 0)),
            "MTX" => self.matrices.get(((n - 1) / 2) as usize).map(|s| (s, n % 2 == 0)),
            _ => None,
        };
        let known = target.is_some() || group == "OFF" || (group == "MON" && (1..=2).contains(&n));
        if !known {
            return false;
        }
        // One source per output, as on the console.
        for s in self.buses.iter().flatten().chain(&self.mains).chain(&self.matrices) {
            for o in [&s.params.output_left, &s.params.output_right] {
                if o.load(Relaxed) == port {
                    o.store(-1, Relaxed);
                }
            }
        }
        for o in [&c.monitor_output_left, &c.monitor_output_right] {
            if o.load(Relaxed) == port {
                o.store(-1, Relaxed);
            }
        }
        match (group, target) {
            (_, Some((s, right))) => {
                if right { &s.params.output_right } else { &s.params.output_left }.store(port, Relaxed);
            }
            ("MON", None) => if n == 2 { &c.monitor_output_right } else { &c.monitor_output_left }.store(port, Relaxed),
            _ => {}
        }
        true
    }

    fn set_fx_model(&self, i: usize, v: &Value) -> Result<(), SetError> {
        let c = &self.engine.controls;
        let p = c.fx.get(i).ok_or(SetError::NotFound)?;
        let token = str_of(v);
        let model = model_index(FX_MODELS, &token).ok_or(SetError::BadValue)?;
        let current = p.model_index();
        if model == current {
            return Ok(());
        }
        // EXT without a plugin is an empty plugin slot (plugins are loaded on
        // Live DAW's EFFECTS screen); a loaded plugin isn't replaced remotely.
        if current == FX_EXT && self.engine.fx(i).is_some_and(|r| r.is_loaded()) {
            return Err(SetError::Unsupported(format!("FX {} holds a plugin", i + 1)));
        }
        if !allowed_in_slot(model, i) {
            return Err(SetError::Unsupported(format!("{} needs a premium slot (FX 1-8)", FX_MODELS[model].name)));
        }
        // The message thread builds the device (ld_engine_collect).
        p.set_model(model, true);
        Ok(())
    }

    #[allow(clippy::too_many_arguments)]
    fn set_strip(&self, row: u16, def: &Def, kind: StripKind, n: u16, field: SF, v: &Value) -> Result<bool, SetError> {
        let Some(s) = self.strip(kind, n) else { return Ok(false) };
        let p = &s.params;
        match field {
            SF::Name => s.set_name(&str_of(v)),
            SF::Colour => p.colour.store(v.as_i32().unwrap_or(1), Relaxed),
            SF::Icon => p.icon.store(v.as_i32().unwrap_or(0), Relaxed),
            SF::Led => set_b(&p.led, v),
            SF::Mute => set_b(&p.mute, v),
            SF::Fader => set_lvl(&p.fader_db, v),
            SF::Pan => set_f(&p.pan, v),
            SF::Width => set_f(&p.width, v),
            SF::Solo => set_b(&p.solo, v),
            SF::SoloSafe => set_b(&p.solo_safe, v),
            SF::Mon => set_item(&p.monitor_bus, def, v),
            SF::Proc => set_item(&p.process_order, def, v),
            SF::Tap => set_item(&p.tap, def, v),
            SF::Trim => set_f(&p.input.trim_db, v),
            SF::Bal => set_f(&p.input.balance_db, v),
            SF::Inv => set_b(&p.input.invert, v),
            SF::DlyOn => set_b(&p.input.delay_on, v),
            SF::DlyMode => set_item(&p.input.delay_mode, def, v),
            SF::Dly => set_f(&p.input.delay, v),
            SF::AltSel => set_b(&p.alt_selected, v),
            SF::Conn { alt, number } => {
                let (left, right) = if alt { (&p.alt_input_left, &p.alt_input_right) } else { (&p.input_left, &p.input_right) };
                let gname = if alt { "altgrp" } else { "grp" };
                let nname = if alt { "altin" } else { "in" };
                let cur = left.load(Relaxed);
                let (cur_g, cur_n) = match port::source_to_wing(cur) {
                    Some((g, k)) if cur >= 0 => (g.to_owned(), k),
                    _ => self.shadow_pair(row, gname, nname).unwrap_or(("OFF".into(), 1)),
                };
                let (g, k) = if number { (cur_g, v.as_i32().unwrap_or(1)) } else { (str_of(v), cur_n) };
                match port::wing_to_source(&g, k) {
                    Some(src) => {
                        left.store(src, Relaxed);
                        let stereo = p.stereo.load(Relaxed);
                        right.store(if stereo && src >= 0 { src + 1 } else { -1 }, Relaxed);
                    }
                    None => left.store(-1, Relaxed),
                }
                // Keep the pair as set, so an unmappable patch reads back.
                for (name, val) in [(gname, Value::Str(g)), (nname, Value::Int(k))] {
                    if let Some(r) = self.sibling(row, name) {
                        self.state.shadow_set(ShadowKey::Row(r), val);
                    }
                }
            }
            SF::LowCut => set_b(&p.filter.low_cut, v),
            SF::LowCutF => set_f(&p.filter.low_cut_freq, v),
            SF::LowCutS => set_item(&p.filter.low_cut_slope, def, v),
            SF::HighCut => set_b(&p.filter.high_cut, v),
            SF::HighCutF => set_f(&p.filter.high_cut_freq, v),
            SF::HighCutS => set_item(&p.filter.high_cut_slope, def, v),
            SF::Tool => set_b(&p.filter.tool, v),
            SF::BlkOn(blk) => match blk {
                Blk::Eq => set_b(&p.eq.on, v),
                Blk::Tool => set_b(&p.filter.tool, v),
                _ => set_b(&dyn_slot(p, blk).on, v),
            },
            SF::BlkMdl(blk) => {
                let (current, table) = block_spec(p, kind, blk);
                let token = str_of(v);
                let m = model_index(table, &token).ok_or(SetError::BadValue)?;
                if table[m].token != current.token {
                    match blk {
                        Blk::Gate | Blk::Dyn => dyn_slot(p, blk).set_model(m),
                        Blk::Eq => p.eq.set_model(m, kind.is_output()),
                        Blk::Tool => p.filter.set_tool_model(m),
                    }
                }
            }
            SF::BlkMix(blk) => match blk {
                Blk::Eq => set_f(&p.eq.mix, v),
                _ => set_f(&dyn_slot(p, blk).mix, v),
            },
            SF::BlkGain(blk) => set_f(&dyn_slot(p, blk).gain, v),
            SF::Sc(blk, sc) => {
                let d = dyn_slot(p, blk);
                match sc {
                    ScF::Type => set_item(&d.sc_filter, def, v),
                    ScF::F => set_f(&d.sc_freq, v),
                    ScF::Q => set_f(&d.sc_q, v),
                    ScF::Src => {
                        let text = str_of(v);
                        if text == "SELF" {
                            d.sc_source.store(-1, Relaxed);
                        } else if let (StripKind::Channel, Some(k)) =
                            (kind, text.strip_prefix("CH.").and_then(|k| k.parse::<i32>().ok()))
                        {
                            d.sc_source.store(k - 1, Relaxed);
                        } else {
                            return Ok(false); // keyed from a bus/main/matrix: not in the engine
                        }
                    }
                    ScF::Tap => set_item(&d.sc_tap, def, v),
                }
            }
            SF::Xo(xo) => {
                let d = &p.dynamics;
                match xo {
                    XoF::Depth => set_f(&d.xo_depth, v),
                    XoF::Type => set_item(&d.xo_kind, def, v),
                    XoF::F => set_f(&d.xo_freq, v),
                    XoF::Q => set_f(&d.xo_q, v),
                }
            }
            SF::Ins(post, insf) => {
                let ins: &InsertParams = if post { &p.post_insert } else { &p.pre_insert };
                match insf {
                    InsF::On => set_b(&ins.on, v),
                    InsF::Fx => ins.fx.store(fx_from_item(v).ok_or(SetError::BadValue)?, Relaxed),
                    InsF::Mode => set_item(&ins.mode, def, v),
                    InsF::W => set_f(&ins.weight, v),
                }
            }
            SF::Main(i, mf) => {
                let m = p.mains.get(i as usize).ok_or(SetError::NotFound)?;
                match mf {
                    MainF::On => set_b(&m.on, v),
                    MainF::Lvl => set_lvl(&m.level_db, v),
                    MainF::Pre => set_b(&m.pre, v),
                }
            }
            SF::Send(slot, sf) => {
                let send = s.sends.get_or_create(slot as usize).ok_or(SetError::NotFound)?;
                match sf {
                    SendF::On => set_b(&send.enabled, v),
                    SendF::Lvl => set_lvl(&send.level_db, v),
                    SendF::Pon => set_b(&send.ignore_mute, v),
                    SendF::Mode => set_item(&send.mode, def, v),
                    SendF::Plink => {
                        let k = v.as_i32().unwrap_or(0);
                        send.pan_link.store(k != 0, Relaxed);
                        send.pan_reversed.store(k == 2, Relaxed);
                    }
                    SendF::Pan => set_f(&send.pan, v),
                    SendF::Pre => send.mode.store(if v.as_bool() { send_mode::PRE } else { send_mode::POST }, Relaxed),
                }
            }
            SF::Mtx(i, sf) => {
                let m = p.matrix.get(i as usize).ok_or(SetError::NotFound)?;
                match sf {
                    SendF::On => set_b(&m.on, v),
                    SendF::Lvl => set_lvl(&m.level_db, v),
                    SendF::Pon => set_b(&m.ignore_mute, v),
                    SendF::Mode => {
                        let mode = str_of(v);
                        m.pre.store(mode == "PRE", Relaxed);
                        if mode == "GRP" {
                            return Err(SetError::Unsupported("GROUP matrix sends".into()));
                        }
                    }
                    SendF::Pan => set_f(&m.pan, v),
                    SendF::Pre => set_b(&m.pre, v),
                    SendF::Plink => return Ok(false),
                }
            }
            SF::Tags => {
                let (dca, mgrp, extra) = parse_tags(&str_of(v));
                p.dca_mask.store(dca, Relaxed);
                p.mute_group_mask.store(mgrp, Relaxed);
                self.state.shadow_set(ShadowKey::Row(row), Value::Str(extra));
            }
            SF::BusMono => set_b(&p.mono, v),
            SF::DirOn => set_b(&p.direct_on, v),
            SF::DirLvl => set_lvl(&p.direct_level_db, v),
            SF::DirInv => set_b(&p.direct_invert, v),
            SF::TapWidth => set_f(&p.tap_width, v),
            SF::TapEqOn => set_b(&p.tap_eq.on, v),
            SF::TapEq(band, tf) => {
                let b = p.tap_eq.bands.get(band as usize).ok_or(SetError::NotFound)?;
                match tf {
                    TapF::Gain => set_f(&b.gain_db, v),
                    TapF::Freq => set_f(&b.freq, v),
                    TapF::Q => set_f(&b.q, v),
                }
            }
            SF::ActName | SF::ActColour | SF::ActIcon | SF::SoloLed | SF::EffFader | SF::EffMute | SF::Mode | SF::Slot(_) => {
                return Err(SetError::ReadOnly)
            }
        }
        Ok(true)
    }

    /// Every leaf under `node` (depth first), with the active models' parameters.
    pub fn leaves(&self, node: Node, out: &mut Vec<Node>) {
        let s = schema();
        for c in s.children(node, self) {
            if s.is_leaf(c, self) {
                out.push(c);
            } else {
                self.leaves(c, out);
            }
        }
    }
}

fn str_of(v: &Value) -> String {
    match v {
        Value::Str(s) => s.clone(),
        Value::Int(i) => i.to_string(),
        Value::Float(f) => f.to_string(),
    }
}

/// A shadowed group of `row`'s (grp, in) pair, if Live DAW can't route it
/// (else the engine's state wins: a stale shadow must not show).
fn unroutable_group(row: u16, group: &str) -> Option<&'static str> {
    if matches!(group, "BUS" | "MAIN" | "MTX" | "MON" | "OFF") {
        return None;
    }
    let s = schema();
    let grp = match s.child(s.parent(Node::Row(row)), "grp", &NoModels)? {
        Node::Row(r) => r,
        _ => return None,
    };
    def_items_find(Some(grp), group)
}

/// `text` as an item of the enum of `row` (for showing a shadowed patch group).
fn def_items_find(row: Option<u16>, text: &str) -> Option<&'static str> {
    let s = schema();
    let def = s.def(Node::Row(row?), &NoModels)?;
    def.items().iter().find(|i| i.item == text).map(|i| i.item)
}

// ---- coverage ----------------------------------------------------------------------

/// What maps and what doesn't, counted over the whole tree (every model of
/// every slot), by subtree.
#[derive(Debug, Default, Clone)]
pub struct Coverage {
    /// subtree pattern ("/ch/N/gate/<model>") -> (mapped, total).
    pub by_group: BTreeMap<String, (usize, usize)>,
    pub mapped: usize,
    pub total: usize,
}

impl Coverage {
    pub fn report(&self) -> String {
        let mut out = format!(
            "WING tree coverage: {} of {} parameters map to Live DAW ({:.1} %)\n",
            self.mapped,
            self.total,
            100.0 * self.mapped as f64 / self.total.max(1) as f64
        );
        for (g, (m, t)) in &self.by_group {
            out.push_str(&format!("  {g:<34} {m:>6} / {t:<6}\n"));
        }
        out
    }
}

fn group_pattern(parts: &[std::borrow::Cow<'static, str>]) -> String {
    fn n(s: &str) -> &str {
        if s.parse::<u32>().is_ok() || (s.starts_with("MX") && s[2..].parse::<u32>().is_ok()) {
            "N"
        } else {
            s
        }
    }
    let take = match parts.first().map(|p| p.as_ref()) {
        Some("ch" | "aux" | "bus" | "main" | "mtx") => 3,
        Some("io") => 3,
        Some("$ctl") => 2,
        _ => 2,
    };
    let mut g = String::new();
    for p in parts.iter().take(take.min(parts.len().saturating_sub(1)).max(1)) {
        g.push('/');
        g.push_str(n(p));
    }
    g
}

/// Counts the leaves that bind to the engine. Strips are counted as if the
/// console had all of the WING's 40 channels.
pub fn coverage() -> Coverage {
    let s = schema();
    let b = bindings();
    let mut cov = Coverage::default();
    for (r, row) in s.rows.iter().enumerate() {
        let def = &s.defs[row.def as usize];
        if !def.is_leaf() {
            continue;
        }
        let parts = s.row_parts(r as u16);
        let e = cov.by_group.entry(group_pattern(&parts)).or_default();
        e.1 += 1;
        cov.total += 1;
        if b[r] != Binding::None {
            e.0 += 1;
            cov.mapped += 1;
        }
    }
    for slot in &s.slots {
        let parts = s.row_parts(slot.row);
        let tables: Option<&[ModelSpec]> = match b[slot.row as usize] {
            Binding::Strip(kind, _, SF::Slot(blk)) => Some(match blk {
                Blk::Gate | Blk::Dyn => DYN_MODELS,
                Blk::Eq if kind.is_output() => EQ_MODELS_BUS,
                Blk::Eq => EQ_MODELS_CH,
                Blk::Tool => FILTER_TOOLS,
            }),
            Binding::FxSlot(_) => Some(FX_MODELS),
            _ => None,
        };
        let mut key = group_pattern(&parts);
        key.push_str("/<models>");
        for m in &s.families[slot.family as usize].models {
            let spec = tables.and_then(|t| model_index(t, m.token).map(|i| &t[i]));
            for p in &m.params {
                let e = cov.by_group.entry(key.clone()).or_default();
                e.1 += 1;
                cov.total += 1;
                if spec.is_some_and(|sp| sp.param_index(p.name).is_some()) {
                    e.0 += 1;
                    cov.mapped += 1;
                }
            }
        }
    }
    cov
}

#[cfg(test)]
mod tests {
    use super::*;

    fn engine() -> Engine {
        let e = Engine::new(0);
        e.set_num_channels(40);
        e
    }

    fn node(c: &Console, path: &str) -> Node {
        schema().resolve(path, c).unwrap_or_else(|| panic!("{path}"))
    }

    #[test]
    fn strips_round_trip_per_kind() {
        let e = engine();
        let st = WingState::default();
        let c = Console::new(&e, &st);
        let cases: &[(&str, Value)] = &[
            ("/ch/3/fdr", Value::Float(-6.5)),
            ("/ch/3/mute", Value::Int(1)),
            ("/ch/3/pan", Value::Float(-40.0)),
            ("/ch/3/wid", Value::Float(80.0)),
            ("/ch/3/name", Value::str("Kick")),
            ("/ch/3/col", Value::Int(9)),
            ("/ch/3/icon", Value::Int(12)),
            ("/ch/3/in/set/trim", Value::Float(4.5)),
            ("/ch/3/in/set/inv", Value::Int(1)),
            ("/ch/3/flt/lc", Value::Int(1)),
            ("/ch/3/flt/lcf", Value::Float(120.0)),
            ("/ch/3/flt/lcs", Value::str("18")),
            ("/ch/3/gate/on", Value::Int(1)),
            ("/ch/3/eq/mdl", Value::str("SOUL")),
            ("/ch/3/dyn/mdl", Value::str("LA")),
            ("/ch/3/proc", Value::str("EGDI")),
            ("/ch/3/ptap", Value::str("POST")),
            ("/ch/3/preins/ins", Value::str("FX3")),
            ("/ch/3/postins/mode", Value::str("AUTO_X")),
            ("/ch/3/main/2/on", Value::Int(1)),
            ("/ch/3/main/2/lvl", Value::Float(-3.0)),
            ("/ch/3/send/5/on", Value::Int(1)),
            ("/ch/3/send/5/lvl", Value::Float(-12.0)),
            ("/ch/3/send/5/mode", Value::str("POST")),
            ("/ch/3/send/5/plink", Value::Int(2)),
            ("/ch/3/send/MX2/lvl", Value::Float(-2.0)),
            ("/ch/3/tags", Value::str("#D1,#M3,mytag")),
            ("/aux/2/fdr", Value::Float(1.5)),
            ("/aux/2/dyn/mdl", Value::str("COMP")),
            ("/bus/4/fdr", Value::Float(-10.0)),
            ("/bus/4/busmono", Value::Int(1)),
            ("/bus/4/eq/mdl", Value::str("PIA")),
            ("/bus/12/send/3/on", Value::Int(1)),
            ("/bus/12/send/3/pre", Value::Int(0)),
            ("/main/1/fdr", Value::Float(-4.0)),
            ("/main/1/dly/on", Value::Int(1)),
            ("/mtx/2/dir/lvl", Value::Float(-20.0)),
            ("/mtx/2/mute", Value::Int(1)),
            ("/dca/3/fdr", Value::Float(-8.0)),
            ("/dca/3/name", Value::str("Drums")),
            ("/mgrp/2/mute", Value::Int(1)),
            ("/mgrp/2/name", Value::str("Band")),
            ("/fx/2/mdl", Value::str("PLATE")),
            ("/fx/2/fxmix", Value::Float(60.0)),
            ("/cfg/mainlink", Value::str("2-3")),
            ("/cfg/solo/chtap", Value::str("AFL")),
            ("/io/altsw", Value::Int(1)),
        ];
        for (path, v) in cases {
            let n = node(&c, path);
            assert!(c.is_mapped(n), "{path} not mapped");
            let got = c.set(n, v).unwrap_or_else(|e| panic!("{path}: {e}"));
            assert_eq!(&got, v, "{path}");
            assert_eq!(c.get(n).as_ref(), Some(v), "{path}");
        }
        // The engine saw it.
        let ch3 = c.strip(StripKind::Channel, 3).unwrap();
        assert_eq!(ch3.params.fader_db.load(), -6.5);
        assert_eq!(ch3.params.dca_mask.load(Relaxed), 1);
        assert_eq!(ch3.params.mute_group_mask.load(Relaxed), 4);
        assert_eq!(ch3.name(), "Kick");
        assert!(ch3.sends.find(4).unwrap().pan_reversed.load(Relaxed));
        assert_eq!(e.controls.main_link.load(Relaxed), 2);
        assert_eq!(e.dca_name(2), "Drums");
        assert_eq!(e.controls.mute_groups.load(Relaxed), 2);
        let bus12 = c.strip(StripKind::Bus, 12).unwrap();
        assert_eq!(bus12.sends.find(2).unwrap().mode.load(Relaxed), send_mode::POST);
    }

    #[test]
    fn model_parameters_round_trip() {
        let e = engine();
        let st = WingState::default();
        let c = Console::new(&e, &st);
        c.set(node(&c, "/ch/1/gate/mdl"), &Value::str("GATE")).unwrap();
        let thr = node(&c, "/ch/1/gate/thr");
        assert_eq!(c.set(thr, &Value::Float(-33.0)).unwrap(), Value::Float(-33.0));
        assert_eq!(c.strip(StripKind::Channel, 1).unwrap().params.gate.param("thr").unwrap().load(), -33.0);
        // A choice parameter by item and by index.
        let ratio = node(&c, "/ch/1/gate/ratio");
        assert_eq!(c.set(ratio, &Value::str("1:2")).unwrap(), Value::str("1:2"));
        assert_eq!(c.set(ratio, &Value::Int(0)).unwrap(), Value::str("1:1.5"));
        // Changing the model changes the parameters behind the same ids.
        c.set(node(&c, "/ch/1/dyn/mdl"), &Value::str("COMP")).unwrap();
        let ratio = node(&c, "/ch/1/dyn/ratio");
        let v = c.get(ratio).unwrap();
        assert!(matches!(v, Value::Float(_)), "{v:?}");
        c.set(ratio, &Value::Float(4.0)).unwrap();
        assert_eq!(c.get(ratio), Some(Value::Float(4.0)));
        // EQ band and an FX parameter.
        c.set(node(&c, "/ch/2/eq/mdl"), &Value::str("STD")).unwrap();
        let g = node(&c, "/ch/2/eq/2g");
        c.set(g, &Value::Float(5.5)).unwrap();
        assert_eq!(c.get(g), Some(Value::Float(5.5)));
        c.set(node(&c, "/fx/1/mdl"), &Value::str("HALL")).unwrap();
        let dcy = node(&c, "/fx/1/dcy");
        c.set(dcy, &Value::Float(3.0)).unwrap();
        assert_eq!(c.get(dcy), Some(Value::Float(3.0)));
        assert_eq!(e.controls.fx[0].param("dcy").unwrap().load(), 3.0);
        // Premium devices only in slots 1-8.
        assert!(matches!(c.set(node(&c, "/fx/12/mdl"), &Value::str("HALL")), Err(SetError::Unsupported(_) | SetError::BadValue)));
    }

    #[test]
    fn levels_and_routing() {
        let e = engine();
        let st = WingState::default();
        let c = Console::new(&e, &st);
        let fdr = node(&c, "/ch/1/fdr");
        c.set(fdr, &Value::Float(-144.0)).unwrap();
        assert_eq!(c.get(fdr), Some(Value::Float(-144.0)));
        assert!(c.strip(StripKind::Channel, 1).unwrap().params.fader_db.load() <= MINUS_INF_DB);
        // Input patch: AES50 B 3 = port 51; the WING-LIVE card = playback.
        c.set(node(&c, "/ch/1/in/conn/grp"), &Value::str("B")).unwrap();
        c.set(node(&c, "/ch/1/in/conn/in"), &Value::Int(3)).unwrap();
        assert_eq!(c.strip(StripKind::Channel, 1).unwrap().params.input_left.load(Relaxed), 50);
        c.set(node(&c, "/ch/2/in/conn/grp"), &Value::str("CRD")).unwrap();
        assert_eq!(c.get(node(&c, "/ch/2/in/conn/grp")), Some(Value::str("CRD")));
        // An internal source isn't routable but reads back.
        c.set(node(&c, "/ch/4/in/conn/grp"), &Value::str("BUS")).unwrap();
        assert_eq!(c.get(node(&c, "/ch/4/in/conn/grp")), Some(Value::str("BUS")));
        assert_eq!(c.strip(StripKind::Channel, 4).unwrap().params.input_left.load(Relaxed), -1);
        // Outputs: A 7/8 = Main 2 L/R.
        // (The number first: each strip has one output pair here, so MAIN with
        // the default number 1 would move Main 1 there.)
        c.set(node(&c, "/io/out/A/7/in"), &Value::Int(3)).unwrap();
        c.set(node(&c, "/io/out/A/7/grp"), &Value::str("MAIN")).unwrap();
        c.set(node(&c, "/io/out/A/8/in"), &Value::Int(4)).unwrap();
        c.set(node(&c, "/io/out/A/8/grp"), &Value::str("MAIN")).unwrap();
        assert_eq!(c.get(node(&c, "/io/out/A/8/in")), Some(Value::Int(4)));
        let m2 = c.strip(StripKind::Main, 2).unwrap();
        assert_eq!((m2.params.output_left.load(Relaxed), m2.params.output_right.load(Relaxed)), (6, 7));
        assert_eq!(c.get(node(&c, "/io/out/A/1/grp")), Some(Value::str("MAIN"))); // Main 1 on ports 1/2
                                                                                  // Stereo sources.
        c.set(node(&c, "/ch/5/in/conn/grp"), &Value::str("A")).unwrap();
        c.set(node(&c, "/ch/5/in/conn/in"), &Value::Int(9)).unwrap();
        c.set(node(&c, "/io/in/A/9/mode"), &Value::str("ST")).unwrap();
        let ch5 = c.strip(StripKind::Channel, 5).unwrap();
        assert!(ch5.params.stereo.load(Relaxed));
        assert_eq!(ch5.params.input_right.load(Relaxed), 9);
        assert_eq!(c.get(node(&c, "/ch/5/in/set/$mode")), Some(Value::str("ST")));
    }

    #[test]
    fn unmapped_nodes_keep_their_values() {
        let e = engine();
        let st = WingState::default();
        let c = Console::new(&e, &st);
        let peq = node(&c, "/io/in/LCL/1/g"); // a LCL source shares its port with A 1 here
        assert!(!c.is_mapped(peq));
        assert_eq!(c.get(peq), Some(Value::Float(0.0)));
        c.set(peq, &Value::Float(3.0)).unwrap();
        assert_eq!(c.get(peq), Some(Value::Float(3.0)));
        assert_eq!(c.set(node(&c, "/ch/1/$sololed"), &Value::Int(1)), Err(SetError::ReadOnly));
        assert_eq!(c.set(node(&c, "/ch/1"), &Value::Int(1)), Err(SetError::IsNode));
        assert_eq!(c.set(node(&c, "/ch/1/mon"), &Value::str("nope")), Err(SetError::BadValue));
    }

    #[test]
    fn rta_settings() {
        let e = engine();
        let st = WingState::default();
        let c = Console::new(&e, &st);
        let a = e.analysis().controls();
        c.set(node(&c, "/cfg/rta/rtadecay"), &Value::str("SLOW")).unwrap();
        c.set(node(&c, "/cfg/rta/rtadet"), &Value::str("RMS")).unwrap();
        c.set(node(&c, "/cfg/rta/rtarange"), &Value::Float(30.0)).unwrap();
        c.set(node(&c, "/cfg/rta/eqgain"), &Value::Float(20.0)).unwrap();
        c.set(node(&c, "/cfg/rta/eqauto"), &Value::Int(0)).unwrap();
        assert_eq!((a.home.decay.load(Relaxed), a.home.detector.load(Relaxed), a.home.range.load(Relaxed)), (0, 1, 0));
        assert_eq!(a.eq.gain_db.load(), 20.0);
        assert!(!a.eq.autogain.load(Relaxed));
        a.eq.range.store(1, Relaxed);
        assert_eq!(c.get(node(&c, "/cfg/rta/eqrange")), Some(Value::Float(60.0)));
        assert!(!c.is_mapped(node(&c, "/cfg/rta/rtatap")), "the WING's taps aren't Live DAW's");
    }

    #[test]
    fn tap_width_and_tap_eq() {
        let e = engine();
        let st = WingState::default();
        let c = Console::new(&e, &st);
        let p = &c.strip(StripKind::Channel, 3).unwrap().params;
        c.set(node(&c, "/ch/3/tapwid"), &Value::Float(-50.0)).unwrap();
        c.set(node(&c, "/ch/3/peq/on"), &Value::Int(1)).unwrap();
        c.set(node(&c, "/ch/3/peq/2g"), &Value::Float(4.5)).unwrap();
        c.set(node(&c, "/ch/3/peq/3f"), &Value::Float(8000.0)).unwrap();
        c.set(node(&c, "/ch/3/peq/1q"), &Value::Float(2.0)).unwrap();
        assert_eq!(p.tap_width.load(), -50.0);
        assert!(p.tap_eq.on.load(Relaxed));
        assert_eq!(p.tap_eq.bands[1].gain_db.load(), 4.5);
        assert_eq!(p.tap_eq.bands[2].freq.load(), 8000.0);
        assert_eq!(p.tap_eq.bands[0].q.load(), 2.0);
        p.tap_eq.bands[0].gain_db.store(-3.0);
        assert_eq!(c.get(node(&c, "/ch/3/peq/1g")), Some(Value::Float(-3.0)));
        assert!(c.is_mapped(node(&c, "/ch/3/tapwid")));
    }

    #[test]
    fn solo_modes_override_and_talkback() {
        let e = engine();
        let st = WingState::default();
        let c = Console::new(&e, &st);
        let ctl = &e.controls;
        c.set(node(&c, "/cfg/solo/mode"), &Value::str("SIP")).unwrap();
        assert_eq!(ctl.solo_mode.load(Relaxed), solo_mode::SIP);
        c.set(node(&c, "/cfg/solo/mon"), &Value::str("SPK")).unwrap();
        assert_eq!(ctl.solo_bus.load(Relaxed), 1, "SPK = B, the SPEAKERS");
        assert_eq!(c.get(node(&c, "/cfg/solo/mon")), Some(Value::str("SPK")));
        c.set(node(&c, "/cfg/solo/mute"), &Value::Int(1)).unwrap();
        assert!(ctl.monitor_mute.load(Relaxed));
        c.set(node(&c, "/$ctl/cfg/muteovr"), &Value::Int(1)).unwrap();
        assert!(ctl.mute_override.load(Relaxed));
        assert_eq!(c.set(node(&c, "/$stat/sip"), &Value::Int(1)), Err(SetError::ReadOnly));

        c.set(node(&c, "/cfg/talk/assign"), &Value::str("AUX8")).unwrap();
        assert_eq!(ctl.talk.assignment(), Some((StripKind::Aux, 7)));
        c.set(node(&c, "/cfg/talk/lvl"), &Value::Float(-6.0)).unwrap();
        assert_eq!(ctl.talk.level_db.load(), -6.0);
        c.set(node(&c, "/cfg/talk/B/mode"), &Value::str("LATCH")).unwrap();
        c.set(node(&c, "/cfg/talk/B/mondim"), &Value::Float(15.0)).unwrap();
        c.set(node(&c, "/cfg/talk/B/indiv"), &Value::Int(1)).unwrap();
        c.set(node(&c, "/cfg/talk/B/B12"), &Value::Int(1)).unwrap();
        c.set(node(&c, "/cfg/talk/B/MX3"), &Value::Int(1)).unwrap();
        c.set(node(&c, "/cfg/talk/B/M4"), &Value::Int(1)).unwrap();
        c.set(node(&c, "/cfg/talk/A/$on"), &Value::Int(1)).unwrap();
        let b = &ctl.talk.paths[1];
        assert_eq!(b.mode.load(Relaxed), 2);
        assert_eq!(b.mon_dim_db.load(), 15.0);
        assert!(b.individual.load(Relaxed));
        assert_eq!((b.buses.load(Relaxed), b.matrices.load(Relaxed), b.mains.load(Relaxed)), (1 << 11, 1 << 2, 1 << 3));
        assert!(ctl.talk.paths[0].on.load(Relaxed));
        c.set(node(&c, "/cfg/talk/B/B12"), &Value::Int(0)).unwrap();
        assert_eq!(b.buses.load(Relaxed), 0);
        assert_eq!(c.get(node(&c, "/cfg/talk/B/MX3")), Some(Value::Int(1)));
        c.set(node(&c, "/cfg/talk/assign"), &Value::str("OFF")).unwrap();
        assert_eq!(ctl.talk.assignment(), None);
    }

    #[test]
    fn monitor_paths() {
        let e = engine();
        let st = WingState::default();
        let c = Console::new(&e, &st);
        let ctl = &e.controls;
        let (phones, speakers) = (&ctl.monitor.paths[1], &ctl.monitor.paths[0]);
        // /cfg/mon/1 is the PHONES.
        c.set(node(&c, "/cfg/mon/1/eq/on"), &Value::Int(1)).unwrap();
        c.set(node(&c, "/cfg/mon/1/eq/3g"), &Value::Float(5.0)).unwrap();
        c.set(node(&c, "/cfg/mon/1/eq/3q"), &Value::Float(3.0)).unwrap();
        c.set(node(&c, "/cfg/mon/1/eq/lsf"), &Value::Float(120.0)).unwrap();
        c.set(node(&c, "/cfg/mon/1/eq/hsg"), &Value::Float(-4.0)).unwrap();
        c.set(node(&c, "/cfg/mon/1/inv"), &Value::Int(1)).unwrap();
        c.set(node(&c, "/cfg/mon/1/wid"), &Value::Float(50.0)).unwrap();
        c.set(node(&c, "/cfg/mon/1/lim"), &Value::Float(-10.0)).unwrap();
        c.set(node(&c, "/cfg/mon/1/eqbdtrim"), &Value::Float(6.0)).unwrap();
        c.set(node(&c, "/cfg/mon/1/dim"), &Value::Float(30.0)).unwrap();
        c.set(node(&c, "/cfg/mon/1/srcmix"), &Value::Float(-20.0)).unwrap();
        c.set(node(&c, "/cfg/mon/1/src"), &Value::str("BUS.3")).unwrap();
        c.set(node(&c, "/cfg/mon/1/dirin"), &Value::str("AUX.6")).unwrap();
        assert!(phones.eq_on.load(Relaxed));
        assert_eq!((phones.eq[3].gain_db.load(), phones.eq[3].q.load()), (5.0, 3.0));
        assert_eq!(phones.eq[0].freq.load(), 120.0);
        assert_eq!(phones.eq[7].gain_db.load(), -4.0);
        assert!(phones.invert.load(Relaxed) && !speakers.invert.load(Relaxed));
        assert_eq!((phones.width.load(), phones.limiter_db.load(), phones.band_solo_db.load()), (50.0, -10.0, 6.0));
        assert_eq!(ctl.phones.dim_db.load(), 30.0);
        assert_eq!(ctl.phones.source.source_mix_db.load(), -20.0);
        assert_eq!(ctl.phones.source.source.load(Relaxed), 4 + 8 + 2, "BUS.3");
        assert_eq!(phones.direct_source.load(Relaxed), 40 + 5, "AUX.6");
        assert_eq!(c.get(node(&c, "/cfg/mon/1/dirin")), Some(Value::str("AUX.6")));
        assert_eq!(c.get(node(&c, "/cfg/mon/1/src")), Some(Value::str("BUS.3")));
        ctl.phones.level_db.store(-12.0);
        assert_eq!(c.get(node(&c, "/cfg/mon/1/$lvl")), Some(Value::Float(-12.0)));

        // /cfg/mon/2 is the SPEAKERS.
        c.set(node(&c, "/cfg/mon/2/dly/on"), &Value::Int(1)).unwrap();
        c.set(node(&c, "/cfg/mon/2/dly/m"), &Value::Float(17.0)).unwrap();
        c.set(node(&c, "/cfg/mon/2/pan"), &Value::Float(-30.0)).unwrap();
        c.set(node(&c, "/cfg/mon/2/pfldim"), &Value::Float(12.0)).unwrap();
        c.set(node(&c, "/cfg/mon/2/srclvl"), &Value::Float(-3.0)).unwrap();
        c.set(node(&c, "/cfg/mon/2/src"), &Value::str("OFF")).unwrap();
        c.set(node(&c, "/cfg/mon/2/dirin"), &Value::str("MTX.8")).unwrap();
        assert!(speakers.delay_on.load(Relaxed));
        assert_eq!((speakers.delay_m.load(), speakers.pan.load()), (17.0, -30.0));
        assert_eq!(ctl.speakers.pfl_dim_db.load(), 12.0);
        assert_eq!(ctl.speakers.source_level_db.load(), -3.0);
        assert_eq!(ctl.speakers.source.load(Relaxed), -1, "OFF");
        assert_eq!(speakers.direct_source.load(Relaxed), 75);
        c.set(node(&c, "/cfg/mon/2/dirin"), &Value::str("OFF")).unwrap();
        assert_eq!(speakers.direct_source.load(Relaxed), -1);
        assert!(c.is_mapped(node(&c, "/cfg/mon/2/eq/6f")));
    }

    #[test]
    fn sources_and_source_solo() {
        let e = engine();
        let st = WingState::default();
        let c = Console::new(&e, &st);
        let table = e.sources();
        // A 5 = port 4; B 1 = port 48; CRD 3 = playback channel 3.
        c.set(node(&c, "/io/in/A/5/name"), &Value::str("Snare")).unwrap();
        c.set(node(&c, "/io/in/A/5/col"), &Value::Int(9)).unwrap();
        c.set(node(&c, "/io/in/A/5/g"), &Value::Float(30.0)).unwrap();
        c.set(node(&c, "/io/in/A/5/vph"), &Value::Int(1)).unwrap();
        c.set(node(&c, "/io/in/A/5/pol"), &Value::Int(1)).unwrap();
        c.set(node(&c, "/io/in/A/5/icon"), &Value::Int(14)).unwrap();
        c.set(node(&c, "/io/in/A/5/tags"), &Value::str("#M2,#D3,drums")).unwrap();
        c.set(node(&c, "/io/in/B/1/mute"), &Value::Int(1)).unwrap();
        c.set(node(&c, "/io/in/CRD/3/name"), &Value::str("Play 3")).unwrap();
        let p = table.get(4).unwrap();
        assert_eq!(table.name(4), "Snare");
        assert_eq!((p.colour.load(Relaxed), p.icon.load(Relaxed)), (9, 14));
        assert_eq!(p.gain_db.load(), 30.0);
        assert!(p.phantom.load(Relaxed) && p.invert.load(Relaxed));
        assert_eq!(p.mute_groups.load(Relaxed), 0b10);
        assert_eq!(c.get(node(&c, "/io/in/A/5/tags")), Some(Value::str("#D3,#M2,drums")));
        assert!(table.get(48).unwrap().mute.load(Relaxed));
        assert_eq!(table.name(sources::source_index(PLAYBACK_SOURCE_BASE + 2).unwrap()), "Play 3");
        // $mute: 1 own mute, 2 a mute group.
        assert_eq!(c.get(node(&c, "/io/in/B/1/$mute")), Some(Value::Int(1)));
        assert_eq!(c.get(node(&c, "/io/in/A/5/$mute")), Some(Value::Int(0)));
        e.controls.mute_groups.store(0b10, Relaxed);
        assert_eq!(c.get(node(&c, "/io/in/A/5/$mute")), Some(Value::Int(2)));
        // Other groups share ports here: they stay in the shadow store.
        assert!(!c.is_mapped(node(&c, "/io/in/LCL/5/name")));
        // MODE pairs Live DAW's sources.
        c.set(node(&c, "/io/in/A/7/mode"), &Value::str("M/S")).unwrap();
        assert_eq!(table.get(7).unwrap().mode.load(Relaxed), source_mode::MID_SIDE);
        assert_eq!(c.get(node(&c, "/io/in/A/8/mode")), Some(Value::str("M/S")));

        // SOURCE SOLO: AUX7 hosts A 5.
        c.set(node(&c, "/cfg/solo/srcsolo"), &Value::str("AUX7")).unwrap();
        c.set(node(&c, "/cfg/solo/$srcsgrp"), &Value::Int(3)).unwrap(); // A
        c.set(node(&c, "/cfg/solo/$srcsin"), &Value::Int(5)).unwrap();
        c.set(node(&c, "/cfg/solo/$srcsolo"), &Value::Int(1)).unwrap();
        let m = &e.controls.monitor;
        assert_eq!(m.source_solo.load(Relaxed), 2);
        assert_eq!(m.source_solo_port.load(Relaxed), 4);
        assert!(m.source_solo_on.load(Relaxed));
        assert_eq!(c.get(node(&c, "/cfg/solo/$srcsgrp")), Some(Value::Int(3)));
        assert_eq!(c.get(node(&c, "/cfg/solo/$srcsin")), Some(Value::Int(5)));
        // The host's SOLO counts as a source solo; releasing $srcsolo releases it.
        c.set(node(&c, "/cfg/solo/$srcsolo"), &Value::Int(0)).unwrap();
        let aux7 = c.strip(StripKind::Aux, 7).unwrap();
        aux7.params.solo.store(true, Relaxed);
        assert_eq!(c.get(node(&c, "/cfg/solo/$srcsolo")), Some(Value::Int(1)));
        c.set(node(&c, "/cfg/solo/$srcsolo"), &Value::Int(0)).unwrap();
        assert!(!aux7.params.solo.load(Relaxed));
    }

    #[test]
    fn coverage_counts() {
        let cov = coverage();
        assert!(cov.total > 60000, "{}", cov.total);
        assert!(cov.mapped > 30000, "{}", cov.report());
    }
}
