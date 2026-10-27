//! The mapping layer: WING parameter paths and ids resolved to Sidecar's
//! engine state, in both directions, with the WING's units and ranges.
//!
//! Every static node of the tree gets a [`Binding`] computed once from its
//! path; model parameters bind through their slot (a strip's GATE, EQ,
//! DYNAMICS or filter tool, or an FX slot) and the active model's parameter
//! of the same name (our model tables come from the same sweep, so names and
//! ranges line up). Nodes without a Sidecar equivalent ([`Binding::Stored`]:
//! the cards, the console's hardware and network, other models' surfaces...)
//! answer the factory value a factory-initialized WING answered
//! ([`factory_value`]) and are kept in a shadow store ([`WingState`]), so a
//! client reads back what it wrote and snapshot files keep them, but they
//! don't affect the audio. SETUP options the application keeps in its
//! settings are bridged through the host ([`crate::host`]).

use crate::schema::{schema, Def, ModelSource, NoModels, Node, NodeType};
use crate::surface::{self, LayerF, Surface, SurfaceEdit, UserF};
use crate::value::{self, Value, FADER_MAX, FADER_MIN};
use livedaw_dsp::delay::delay_range;
use livedaw_dsp::util::MINUS_INF_DB;
use livedaw_dsp::wing::fx::allowed_in_slot;
use livedaw_dsp::wing::{model_index, ModelSpec, ParamKind, DYN_MODELS, EQ_MODELS_BUS, EQ_MODELS_CH, FILTER_TOOLS, FX_MODELS};
use livedaw_engine::analyser::{listen_mode, rta_view};
use livedaw_engine::atomic::AtomicF32;
use livedaw_engine::keytap::KeySource;
use livedaw_engine::monitor::{MonitorDspParams, MONITOR_EQ_BANDS};
use livedaw_engine::params::{
    ext, DynSlotParams, EngineControls, InsertParams, MonitorSourceParams, StripParams, FX_EXT, FX_NONE, NUM_DCAS, NUM_FX,
    NUM_MUTE_GROUPS,
};
use livedaw_engine::recorder::codec::FileFormat;
use livedaw_engine::recorder::usb::{PlayerState, RecFormat, RecState, RecTap};
use livedaw_engine::sends::send_mode;
use livedaw_engine::solo::solo_mode;
use livedaw_engine::sources::{self, source_mode, SourceParams};
use livedaw_engine::talk::TALK_OFF;
use livedaw_engine::{Engine, Strip, StripKind, PLAYBACK_SOURCE_BASE};
use livedaw_session::ConsoleModel;
use std::collections::{BTreeMap, HashMap, VecDeque};
use std::path::Path;
use std::sync::atomic::{AtomicBool, AtomicI32, Ordering::Relaxed};
use std::sync::{Arc, Mutex, OnceLock};

// ---- identity and shadow store ------------------------------------------------------

/// What the console says about itself (discovery, `/?`, `$syscfg`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Identity {
    pub name: String,
    pub model: String,
    /// `/$syscfg/$cnscfg`, the build type: WING Edit takes its ACTIVE WING
    /// MODEL from it ("wing-fullsize", "wing-bk", "wing-compact", "wing-rack").
    pub config: String,
    pub serial: String,
    pub firmware: String,
}

impl Default for Identity {
    fn default() -> Self {
        Self {
            name: "SIDECAR".into(),
            // The full-size WING's own model (`$cnsmdl`, discovery).
            model: "ngc-full".into(),
            config: "wing-fullsize".into(),
            serial: "SIDECAR0001".into(),
            // Our tree is the WING 3.1 tree (libwing compares the leading numbers).
            firmware: "3.1-0-sidecar:release".into(),
        }
    }
}

impl Identity {
    /// The model a console of `model` reports (discovery, `/?`,
    /// `/$syscfg/$cnsmdl`) as the WING family does (WING remote protocols,
    /// `$cnsmdl`): "ngc-full" for the full-size console, "wing-bk" for the
    /// black one (`black`, SURFACE FINISH BLACK), "wing-compact" for the
    /// COMPACT, so WING Edit and Mixing Station show the right model. The
    /// serial (SIDECAR0001) and firmware (3.1-0-sidecar) still tell Sidecar
    /// apart.
    pub fn model_name(model: ConsoleModel, black: bool) -> &'static str {
        match model {
            ConsoleModel::Full if black => BLACK_WING,
            ConsoleModel::Full => "ngc-full",
            ConsoleModel::Compact => "wing-compact",
        }
    }

    /// The build type (`/$syscfg/$cnscfg`) of a console of `model`, as WING
    /// Edit 3.3 names the models ("wing-fullsize", "wing-bk", "wing-rack",
    /// "wing-compact"; a factory-set silver WING answers "wing-fullsize").
    pub fn config_name(model: ConsoleModel, black: bool) -> &'static str {
        match model {
            ConsoleModel::Full if black => BLACK_WING,
            ConsoleModel::Full => "wing-fullsize",
            ConsoleModel::Compact => "wing-compact",
        }
    }

    /// The default identity of a console of `model` (`black`: the black
    /// full-size WING; the COMPACT is black whatever it says).
    pub fn of_console(model: ConsoleModel, black: bool) -> Self {
        Self { model: Self::model_name(model, black).into(), config: Self::config_name(model, black).into(), ..Self::default() }
    }

    /// The default identity of a (silver) console of `model`.
    pub fn of_model(model: ConsoleModel) -> Self {
        Self::of_console(model, false)
    }
}

/// The black full-size WING, in both `$cnsmdl` (the protocol document lists
/// it beside "ngc-full") and `$cnscfg` (WING Edit's model list).
const BLACK_WING: &str = "wing-bk";

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum ShadowKey {
    Row(u16),
    /// slot, model (index in the family), position.
    Param(u16, u16, u8),
}

/// The show's scenes as the host shows them (`/$ctl/lib`). Scenes live in
/// the show on the host's message thread: it publishes this and runs the
/// [`LibAction`]s clients queue.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Library {
    /// A host runs the actions (else `$action` is refused).
    pub attached: bool,
    pub show: String,
    pub num_scenes: i32,
    /// The current scene (-1 none), its name and EDIT TAG (0 none).
    pub current: i32,
    pub scene: String,
    pub tag: i32,
}

/// A scene recall for the host (`/$ctl/lib/$action`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LibAction {
    /// GO: the scene at an index (0-based).
    Go(i32),
    GoNext,
    GoPrev,
    /// GOTAG: the first scene with this tag (1-128).
    GoTag(i32),
}

/// The USB recorder's settings as the host keeps them (`/rec`). The host
/// names the files (REC PATH, the USB folder), so REC, NEW FILE and PLAYFILE
/// go to it as [`UsbAction`]s; the transport itself is the engine's
/// (`Engine::usb`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct UsbSettings {
    /// A host takes the actions (else REC from STOP, NEW FILE and PLAYFILE are refused).
    pub attached: bool,
    /// The USB storage folder (paths are shown relative to it).
    pub storage: String,
    /// REC PATH, relative to the storage folder ("/" separated).
    pub rec_path: String,
    /// The next recording's format: 2 or 4 channels, 16 or 24 bit.
    pub channels: i32,
    pub bits: i32,
}

impl Default for UsbSettings {
    fn default() -> Self {
        Self { attached: false, storage: String::new(), rec_path: String::new(), channels: 2, bits: 24 }
    }
}

/// What the host's USB recorder does for a client (`/rec/$action`, `/play/$action`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum UsbAction {
    /// REC from STOP: a new file in REC PATH.
    Record,
    NewFile,
    /// PLAYFILE: `$playfile`, relative to the storage folder.
    PlayFile(String),
}

/// The GLOBAL recall safes of the show (`/$ctl/safes/dca`, `mute`, `fx`;
/// bit n = DCA, mute group or FX slot n+1). The show lives on the host's
/// message thread, which publishes them and takes the clients' changes.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Safes {
    pub attached: bool,
    pub dcas: u32,
    pub mute_groups: u32,
    pub fx: u32,
}

/// State of the virtual console beyond the engine: values of nodes Sidecar
/// has no equivalent for, the console identity, the scene library, the USB
/// recorder's settings, the show's recall safes and the surface's layers and
/// custom controls.
#[derive(Default)]
pub struct WingState {
    pub shadow: Mutex<HashMap<ShadowKey, Value>>,
    pub identity: Mutex<Identity>,
    pub library: Mutex<Library>,
    /// `$actionidx`: the scene (or tag) GO and GOTAG use.
    pub lib_index: AtomicI32,
    lib_actions: Mutex<VecDeque<LibAction>>,
    /// The USB settings; `.1`: a client changed them (the host takes them).
    usb: Mutex<(UsbSettings, bool)>,
    usb_actions: Mutex<VecDeque<UsbAction>>,
    /// `/play/$playfile`.
    play_file: Mutex<String>,
    /// The global safes; `.1`: a client changed them.
    safes: Mutex<(Safes, bool)>,
    /// The show's layers and custom controls as the host published them.
    pub(crate) surface: Mutex<Surface>,
    pub(crate) surface_edits: Mutex<VecDeque<SurfaceEdit>>,
    /// The host's settings that are WING parameters (see [`crate::host`]).
    pub(crate) host: Mutex<crate::host::HostValues>,
    /// The console model and whether the full-size console is black (the
    /// identity and the COMPACT's own nodes follow them).
    console: Mutex<(ConsoleModel, bool)>,
}

impl WingState {
    pub fn new(identity: Identity) -> Self {
        Self { identity: Mutex::new(identity), ..Self::default() }
    }

    pub fn library(&self) -> Library {
        self.library.lock().unwrap_or_else(|e| e.into_inner()).clone()
    }

    /// Host: publishes the show's scenes (and that it runs the actions).
    pub fn set_library(&self, library: Library) {
        *self.library.lock().unwrap_or_else(|e| e.into_inner()) = library;
    }

    /// Host: the next scene recall a client asked for.
    pub fn take_lib_action(&self) -> Option<LibAction> {
        self.lib_actions.lock().unwrap_or_else(|e| e.into_inner()).pop_front()
    }

    fn push_lib_action(&self, a: LibAction) {
        let mut q = self.lib_actions.lock().unwrap_or_else(|e| e.into_inner());
        // A host that stopped polling must not let the queue grow.
        if q.len() < 64 {
            q.push_back(a);
        }
    }

    pub fn usb_settings(&self) -> UsbSettings {
        lock(&self.usb).0.clone()
    }

    /// Host: publishes its USB settings, unless a client's change is still
    /// to be taken (`take_usb_settings` first).
    pub fn set_usb_settings(&self, settings: UsbSettings) {
        let mut u = lock(&self.usb);
        if !u.1 {
            u.0 = settings;
        }
    }

    /// Host: the settings a client changed since the last call.
    pub fn take_usb_settings(&self) -> Option<UsbSettings> {
        let mut u = lock(&self.usb);
        std::mem::take(&mut u.1).then(|| u.0.clone())
    }

    fn change_usb_settings(&self, f: impl FnOnce(&mut UsbSettings)) {
        let mut u = lock(&self.usb);
        f(&mut u.0);
        u.1 = true;
    }

    /// Host: the next action a client asked the USB recorder for.
    pub fn take_usb_action(&self) -> Option<UsbAction> {
        lock(&self.usb_actions).pop_front()
    }

    fn push_usb_action(&self, a: UsbAction) {
        let mut q = lock(&self.usb_actions);
        if q.len() < 64 {
            q.push_back(a);
        }
    }

    pub fn safes(&self) -> Safes {
        lock(&self.safes).0
    }

    /// Host: publishes the show's global safes (unless a client's change is
    /// still to be taken).
    pub fn set_safes(&self, safes: Safes) {
        let mut s = lock(&self.safes);
        if !s.1 {
            s.0 = safes;
        }
    }

    fn change_safes(&self, f: impl FnOnce(&mut Safes)) {
        let mut s = lock(&self.safes);
        f(&mut s.0);
        s.1 = true;
    }

    /// Host: the safes a client changed since the last call.
    pub fn take_safes(&self) -> Option<Safes> {
        let mut s = lock(&self.safes);
        std::mem::take(&mut s.1).then_some(s.0)
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

    /// The console reports itself as `model` from now on (clients see it at
    /// their next discovery or `/?`).
    pub fn set_console_model(&self, model: ConsoleModel) {
        let black = lock(&self.console).1;
        self.set_console(model, black);
    }

    /// The full-size console is the black WING (`black`) or the silver one
    /// from now on: it reports "wing-bk" or "ngc-full" / "wing-fullsize".
    pub fn set_black_finish(&self, black: bool) {
        let model = lock(&self.console).0;
        self.set_console(model, black);
    }

    fn set_console(&self, model: ConsoleModel, black: bool) {
        *lock(&self.console) = (model, black);
        let mut id = lock(&self.identity);
        id.model = Identity::model_name(model, black).into();
        id.config = Identity::config_name(model, black).into();
    }

    /// The console model clients see.
    pub fn console_model(&self) -> ConsoleModel {
        lock(&self.console).0
    }

    pub fn clear_shadow(&self) {
        self.shadow.lock().unwrap_or_else(|e| e.into_inner()).clear();
    }
}

fn lock<T>(m: &Mutex<T>) -> std::sync::MutexGuard<'_, T> {
    m.lock().unwrap_or_else(|e| e.into_inner())
}

// ---- ports -------------------------------------------------------------------------

/// How Sidecar's inputs and outputs appear on the WING's patch.
///
/// Hardware ports are laid out as the AES50 groups: A 1-48 = ports 1-48, B
/// 1-48 = ports 49-96, C = 97-144. LCL is Sidecar's LOCAL IN / LOCAL OUT,
/// ROUTING's name for ports 1-48: LCL n is A n. The recorder's 64 playback
/// channels are the WING-LIVE card (CRD), the USB player's four channels the
/// PLAY group, the two oscillators the OSC group, USR 1-56 the user signals
/// and user patches (manual 2.6), BUS / MAIN / MTX n the strips' signals
/// (2k-1 = left of strip k, 2k = right). On import, the other input groups
/// (AUX, SC, USB, MOD, AES) land on the port of the same number.
pub mod port {
    use super::PLAYBACK_SOURCE_BASE;
    use livedaw_engine::routing::{strip_source, strip_source_input};
    use livedaw_engine::sources::{osc_channel, usr_channel, NUM_OSCILLATORS, NUM_USR_SOURCES, OSC_SOURCE_BASE, USR_SOURCE_BASE};
    use livedaw_engine::{StripKind, USB_CHANNELS, USB_PLAYER_SOURCE_BASE};

    pub const BLOCK: i32 = 48;
    pub const BLOCKS: [&str; 3] = ["A", "B", "C"];

    /// A strip input (-1 none, a hardware port, or a playback channel) as (group, number).
    pub fn source_to_wing(src: i32) -> Option<(&'static str, i32)> {
        if src < 0 {
            return Some(("OFF", 1));
        }
        if let Some(n) = osc_channel(src) {
            return Some(("OSC", n as i32 + 1));
        }
        if let Some(n) = livedaw_engine::recorder::usb_player_channel(src) {
            return Some(("PLAY", n as i32 + 1));
        }
        if let Some(n) = usr_channel(src) {
            return Some(("USR", n as i32 + 1));
        }
        if let Some((kind, number, side)) = strip_source(src) {
            let group = match kind {
                StripKind::Bus => "BUS",
                StripKind::Main => "MAIN",
                _ => "MTX",
            };
            return Some((group, 2 * number as i32 + side as i32 + 1));
        }
        if src >= PLAYBACK_SOURCE_BASE {
            let n = src - PLAYBACK_SOURCE_BASE;
            return (n < 64).then_some(("CRD", n + 1));
        }
        let block = (src / BLOCK) as usize;
        BLOCKS.get(block).map(|g| (*g, src % BLOCK + 1))
    }

    /// (group, number) to a strip input. None: no Sidecar equivalent.
    pub fn wing_to_source(group: &str, n: i32) -> Option<i32> {
        let n = n.max(1);
        match group {
            "OFF" => Some(-1),
            "A" => Some(n - 1),
            "B" => Some(BLOCK + n - 1),
            "C" => Some(2 * BLOCK + n - 1),
            "CRD" => (n <= 64).then_some(PLAYBACK_SOURCE_BASE + n - 1),
            "OSC" => (n <= NUM_OSCILLATORS as i32).then_some(OSC_SOURCE_BASE + n - 1),
            "PLAY" => (n <= USB_CHANNELS as i32).then_some(USB_PLAYER_SOURCE_BASE + n - 1),
            "USR" => (n <= NUM_USR_SOURCES as i32).then_some(USR_SOURCE_BASE + n - 1),
            "BUS" | "MAIN" | "MTX" => {
                let kind = match group {
                    "BUS" => StripKind::Bus,
                    "MAIN" => StripKind::Main,
                    _ => StripKind::Matrix,
                };
                Some(strip_source_input(kind, (n - 1) as usize / 2, (n - 1) as usize % 2)).filter(|s| *s >= 0)
            }
            "LCL" | "AUX" | "SC" | "USB" | "MOD" | "AES" => Some(n - 1),
            _ => None,
        }
    }

    /// A WING output (group, number) as a hardware output port.
    pub fn output_port(block: usize, n: i32) -> i32 {
        block as i32 * BLOCK + n - 1
    }

    /// The input groups in the WING's order (`/cfg/solo/$srcsgrp` 1-13).
    pub const IN_GROUPS: [&str; 13] = ["LCL", "AUX", "A", "B", "C", "SC", "USB", "CRD", "MOD", "PLAY", "AES", "USR", "OSC"];

    /// Input groups whose sources are Sidecar's sources (ROUTING > SOURCES):
    /// LOCAL IN (the same ports as AES50 A), the AES50 ports, the WING-LIVE
    /// card, the USB player, the oscillators and the USR group (user signals
    /// and user patches). The other groups only share port numbers with them
    /// here (for patches from WING snapshots): their source settings are
    /// stored as set.
    pub const SOURCE_GROUPS: [&str; 8] = ["LCL", "A", "B", "C", "CRD", "PLAY", "OSC", "USR"];

    /// Output groups that are Sidecar's outputs: LOCAL OUT (ports 1-8 here, as
    /// A 1-8) and the AES50 blocks.
    pub const OUT_GROUPS: [&str; 4] = ["LCL", "A", "B", "C"];

    /// An output group's first port (LCL is A).
    pub fn out_block(group: &str) -> Option<usize> {
        match group {
            "LCL" | "A" => Some(0),
            "B" => Some(1),
            "C" => Some(2),
            _ => None,
        }
    }
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
    /// `$stat`: "-" (no FX), "OK", "N/A" (the FX slot can't run).
    Stat,
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
    /// `clink`: LINK CUSTOMIZATION TO SOURCE (input and aux channels).
    CustLink,
    Led,
    Mute,
    Fader,
    Pan,
    Width,
    Solo,
    SoloLed,
    SoloSafe,
    /// `$muteovr`: MUTEGROUP/SIP OVERRIDE unmuted the strip.
    MuteOverride,
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
    /// Matrix DIRECT IN (`dir/in`: OFF, AES, MON.PH, MON.SPK, MON.BUS).
    DirIn,
    /// TAP WIDTH and TAP EQ (the WING's PRE EQ, `/ch/N/peq`): band 0-2.
    TapWidth,
    TapEqOn,
    TapEq(u8, TapF),
    /// LISTEN (`analyser::listen_mode`): `eq/$solo`, `gatesc/$solo`,
    /// `dynsc/$solo`, `dynxo/$solo`; and the band BAND SOLO plays (`eq/$solobd`).
    Listen(i32),
    ListenBand,
    /// `in/set/$g`, `$vph`: the GAIN and +48V of the source the strip is patched to.
    SrcGain,
    SrcPhantom,
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
    Icon,
    Led,
    Mute,
    Fader,
    Solo,
    SoloLed,
    /// SOLO BUS: A, B, A+B.
    Mon,
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
    /// The analysed strip (`$src`, read-only).
    Source,
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
    /// MONO / STEREO (`$BUS` ... sources, which are stereo).
    Mode,
}

/// `/io/in/USR/<n>/user/...`: what a user signal (USR 1-24: `grp` CH .. MTX,
/// `in`, `lr`, `tap`) or a user patch (USR 25-56: `grp` LCL .. AES, `in`)
/// copies (manual 2.6).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum UsrF {
    Grp,
    In,
    Lr,
    Tap,
}

/// `/io/in/OSC/<n>/osc/...`: an oscillator's settings.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum OscF {
    Level,
    Mode,
    Freq,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum InfoF {
    Name,
    Firmware,
    Serial,
    Model,
    /// `$cnscfg`.
    Config,
}

/// What a node of the tree is in Sidecar.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Binding {
    /// A Sidecar feature this layer doesn't bind (yet): the shadow store.
    None,
    /// No Sidecar equivalent (a card, the console's hardware, another
    /// model's surface...): answers the WING's factory value
    /// ([`factory_value`]) and keeps what a client sets, as the shadow store.
    Stored,
    /// A host setting (index into [`crate::host::HOST_SETTINGS`]).
    Host(u8),
    /// `/$globals/clkrate`: the audio device's rate (set on Sidecar's SETUP > AUDIO).
    ClockRate,
    /// `/io/in/$BUS|$MAIN|$MTX/<n>/...`: a bus, main or matrix as a source
    /// (read only): signal `n` (1-based, two per strip), the field.
    StripSource(StripKind, u16, SrcF),
    /// `/io/out/REC/<n>/grp`, `in` (false / true): what the USB recorder's
    /// RECORDER port n (0-based) records.
    RecOut(u8, bool),
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
    /// USR source (0-based: 0-23 user signals, 24-55 user patches): what it copies.
    UserSrc(u8, UsrF),
    /// Oscillator (0-based) settings.
    Osc(u8, OscF),
    /// RTA settings: false = HOME (`rta*`), true = EQ (`eq*`).
    Rta(bool, RtaF),
    /// The RTA on screen (`/cfg/rta/$src`, `$dec`, `$det`).
    RtaShown(RtaF),
    /// FX slot (0-based) assignment: the strip using it (`$a_chn`, false) or
    /// the insert it is in (`$a_pos`, true).
    FxAssign(u8, bool),
    /// `/$ctl/lib`: scene control (the show lives on the host's message thread).
    Lib(LibF),
    /// The USB player (`/play`) and recorder (`/rec`).
    Play(PlayF),
    Rec(RecF),
    /// Recall safes (`/$ctl/safes`).
    Safe(SafeF),
    /// `/$ctl/layer/<L|C|R>` (section 0-2) and `/$ctl/user`: the show's fader
    /// layers and custom controls (see [`surface`]).
    Layer(u8, LayerF),
    User(UserF),
}

/// `/play/...`: the transport (`$action`, `$actstate`, `$pos`), what plays
/// (`$actfile`, `$song`, the queue as `$actlist` / `$actidx`, the clip's
/// format), PLAYFILE's `$playfile` and REPEAT.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PlayF {
    Action,
    State,
    File,
    Song,
    List,
    Index,
    PlayFile,
    Pos,
    Total,
    Resolution,
    Channels,
    Rate,
    Format,
    Repeat,
}

/// `/rec/...`: `$action`, `$actstate`, `$actfile`, `$time`, REC PATH and the format.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RecF {
    Action,
    State,
    File,
    Path,
    Resolution,
    Channels,
    Time,
}

/// `/$ctl/safes/...`: the strips' recall safe (`ch`, `aux`, `bus`, `main`,
/// `mtx`) and the show's GLOBAL safes for DCAs, mute groups and FX slots.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SafeF {
    Strips(StripKind),
    Dcas,
    MuteGroups,
    Fx,
    /// `/$ctl/safes/source/<group>` of a group with Sidecar's sources
    /// (`SAFE_SOURCE_GROUPS`); the engine's source safes.
    Source(u8),
    /// `/$ctl/safes/output/<group>` of a group with Sidecar's outputs
    /// (`SAFE_OUTPUT_GROUPS`); the engine's output safes.
    Output(u8),
}

/// The source groups whose safes are Sidecar's sources, and their items on
/// the WING (`/$ctl/safes/source/*`); the other groups are only kept.
const SAFE_SOURCE_GROUPS: [(&str, u16); 7] = [("LCL", 24), ("A", 48), ("B", 48), ("C", 48), ("CRD", 64), ("PLAY", 4), ("OSC", 2)];
/// The output groups whose safes are Sidecar's outputs (`/$ctl/safes/output/*`).
const SAFE_OUTPUT_GROUPS: [(&str, u16); 4] = [("LCL", 8), ("A", 48), ("B", 48), ("C", 48)];

/// `/$ctl/lib/...`: `$action`, `$actionidx`, `$actidx`, `$active`, `$activeid`, `$actshow`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LibF {
    Action,
    ActionIndex,
    Current,
    Active,
    ActiveTag,
    Show,
}

fn kind_name(kind: StripKind) -> &'static str {
    match kind {
        StripKind::Channel => "ch",
        StripKind::Aux => "aux",
        StripKind::Bus => "bus",
        StripKind::Main => "main",
        StripKind::Matrix => "mtx",
    }
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
        ["clink"] if input => SF::CustLink,
        ["mute"] => SF::Mute,
        ["fdr"] => SF::Fader,
        ["pan"] => SF::Pan,
        ["wid"] => SF::Width,
        ["$solo"] => SF::Solo,
        ["$sololed"] => SF::SoloLed,
        ["solosafe"] => SF::SoloSafe,
        ["$muteovr"] => SF::MuteOverride,
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
        ["in", "set", "$g"] if input => SF::SrcGain,
        ["in", "set", "$vph"] if input => SF::SrcPhantom,
        ["eq", "$solo"] => SF::Listen(listen_mode::BAND),
        ["eq", "$solobd"] => SF::ListenBand,
        ["gatesc", "$solo"] if ch => SF::Listen(listen_mode::GATE_KEY),
        ["dynsc", "$solo"] => SF::Listen(listen_mode::DYN_KEY),
        ["dynxo", "$solo"] if kind != Aux => SF::Listen(listen_mode::XOVER),
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
                    "tap" => ScF::Tap, // aux, buses, mains, matrices: BUS .. INS2 (keytap.rs)
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
        ["preins", "$stat"] => SF::Ins(false, InsF::Stat),
        ["postins", "$stat"] if kind != Aux => SF::Ins(true, InsF::Stat),
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
                    "plink" if input => SendF::Plink,
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
                } else if kind == Bus && livedaw_engine::routing::bus_feeds_bus(n as usize - 1, b as usize - 1) {
                    // Any bus feeds any other bus (manual 5.10, BUS → BUS).
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
        ["dir", "in"] if kind == Matrix => SF::DirIn,
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
                    "icon" => Binding::Dca(i, DcaF::Icon),
                    "led" => Binding::Dca(i, DcaF::Led),
                    "mute" => Binding::Dca(i, DcaF::Mute),
                    "fdr" => Binding::Dca(i, DcaF::Fader),
                    "$solo" => Binding::Dca(i, DcaF::Solo),
                    "$sololed" => Binding::Dca(i, DcaF::SoloLed),
                    "mon" => Binding::Dca(i, DcaF::Mon),
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
        ["fx", n, f @ ("$a_chn" | "$a_pos")] => {
            number(n, NUM_FX as u16).map_or(Binding::None, |n| Binding::FxAssign(n as u8 - 1, *f == "$a_pos"))
        }
        ["play", f] => play_field(f).map_or(Binding::None, Binding::Play),
        ["rec", f] => rec_field(f).map_or(Binding::None, Binding::Rec),
        ["$ctl", "safes", f] => match *f {
            "dca" => Binding::Safe(SafeF::Dcas),
            "mute" => Binding::Safe(SafeF::MuteGroups),
            "fx" => Binding::Safe(SafeF::Fx),
            k => strip_kind(k).map_or(Binding::None, |kind| Binding::Safe(SafeF::Strips(kind))),
        },
        // Source and output safes of the groups Sidecar has (G5.6.3).
        ["$ctl", "safes", "source", g] => match SAFE_SOURCE_GROUPS.iter().position(|(n, _)| n == g) {
            Some(i) => Binding::Safe(SafeF::Source(i as u8)),
            None => Binding::Stored,
        },
        ["$ctl", "safes", "output", g] => match SAFE_OUTPUT_GROUPS.iter().position(|(n, _)| n == g) {
            Some(i) => Binding::Safe(SafeF::Output(i as u8)),
            None => Binding::Stored,
        },
        ["$ctl", "lib", f] => match *f {
            "$action" => Binding::Lib(LibF::Action),
            "$actionidx" => Binding::Lib(LibF::ActionIndex),
            "$actidx" => Binding::Lib(LibF::Current),
            "$active" => Binding::Lib(LibF::Active),
            "$activeid" => Binding::Lib(LibF::ActiveTag),
            "$actshow" => Binding::Lib(LibF::Show),
            _ => Binding::None,
        },
        ["$ctl", "layer", rest @ ..] => surface::bind_layer(rest).map_or(Binding::None, |(s, f)| Binding::Layer(s, f)),
        ["$ctl", "user", rest @ ..] => surface::bind_user(rest).map_or(Binding::None, Binding::User),
        ["cfg", "rta", "$src"] => Binding::RtaShown(RtaF::Source),
        ["cfg", "rta", "$dec"] => Binding::RtaShown(RtaF::Decay),
        ["cfg", "rta", "$det"] => Binding::RtaShown(RtaF::Detector),
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
        ["io", "out", "REC", n, f @ ("grp" | "in")] => {
            number(n, livedaw_engine::USB_CHANNELS as u16).map_or(Binding::Stored, |n| Binding::RecOut(n as u8 - 1, *f == "in"))
        }
        ["io", "out", g, n, f] => match (port::out_block(g), number(n, 64)) {
            (Some(b), Some(n)) if n as i32 <= port::BLOCK && (*f == "grp" || *f == "in") => Binding::Out(b as u8, n, *f == "in"),
            _ => Binding::None,
        },
        ["io", "in", g @ ("$BUS" | "$MAIN" | "$MTX"), n, f] => {
            let kind = match *g {
                "$BUS" => StripKind::Bus,
                "$MAIN" => StripKind::Main,
                _ => StripKind::Matrix,
            };
            let field = match *f {
                "name" => Some(SrcF::Name),
                "col" => Some(SrcF::Colour),
                "icon" => Some(SrcF::Icon),
                "tags" => Some(SrcF::Tags),
                "mode" => Some(SrcF::Mode),
                _ => None,
            };
            match (number(n, 2 * wing_strips(kind)), field) {
                (Some(n), Some(f)) => Binding::StripSource(kind, n, f),
                _ => Binding::None,
            }
        }
        ["io", "in", "USR", n, "user", f] => {
            let signal = |n: u16| (n as usize) <= sources::NUM_USER_SIGNALS;
            match (number(n, sources::NUM_USR_SOURCES as u16), *f) {
                (Some(n), "grp") => Binding::UserSrc(n as u8 - 1, UsrF::Grp),
                (Some(n), "in") => Binding::UserSrc(n as u8 - 1, UsrF::In),
                (Some(n), "lr") if signal(n) => Binding::UserSrc(n as u8 - 1, UsrF::Lr),
                (Some(n), "tap") if signal(n) => Binding::UserSrc(n as u8 - 1, UsrF::Tap),
                _ => Binding::None,
            }
        }
        ["io", "in", "OSC", n, "osc", f] => match (number(n, sources::NUM_OSCILLATORS as u16), *f) {
            (Some(n), "lvl") => Binding::Osc(n as u8 - 1, OscF::Level),
            (Some(n), "mode") => Binding::Osc(n as u8 - 1, OscF::Mode),
            (Some(n), "f") => Binding::Osc(n as u8 - 1, OscF::Freq),
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
        ["$syscfg", "$cnscfg"] => Binding::Info(InfoF::Config),
        ["$globals", "clkrate"] => Binding::ClockRate,
        _ => Binding::None,
    }
}

/// A Sidecar feature this layer leaves unbound: `/$ctl/daw/config` (DAW
/// CONFIGURATION), whose firmware 3.1 choices (CC, MSTR) can't show
/// Sidecar's MCU + extender setups.
fn unbound_feature(parts: &[&str]) -> bool {
    parts == ["$ctl", "daw", "config"]
}

/// The binding of a static node: what [`bind`] finds, a host setting, or the
/// WING's factory value where Sidecar has no equivalent.
fn bind_row(parts: &[&str]) -> Binding {
    match bind(parts) {
        Binding::None => {
            if let Some(h) = crate::host::find(&format!("/{}", parts.join("/"))) {
                Binding::Host(h)
            } else if unbound_feature(parts) {
                Binding::None
            } else {
                Binding::Stored
            }
        }
        b => b,
    }
}

fn play_field(f: &str) -> Option<PlayF> {
    Some(match f {
        "$action" => PlayF::Action,
        "$actstate" => PlayF::State,
        "$actfile" => PlayF::File,
        "$song" => PlayF::Song,
        "$actlist" => PlayF::List,
        "$actidx" => PlayF::Index,
        "$playfile" => PlayF::PlayFile,
        "$pos" => PlayF::Pos,
        "$total" => PlayF::Total,
        "$resolution" => PlayF::Resolution,
        "$channels" => PlayF::Channels,
        "$rate" => PlayF::Rate,
        "$format" => PlayF::Format,
        "repeat" => PlayF::Repeat,
        // $songs, $album, $artist (the files' tags, which Sidecar doesn't
        // read) and $actionidx stay in the shadow store.
        _ => return None,
    })
}

fn rec_field(f: &str) -> Option<RecF> {
    Some(match f {
        "$action" => RecF::Action,
        "$actstate" => RecF::State,
        "$actfile" => RecF::File,
        "$path" => RecF::Path,
        "resolution" => RecF::Resolution,
        "channels" => RecF::Channels,
        "$time" => RecF::Time,
        _ => return None,
    })
}

/// The number of strips of a kind on the WING (its safes lists).
fn wing_strips(kind: StripKind) -> u16 {
    match kind {
        StripKind::Channel => 40,
        StripKind::Aux | StripKind::Matrix => 8,
        StripKind::Bus => 16,
        StripKind::Main => 4,
    }
}

/// The items of a safes list.
fn safe_count(sf: SafeF) -> u16 {
    match sf {
        SafeF::Strips(kind) => wing_strips(kind),
        SafeF::Dcas => NUM_DCAS as u16,
        SafeF::MuteGroups => NUM_MUTE_GROUPS as u16,
        SafeF::Fx => NUM_FX as u16,
        SafeF::Source(g) => SAFE_SOURCE_GROUPS[g as usize].1,
        SafeF::Output(g) => SAFE_OUTPUT_GROUPS[g as usize].1,
    }
}

/// The engine flag behind item `n` (1-based) of a source or output safes
/// list: (outputs?, index) - a source table index or an output port.
fn io_safe_index(sf: SafeF, n: u16) -> Option<(bool, usize)> {
    match sf {
        SafeF::Source(g) => {
            let port = port::wing_to_source(SAFE_SOURCE_GROUPS[g as usize].0, i32::from(n))?;
            Some((false, livedaw_engine::sources::source_index(port)?))
        }
        SafeF::Output(g) => {
            let block = port::out_block(SAFE_OUTPUT_GROUPS[g as usize].0)?;
            usize::try_from(port::output_port(block, i32::from(n))).ok().map(|p| (true, p))
        }
        _ => None,
    }
}

/// A safes list as the console writes it: one character per item, "+" safe,
/// " " not ("+ +" = 1 and 3 of 3).
fn safe_flags(mask: u64, count: u16) -> String {
    (0..count.min(64)).map(|i| if mask & (1 << i) != 0 { '+' } else { ' ' }).collect()
}

/// A safes list as numbers and ranges, "1,3,5-8" (bit 0 = 1).
#[cfg(test)]
fn number_list(mask: u64) -> String {
    let mut out: Vec<String> = Vec::new();
    let mut n = 0;
    while n < 64 {
        if mask & (1 << n) == 0 {
            n += 1;
            continue;
        }
        let start = n;
        while n + 1 < 64 && mask & (1 << (n + 1)) != 0 {
            n += 1;
        }
        out.push(if n > start + 1 {
            format!("{}-{}", start + 1, n + 1)
        } else if n > start {
            format!("{},{}", start + 1, n + 1)
        } else {
            (start + 1).to_string()
        });
        n += 1;
    }
    out.join(",")
}

/// A safes list read back: the console's "+" / " " flags, one per item (a
/// string of 0/1 flags too), or numbers and ranges ("1,3,5-8", spaces or
/// commas between them); up to `max` items.
fn parse_number_list(text: &str, max: u16) -> u64 {
    let max = u64::from(max.min(64));
    let all = if max >= 64 { u64::MAX } else { (1 << max) - 1 };
    if text.bytes().all(|c| c == b'+' || c == b' ') {
        return text.bytes().enumerate().filter(|(_, c)| *c == b'+').fold(0, |m, (i, _)| m | 1 << i) & all;
    }
    let t = text.trim();
    if !t.is_empty() && t.len() as u64 == max && t.bytes().all(|c| c == b'0' || c == b'1') {
        return t.bytes().enumerate().filter(|(_, c)| *c == b'1').fold(0, |m, (i, _)| m | 1 << i);
    }
    let mut mask = 0u64;
    for part in t.split(|c: char| c == ',' || c.is_whitespace()).filter(|p| !p.is_empty()) {
        let (a, b) = part.split_once('-').unwrap_or((part, part));
        let (Ok(a), Ok(b)) = (a.trim().parse::<u64>(), b.trim().parse::<u64>()) else { continue };
        for n in a.max(1)..=b.min(max) {
            mask |= 1 << (n - 1);
        }
    }
    mask & all
}

/// A path under the USB storage folder as the WING shows it ("REC/R_1.wav");
/// the file name for a file outside it.
fn usb_path(storage: &str, path: &Path) -> String {
    if !storage.is_empty() {
        if let Ok(rel) = path.strip_prefix(storage) {
            let parts: Vec<String> = rel.components().map(|c| c.as_os_str().to_string_lossy().into_owned()).collect();
            return parts.join("/");
        }
    }
    path.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default()
}

fn mon_field(rest: &[&str]) -> Option<MonF> {
    let last = MONITOR_EQ_BANDS as u8 - 1;
    Some(match rest {
        ["$lvl"] | ["lvl"] | ["$lvlact"] => MonF::Level,
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

/// The MONITOR output group: 1 / 2 = PHONES L / R, 3 / 4 = SPEAKERS L / R
/// (monitor 1 is the PHONES, as `/cfg/mon/1`; ROUTING > OUTPUTS, fig. 32).
fn monitor_output(c: &EngineControls, k: i32) -> &AtomicI32 {
    match k {
        1 => &c.phones.output_left,
        2 => &c.phones.output_right,
        3 => &c.monitor_output_left,
        _ => &c.monitor_output_right,
    }
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
    ["LCL", "AUX", "A", "B", "C", "SC", "USB", "CRD", "MOD", "PLAY", "AES", "USR", "OSC"].into_iter().find(|x| *x == g)
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
                if s.defs[s.rows[r as usize].def as usize].is_leaf() {
                    bind_row(&refs)
                } else {
                    bind(&refs)
                }
            })
            .collect()
    })
}

/// The WING's factory values (`tools/wing`'s dump of a factory-initialized
/// console, folded by `examples/wing_factory.rs`).
static FACTORY: &str = include_str!("../data/wing_factory.tsv");

/// A path with its numbers as `*` (the factory table's patterns).
fn number_pattern(path: &str) -> String {
    path.split('/')
        .map(|p| if !p.is_empty() && p.bytes().all(|c| c.is_ascii_digit()) { "*" } else { p })
        .collect::<Vec<_>>()
        .join("/")
}

/// A factory table value as a node value.
fn factory_parse(def: &Def, text: &str) -> Option<Value> {
    match def.ty {
        NodeType::Node => None,
        NodeType::LinFloat | NodeType::LogFloat | NodeType::Fader => value::parse_number(text).map(Value::Float),
        NodeType::Int => text.trim().parse::<i32>().ok().map(Value::Int),
        NodeType::Str => Some(Value::str(text)),
        // An item our tree doesn't list (a list the console fills at run time) shows as is.
        NodeType::StrEnum => value::parse_display(def, text).or_else(|| Some(Value::str(text))),
        NodeType::FloatEnum => value::parse_display(def, text),
    }
}

/// The WING's factory value of a static parameter (None where the dump
/// has none: a value of the dumped console's own, like its IP address).
pub fn factory_value(row: u16) -> Option<&'static Value> {
    static T: OnceLock<Vec<Option<Value>>> = OnceLock::new();
    T.get_or_init(|| {
        let (mut exact, mut patterns) = (HashMap::new(), HashMap::new());
        for line in FACTORY.lines() {
            if let Some((k, v)) = line.split_once('\t') {
                if k.contains('*') {
                    patterns.insert(k, v);
                } else {
                    exact.insert(k, v);
                }
            }
        }
        let s = schema();
        (0..s.num_rows() as u16)
            .map(|r| {
                let def = &s.defs[s.rows[r as usize].def as usize];
                if !def.is_leaf() {
                    return None;
                }
                let path = format!("/{}", s.row_parts(r).join("/"));
                let text = exact.get(path.as_str()).or_else(|| patterns.get(number_pattern(&path).as_str()))?;
                factory_parse(def, text)
            })
            .collect()
    })
    .get(row as usize)?
    .as_ref()
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
    /// The value is valid on the WING but Sidecar can't do it.
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
        // A custom control's mode (shadow while no host publishes the show).
        if let Some(mode) = s.cc_mode_row(slot) {
            return match self.get(Node::Row(mode))? {
                Value::Str(token) => s.model_index(slot, &token),
                _ => None,
            };
        }
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

/// PAN LINK: 0 off, 1 linked, 2 linked reversed.
fn plink_value(link: &AtomicBool, reversed: &AtomicBool) -> i32 {
    match (link.load(Relaxed), reversed.load(Relaxed)) {
        (false, _) => 0,
        (true, false) => 1,
        (true, true) => 2,
    }
}

fn set_plink(link: &AtomicBool, reversed: &AtomicBool, v: &Value) {
    let k = v.as_i32().unwrap_or(0);
    link.store(k != 0, Relaxed);
    reversed.store(k == 2, Relaxed);
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

    /// The WING's 76 strips with their number in its strip order (FX `$a_chn`,
    /// RTA `$src`): channels 1-40, aux 41-48, buses 49-64, mains 65-68, matrices 69-76.
    fn numbered(&self) -> impl Iterator<Item = (StripKind, i32, &Arc<Strip>)> + '_ {
        fn list(kind: StripKind, base: i32, max: usize, v: &[Arc<Strip>]) -> impl Iterator<Item = (StripKind, i32, &Arc<Strip>)> {
            v.iter().take(max).enumerate().map(move |(i, s)| (kind, base + i as i32 + 1, s))
        }
        list(StripKind::Channel, 0, 40, &self.channels)
            .chain(list(StripKind::Aux, 40, 8, &self.aux))
            .chain(self.buses.iter().enumerate().filter_map(|(i, s)| Some((StripKind::Bus, 48 + i as i32 + 1, s.as_ref()?))))
            .chain(list(StripKind::Main, 64, 4, &self.mains))
            .chain(list(StripKind::Matrix, 68, 8, &self.matrices))
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
                Binding::None | Binding::Stored => false,
                Binding::Strip(kind, n, _) => self.strip(kind, n).is_some(),
                Binding::Layer(..) => self.state.surface().attached,
                Binding::User(_) => self.state.surface().attached,
                _ => true,
            },
            Node::Param { slot, index } => match self.cc_param(slot) {
                Some(_) => self.state.surface().attached,
                None => self.engine_param(slot, index).is_some(),
            },
        }
    }

    /// A custom control key's or encoder's slot (`/$ctl/user/.../bu`, `bd`,
    /// `enc`; see surface.rs): the layer, control and slot of its `mode`.
    fn cc_param(&self, slot: u16) -> Option<(u8, u8, surface::CcSlot)> {
        match bindings()[schema().cc_mode_row(slot)? as usize] {
            Binding::User(UserF::Mode(l, c, cs)) => Some((l, c, cs)),
            _ => None,
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
            Node::Param { slot, index } => match self.cc_param(slot) {
                Some((l, c, cs)) => {
                    let sf = self.state.surface();
                    if sf.attached {
                        sf.get_cc_param(l, c, cs, index).and_then(|v| value::coerce(def, &v))
                    } else {
                        None
                    }
                }
                None => {
                    self.engine_param(slot, index).map(|(values, spec, i)| param_to_wing(&spec.params[i], def, values[i].load()))
                }
            },
        };
        if let Some(v) = mapped {
            // On the parameter's nearest step, as the console reports it (and
            // WING Edit shows it): an engine default between two steps (a HALL
            // decay of 2.00 s) would otherwise read 2.00 here and 2.03 there.
            return Some(match self.binding(node) {
                Binding::Strip(_, _, SF::Dly) => v,
                _ => value::quantise(def, &v),
            });
        }
        if let Some(v) = self.shadow_key(node).and_then(|k| self.state.shadow_get(k)) {
            return Some(v);
        }
        if let Node::Row(r) = node {
            if let Some(v) = factory_value(r) {
                return Some(v.clone());
            }
        }
        Some(value::default_value(def))
    }

    fn get_row(&self, row: u16, def: &Def) -> Option<Value> {
        let e = self.engine;
        let c = &e.controls;
        Some(match bindings()[row as usize] {
            Binding::None | Binding::Stored | Binding::FxSlot(_) => return None,
            Binding::Strip(kind, n, field) => return self.get_strip(row, def, kind, n, field),
            Binding::Host(h) => return crate::host::get(self.state, h, def),
            Binding::ClockRate => {
                let rate = e.sample_rate();
                value::coerce(def, &Value::Float(if (rate - 44100.0).abs() < 1.0 { 44100.0 } else { 48000.0 }))?
            }
            Binding::StripSource(kind, n, field) => {
                let st = self.strip(kind, n.div_ceil(2))?;
                let p = &st.params;
                match field {
                    SrcF::Name => Value::Str(st.name()),
                    SrcF::Colour => int(&p.colour),
                    SrcF::Icon => int(&p.icon),
                    // Buses, mains and matrices are stereo here.
                    SrcF::Mode => value::coerce(def, &Value::str("ST"))?,
                    _ => {
                        let path = format!("/{}/{}/tags", kind_name(kind), n.div_ceil(2));
                        return self.get(schema().resolve(&path, &NoModels)?);
                    }
                }
            }
            Binding::RecOut(port, number) => {
                let (g, k) = self.rec_tap_pair(row, port as usize);
                if number {
                    Value::Int(k)
                } else {
                    Value::str(g)
                }
            }
            Binding::Dca(i, field) => {
                let d = c.dcas.get(i as usize)?;
                match field {
                    DcaF::Name => Value::Str(e.dca_name(i as usize)),
                    DcaF::Colour => int(&d.colour),
                    DcaF::Icon => int(&d.icon),
                    DcaF::Led => b(&d.led),
                    DcaF::Mute => b(&d.mute),
                    DcaF::Fader => lvl(&d.fader_db),
                    DcaF::Solo | DcaF::SoloLed => b(&d.solo),
                    DcaF::Mon => item(def, d.mon.load(Relaxed)),
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
                    RtaF::Source => return None,
                }
            }
            Binding::RtaShown(rf) => {
                let a = e.analysis().controls();
                let v = if a.view.load(Relaxed) == rta_view::HOME { &a.home } else { &a.eq };
                match rf {
                    RtaF::Source => {
                        let kind = StripKind::from_i32(a.strip_kind.load(Relaxed))?;
                        let id = a.strip_id.load(Relaxed);
                        Value::Int(self.numbered().find(|(k, _, s)| *k == kind && s.id() == id).map(|(_, n, _)| n)?)
                    }
                    RtaF::Decay => item(def, v.decay.load(Relaxed)),
                    RtaF::Detector => item(def, v.detector.load(Relaxed)),
                    _ => return None,
                }
            }
            Binding::FxAssign(i, pos) => {
                // The first insert using the slot, in the WING's strip order.
                let (number, post) = self
                    .numbered()
                    .find_map(|(kind, n, s)| {
                        let p = &s.params;
                        let post = kind != StripKind::Aux && p.post_insert.fx.load(Relaxed) == i as i32;
                        (p.pre_insert.fx.load(Relaxed) == i as i32 || post).then_some((n, post))
                    })
                    .unwrap_or((0, false));
                Value::Int(if pos { post as i32 } else { number })
            }
            Binding::Osc(n, of) => {
                let o = e.sources().osc(n as usize)?;
                match of {
                    OscF::Level => f(&o.level_db),
                    OscF::Mode => item(def, o.mode.load(Relaxed)),
                    OscF::Freq => f(&o.freq_hz),
                }
            }
            Binding::Play(pf) => return self.get_play(def, pf),
            Binding::Rec(rf) => return self.get_rec(def, rf),
            Binding::Safe(sf) => Value::Str(safe_flags(self.safe_mask(sf), safe_count(sf))),
            Binding::Lib(lf) => {
                let lib = self.state.library();
                match lf {
                    LibF::Action => item(def, 0), // IDLE: an action is taken at once
                    LibF::ActionIndex => Value::Int(self.state.lib_index.load(Relaxed)),
                    // No scene active reads 0, inside the range, as on the console.
                    LibF::Current => Value::Int(lib.current.max(0)),
                    LibF::Active => Value::Str(lib.scene),
                    LibF::ActiveTag => Value::Int(lib.tag),
                    LibF::Show => Value::Str(lib.show),
                }
            }
            // Shadow until a host publishes the show.
            Binding::Layer(s, lf) => {
                let sf = self.state.surface();
                return if sf.attached { sf.get_layer(s, lf) } else { None };
            }
            Binding::User(uf) => {
                let sf = self.state.surface();
                return if sf.attached { sf.get_user(uf) } else { None };
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
                    SrcF::Mode => return None,
                }
            }
            Binding::UserSrc(i, uf) => match e.sources().user_signals().get(i as usize) {
                Some(u) => match uf {
                    UsrF::Grp => item(def, u.group.load(Relaxed)),
                    UsrF::In => Value::Int(u.number.load(Relaxed)),
                    UsrF::Lr => item(def, u.side.load(Relaxed)),
                    UsrF::Tap => item(def, u.post.load(Relaxed) as i32),
                },
                None => {
                    let (g, k) = self.user_patch_pair(row, i as usize - sources::NUM_USER_SIGNALS);
                    if uf == UsrF::In {
                        Value::Int(k)
                    } else {
                        Value::str(g)
                    }
                }
            },
            Binding::Info(info) => {
                let id = self.state.identity();
                Value::Str(match info {
                    InfoF::Name => id.name,
                    InfoF::Firmware => id.firmware,
                    InfoF::Serial => id.serial,
                    InfoF::Model => id.model,
                    InfoF::Config => id.config,
                })
            }
            Binding::Out(block, n, number) => {
                let port = port::output_port(block as usize, n as i32);
                let (g, k) = self.out_pair(row, port);
                if number {
                    Value::Int(k)
                } else {
                    Value::str(g)
                }
            }
            Binding::SourceMode(group, n) if source_of(group, n).is_some() => {
                // Sidecar's sources pair like the WING's (ROUTING > SOURCES).
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

    /// What feeds hardware output `port`: ("MAIN", 1) etc. None: nothing Sidecar knows of.
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
        if let Some(k) = (1..=4).find(|&k| monitor_output(c, k).load(Relaxed) == port) {
            return Some(("MON", k));
        }
        // FX SEND 2i+1 / 2i+2: FX slot i+1's outboard insert send.
        for (i, f) in c.fx.iter().enumerate() {
            for (side, o) in [&f.send_left, &f.send_right].into_iter().enumerate() {
                if o.load(Relaxed) == port {
                    return Some(("SEND", 2 * i as i32 + side as i32 + 1));
                }
            }
        }
        // A source patched straight to the output (ROUTING > OUTPUTS).
        let src = self.engine.output_patch().get(port);
        port::source_to_wing(src).filter(|_| src >= 0)
    }

    /// The input patch of a strip as the WING shows it. Sidecar stores one
    /// port per input; the (group, number) a client or snapshot set is kept, and
    /// shown while it still names that port (LCL 5 and A 5 are the same port
    /// here), or when Sidecar can't patch it at all (a bus as a source).
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
            SF::Name => Value::Str(s.name()),
            SF::Colour => int(&p.colour),
            SF::Icon => int(&p.icon),
            // What the strip shows: its source's while customization is linked.
            SF::ActName | SF::ActColour | SF::ActIcon => {
                let table = self.engine.sources();
                match self.linked_source(kind, n, s).and_then(|i| Some((i, table.get(i)?))) {
                    Some((i, src)) => match field {
                        SF::ActName => Value::Str(table.name(i)),
                        SF::ActColour => int(&src.colour),
                        _ => int(&src.icon),
                    },
                    None => match field {
                        SF::ActName => Value::Str(s.name()),
                        SF::ActColour => int(&p.colour),
                        _ => int(&p.icon),
                    },
                }
            }
            SF::CustLink => b(&p.cust_link),
            SF::Led => b(&p.led),
            SF::Mute => b(&p.mute),
            SF::Fader | SF::EffFader => lvl(&p.fader_db),
            SF::Pan => f(&p.pan),
            SF::Width => f(&p.width),
            SF::Solo => b(&p.solo),
            SF::SoloLed => b(&p.solo),
            SF::SoloSafe => b(&p.solo_safe),
            SF::MuteOverride => b(&s.console.mute_override),
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
                    // Input channels: SELF, CH.n; the others SELF, BUS.n, MAIN.n,
                    // MTX.n, AUX.n (a source the list lacks reads as SELF).
                    ScF::Src => {
                        let name = KeySource::from_code(d.sc_source.load(Relaxed)).wing_name();
                        value::coerce(def, &Value::Str(name)).unwrap_or(Value::str("SELF"))
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
                    InsF::Stat => Value::str(match usize::try_from(ins.fx.load(Relaxed)) {
                        Err(_) => "-",
                        Ok(i) if self.fx_runs(i) => "OK",
                        Ok(_) => "N/A",
                    }),
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
                    SendF::Plink => Value::Int(plink_value(&send.pan_link, &send.pan_reversed)),
                    SendF::Pan => f(&send.pan),
                    SendF::Pre => Value::bool(send.mode.load(Relaxed) == send_mode::PRE),
                },
                // Never configured: the engine's defaults (the WING's factory send).
                None => match sf {
                    SendF::On | SendF::Pon => Value::Int(0),
                    SendF::Plink => {
                        Value::Int(livedaw_engine::sends::default_pan_link(s.sends.default_mode(slot as usize)) as i32)
                    }
                    SendF::Lvl => Value::Float(db_to_wing(livedaw_engine::sends::DEFAULT_SEND_LEVEL_DB)),
                    SendF::Mode => item(def, s.sends.default_mode(slot as usize)),
                    SendF::Pan => Value::Float(0.0),
                    SendF::Pre => Value::bool(s.sends.default_mode(slot as usize) == send_mode::PRE),
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
                    SendF::Plink => Value::Int(plink_value(&m.pan_link, &m.pan_reversed)),
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
            SF::DirIn => item(def, p.direct_source.load(Relaxed)),
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
            SF::Listen(mode) => Value::bool(self.listening(kind, s, mode)),
            // The band played while listening here; else the band set (shadow).
            SF::ListenBand if self.listening(kind, s, listen_mode::BAND) => {
                Value::Int(self.engine.analysis().controls().listen_band.load(Relaxed) + 1)
            }
            SF::ListenBand => return None,
            SF::SrcGain => f(&self.engine.sources().get(self.strip_source(kind, n, s)?)?.gain_db),
            SF::SrcPhantom => b(&self.engine.sources().get(self.strip_source(kind, n, s)?)?.phantom),
        })
    }

    /// LISTEN `mode` is on for strip `s`.
    fn listening(&self, kind: StripKind, s: &Strip, mode: i32) -> bool {
        let a = self.engine.analysis().controls();
        a.listen_mode.load(Relaxed) == mode && a.listen_kind.load(Relaxed) == kind as i32 && a.listen_id.load(Relaxed) == s.id()
    }

    /// The source (table index) an input strip takes, as the WING shows its
    /// patch (the ALT input while in use): only `port::SOURCE_GROUPS`.
    /// The source whose name, colour and icon the strip shows (customization
    /// linked and a source in use).
    fn linked_source(&self, kind: StripKind, n: u16, s: &Strip) -> Option<usize> {
        if !kind.is_input() || !s.params.cust_link.load(Relaxed) {
            return None;
        }
        self.strip_source(kind, n, s)
    }

    fn strip_source(&self, kind: StripKind, n: u16, s: &Strip) -> Option<usize> {
        let c = &self.engine.controls;
        let alt = s.uses_alt_input(c.input_select_global.load(Relaxed), c.global_alt.load(Relaxed));
        let (g, k) = self.conn_pair(*conn_rows().get(&(kind, n))?, &s.params, alt);
        source_of(g, u16::try_from(k).ok()?)
    }

    /// FX slot `i` holds a device that runs (a model, EXT with a plugin, or
    /// EXT as an outboard insert with a RETURN) and hasn't faulted.
    fn fx_runs(&self, i: usize) -> bool {
        let (Some(p), Some(rack)) = (self.engine.controls.fx.get(i), self.engine.fx(i)) else { return false };
        let model = p.model_index();
        model != FX_NONE && (model != FX_EXT || rack.is_loaded() || rack.outboard_returns(p)) && !rack.is_faulted()
    }

    fn get_play(&self, def: &Def, pf: PlayF) -> Option<Value> {
        let d = self.engine.usb();
        let entry = d.current_entry();
        let info = d.file_info(entry);
        let path = d.queue().entry(entry).map(|q| q.path.clone()).or_else(|| info.as_ref().map(|i| i.path.clone()));
        let named = |text: &str| value::coerce(def, &Value::str(text));
        Some(match pf {
            PlayF::Action => item(def, 0), // IDLE: an action is taken at once
            PlayF::State => item(def, d.player_state() as i32),
            PlayF::File => Value::Str(path.map(|p| usb_path(&self.state.usb_settings().storage, &p)).unwrap_or_default()),
            PlayF::Song => {
                Value::Str(path.and_then(|p| p.file_stem().map(|s| s.to_string_lossy().into_owned())).unwrap_or_default())
            }
            PlayF::List => {
                // The playlist, else the folder the queue plays.
                let q = d.queue();
                let from = q.playlist_file().or_else(|| q.folder()).and_then(|p| p.file_stem());
                Value::Str(from.map(|s| s.to_string_lossy().into_owned()).unwrap_or_default())
            }
            // 0 with nothing queued, as the console.
            PlayF::Index => Value::Int(d.queue().index_of(entry).map_or(0, |i| i as i32)),
            PlayF::PlayFile => Value::Str(lock(&self.state.play_file).clone()),
            PlayF::Pos => Value::Float(seconds(d.position(), d.sample_rate())),
            PlayF::Total => Value::Float(seconds(d.length(), d.sample_rate())),
            PlayF::Resolution => named(if info?.bits <= 16 { "16" } else { "24" })?,
            PlayF::Channels => item(def, info?.channels as i32 - 1),
            PlayF::Rate => named(if info?.sample_rate == 44100 { "44.1" } else { "48" })?,
            PlayF::Format => named(match info?.format {
                FileFormat::Mp3 => "MP3",
                FileFormat::Flac => "FLAC",
                // AIFF is uncompressed PCM, as WAV (the WING doesn't list it).
                FileFormat::Wav | FileFormat::Aiff => "WAV",
            })?,
            PlayF::Repeat => Value::bool(d.queue().repeat()),
        })
    }

    fn set_play(&self, def: &Def, pf: PlayF, v: &Value) -> Result<(), SetError> {
        let d = self.engine.usb();
        match pf {
            PlayF::Action => match item_name(def, v)? {
                "STOP" => d.stop(),
                "PLAY" => d.play(),
                // PAUSE pauses, or goes on when paused (the PAUSE key).
                "PAUSE" => match d.player_state() {
                    PlayerState::Playing => d.pause(),
                    PlayerState::Paused => d.play(),
                    _ => {}
                },
                "NEXT" => {
                    d.next();
                }
                "PREV" => {
                    d.prev();
                }
                "PLAYFILE" => {
                    let file = lock(&self.state.play_file).clone();
                    if file.trim().is_empty() {
                        return Err(SetError::BadValue);
                    }
                    if !self.state.usb_settings().attached {
                        return Err(SetError::Unsupported("no USB storage to play files from".into()));
                    }
                    self.state.push_usb_action(UsbAction::PlayFile(file));
                }
                _ => {}
            },
            PlayF::PlayFile => *lock(&self.state.play_file) = str_of(v),
            PlayF::Pos => d.seek((f64::from(v.as_f32().unwrap_or(0.0)) * d.sample_rate()).round() as i64),
            PlayF::Repeat => d.queue().set_repeat(v.as_bool()),
            _ => return Err(SetError::ReadOnly),
        }
        Ok(())
    }

    fn get_rec(&self, def: &Def, rf: RecF) -> Option<Value> {
        let d = self.engine.usb();
        let u = self.state.usb_settings();
        Some(match rf {
            RecF::Action => item(def, 0),
            RecF::State => item(def, d.rec_state() as i32),
            RecF::File => Value::Str(d.rec_file().map(|p| usb_path(&u.storage, &p)).unwrap_or_default()),
            RecF::Path => Value::Str(u.rec_path),
            RecF::Resolution => value::coerce(def, &Value::Str(u.bits.to_string()))?,
            RecF::Channels => value::coerce(def, &Value::Str(u.channels.to_string()))?,
            RecF::Time => Value::Float(seconds(d.rec_time(), d.sample_rate())),
        })
    }

    fn set_rec(&self, def: &Def, rf: RecF, v: &Value) -> Result<(), SetError> {
        let d = self.engine.usb();
        let attached = self.state.usb_settings().attached;
        let no_storage = || SetError::Unsupported("no USB storage to record to".into());
        match rf {
            RecF::Action => match (item_name(def, v)?, d.rec_state()) {
                ("STOP", _) => d.stop_recording(),
                // Resuming a paused recording keeps its file: no name needed.
                ("REC" | "PAUSE", RecState::Paused) => {
                    d.record(Path::new(""), RecFormat::default()).map_err(SetError::Unsupported)?
                }
                ("REC", RecState::Stopped) if attached => self.state.push_usb_action(UsbAction::Record),
                ("REC", RecState::Stopped) => return Err(no_storage()),
                ("PAUSE", RecState::Recording) => d.pause_recording(),
                ("NEWFILE", RecState::Recording | RecState::Paused) if attached => self.state.push_usb_action(UsbAction::NewFile),
                ("NEWFILE", RecState::Recording | RecState::Paused) => return Err(no_storage()),
                _ => {}
            },
            RecF::Path => {
                let path = str_of(v);
                self.state.change_usb_settings(|u| u.rec_path = path);
            }
            RecF::Resolution | RecF::Channels => {
                let n: i32 = item_name(def, v)?.parse().map_err(|_| SetError::BadValue)?;
                self.state.change_usb_settings(|u| if rf == RecF::Channels { u.channels = n } else { u.bits = n });
            }
            RecF::State | RecF::File | RecF::Time => return Err(SetError::ReadOnly),
        }
        Ok(())
    }

    /// The strips (bit n = WING number n+1), DCAs, mute groups or FX slots that are recall-safe.
    fn safe_mask(&self, sf: SafeF) -> u64 {
        let g = self.state.safes();
        match sf {
            SafeF::Strips(kind) => (1..=wing_strips(kind))
                .filter(|&n| self.strip(kind, n).is_some_and(|s| s.params.recall_safe.load(Relaxed)))
                .fold(0, |m, n| m | 1 << (n - 1)),
            SafeF::Dcas => u64::from(g.dcas),
            SafeF::MuteGroups => u64::from(g.mute_groups),
            SafeF::Fx => u64::from(g.fx),
            SafeF::Source(_) | SafeF::Output(_) => {
                let s = self.engine.io_safes();
                (1..=safe_count(sf))
                    .filter(|&n| match io_safe_index(sf, n) {
                        Some((false, i)) => s.sources.get(i),
                        Some((true, i)) => s.outputs.get(i),
                        None => false,
                    })
                    .fold(0, |m, n| m | 1 << (n - 1))
            }
        }
    }

    fn set_safes(&self, sf: SafeF, text: &str) {
        let max = safe_count(sf);
        let mask = parse_number_list(text, max);
        if let SafeF::Strips(kind) = sf {
            for n in 1..=max {
                if let Some(st) = self.strip(kind, n) {
                    st.params.recall_safe.store(mask & (1 << (n - 1)) != 0, Relaxed);
                }
            }
            return;
        }
        if matches!(sf, SafeF::Source(_) | SafeF::Output(_)) {
            let s = self.engine.io_safes();
            for n in 1..=max {
                match io_safe_index(sf, n) {
                    Some((false, i)) => s.sources.set(i, mask & (1 << (n - 1)) != 0),
                    Some((true, i)) => s.outputs.set(i, mask & (1 << (n - 1)) != 0),
                    None => {}
                }
            }
            return;
        }
        self.state.change_safes(|g| match sf {
            SafeF::Dcas => g.dcas = mask as u32,
            SafeF::MuteGroups => g.mute_groups = mask as u32,
            _ => g.fx = mask as u32,
        });
    }

    /// SOURCE SOLO is on: `$srcsolo`, or the host's own SOLO (as the engine sees it).
    fn source_solo_on(&self) -> bool {
        let m = &self.engine.controls.monitor;
        m.host().is_some_and(|(kind, i)| {
            m.source_solo_on.load(Relaxed) || self.strip(kind, i as u16 + 1).is_some_and(|s| s.params.solo.load(Relaxed))
        })
    }

    /// Sets a parameter. The value is coerced to the parameter's type and range
    /// and, like the console, put on the parameter's nearest step first;
    /// returns the value now in effect.
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
            // (Set below, in the strip's unit's range.)
            (_, Binding::Strip(_, _, SF::Dly)) => {
                Value::Float(v.as_f32().filter(|x| x.is_finite()).ok_or(SetError::BadValue)?.max(0.0))
            }
            _ => value::quantise(def, &value::coerce(def, v).ok_or(SetError::BadValue)?),
        };
        let applied = match node {
            Node::Root => return Err(SetError::NotFound),
            Node::Row(r) => self.set_row(r, def, &cv)?,
            Node::Param { slot, index } => match (self.cc_param(slot), self.engine_param(slot, index)) {
                (Some((l, c, cs)), _) => {
                    let mut sf = self.state.surface();
                    if !sf.attached {
                        false
                    } else if let Some(edit) = sf.set_cc_param(l, c, cs, index, &cv)? {
                        self.state.push_surface_edit(edit);
                        true
                    } else {
                        false // kept as set (no equivalent here)
                    }
                }
                (None, Some((values, spec, i))) => {
                    let x = param_from_wing(&spec.params[i], def, &cv).ok_or(SetError::BadValue)?;
                    values[i].store(x);
                    true
                }
                (None, None) => false,
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
            Binding::None | Binding::Stored | Binding::FxSlot(_) => return Ok(false),
            Binding::Strip(kind, n, field) => return self.set_strip(row, def, kind, n, field, v),
            Binding::Host(h) => crate::host::set(self.state, h, def, v)?,
            Binding::ClockRate => {
                if self.get_row(row, def).as_ref() != Some(v) {
                    return Err(SetError::Unsupported("the audio device sets the rate (SETUP > AUDIO)".into()));
                }
            }
            Binding::StripSource(..) => return Err(SetError::ReadOnly),
            Binding::RecOut(port, number) => {
                let (cur_g, cur_n) = self.rec_tap_pair(row, port as usize);
                let (g, k) = if number { (cur_g.to_owned(), v.as_i32().unwrap_or(1)) } else { (str_of(v), cur_n) };
                let tap = self.rec_tap_of(&g, k);
                self.engine.usb().set_tap(port as usize, tap.unwrap_or(RecTap::Off));
                // Keep the pair as set (the number of an OFF port, a source Sidecar can't record).
                for (name, val) in [("grp", Value::Str(g)), ("in", Value::Int(k))] {
                    if let Some(r) = self.sibling(row, name) {
                        self.state.shadow_set(ShadowKey::Row(r), val);
                    }
                }
            }
            Binding::Dca(i, field) => {
                let d = c.dcas.get(i as usize).ok_or(SetError::NotFound)?;
                match field {
                    DcaF::Name => e.set_dca_name(i as usize, &str_of(v)),
                    DcaF::Colour => d.colour.store(v.as_i32().unwrap_or(1), Relaxed),
                    DcaF::Icon => d.icon.store(v.as_i32().unwrap_or(0), Relaxed),
                    DcaF::Led => set_b(&d.led, v),
                    DcaF::Mute => set_b(&d.mute, v),
                    DcaF::Fader => set_lvl(&d.fader_db, v),
                    DcaF::Solo => set_b(&d.solo, v),
                    DcaF::SoloLed => return Err(SetError::ReadOnly),
                    DcaF::Mon => set_item(&d.mon, def, v),
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
                    // Through the host's SOLO, as on the surface (so CLR SOLO ends it).
                    let m = &c.monitor;
                    match m.host().and_then(|(kind, i)| self.strip(kind, i as u16 + 1)) {
                        Some(host) => {
                            host.params.solo.store(v.as_bool(), Relaxed);
                            m.source_solo_on.store(false, Relaxed);
                        }
                        None => m.source_solo_on.store(v.as_bool(), Relaxed),
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
                        None => return Ok(false), // PLAY: no Sidecar source
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
                    RtaF::Source => return Err(SetError::ReadOnly),
                }
            }
            Binding::Source(group, n, sf) => {
                let index = source_of(group, n).ok_or(SetError::NotFound)?;
                let table = e.sources();
                let p = table.get(index).ok_or(SetError::NotFound)?;
                match sf {
                    // In the group's range and steps (LOCAL IN shares A's ports).
                    SrcF::Gain => table.set_gain(index, v.as_f32().unwrap_or(0.0)),
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
                    SrcF::MuteState | SrcF::Mode => return Err(SetError::ReadOnly),
                }
            }
            Binding::UserSrc(i, uf) => match e.sources().user_signals().get(i as usize) {
                Some(u) => match uf {
                    UsrF::Grp => set_item(&u.group, def, v),
                    UsrF::In => u.number.store(v.as_i32().unwrap_or(1).max(1), Relaxed),
                    UsrF::Lr => set_item(&u.side, def, v),
                    UsrF::Tap => u.post.store(value::enum_index(def, v) == Some(1), Relaxed),
                },
                None => {
                    let n = i as usize - sources::NUM_USER_SIGNALS;
                    let (cur_g, cur_n) = self.user_patch_pair(row, n);
                    let (g, k) = if uf == UsrF::In { (cur_g.to_owned(), v.as_i32().unwrap_or(1)) } else { (str_of(v), cur_n) };
                    e.sources().set_user_patch(n, port::wing_to_source(&g, k).unwrap_or(-1));
                    // Keep the pair as set (an OFF patch's number, LCL n beside A n).
                    for (name, val) in [("grp", Value::Str(g)), ("in", Value::Int(k))] {
                        if let Some(r) = self.sibling(row, name) {
                            self.state.shadow_set(ShadowKey::Row(r), val);
                        }
                    }
                }
            },
            Binding::RtaShown(_) | Binding::FxAssign(..) => return Err(SetError::ReadOnly),
            Binding::Osc(n, of) => {
                let o = e.sources().osc(n as usize).ok_or(SetError::NotFound)?;
                match of {
                    OscF::Level => set_f(&o.level_db, v),
                    OscF::Mode => set_item(&o.mode, def, v),
                    OscF::Freq => set_f(&o.freq_hz, v),
                }
            }
            Binding::Play(pf) => self.set_play(def, pf, v)?,
            Binding::Rec(rf) => self.set_rec(def, rf, v)?,
            Binding::Safe(sf) => self.set_safes(sf, &str_of(v)),
            Binding::Lib(lf) => {
                let lib_index = &self.state.lib_index;
                match lf {
                    LibF::ActionIndex => lib_index.store(v.as_i32().unwrap_or(0), Relaxed),
                    LibF::Action => {
                        let lib = self.state.library();
                        let i = value::enum_index(def, v).ok_or(SetError::BadValue)?;
                        let at = lib_index.load(Relaxed);
                        let action = match def.items().get(i).map_or("IDLE", |it| it.item) {
                            "IDLE" => None,
                            // PREV / NEXT move the selection only, as the LIBRARY's keys.
                            "PREV" => {
                                lib_index.store((at - 1).max(0), Relaxed);
                                None
                            }
                            "NEXT" => {
                                lib_index.store((at + 1).min((lib.num_scenes - 1).max(0)), Relaxed);
                                None
                            }
                            "GO" => Some(LibAction::Go(at)),
                            "GONEXT" => Some(LibAction::GoNext),
                            "GOPREV" => Some(LibAction::GoPrev),
                            _ => Some(LibAction::GoTag(at)),
                        };
                        if let Some(a) = action {
                            if !lib.attached {
                                return Err(SetError::Unsupported("no show to recall scenes from".into()));
                            }
                            self.state.push_lib_action(a);
                        }
                    }
                    _ => return Err(SetError::ReadOnly),
                }
            }
            Binding::Layer(..) | Binding::User(_) => {
                let mut sf = self.state.surface();
                if !sf.attached {
                    return Ok(false);
                }
                let edit = match bindings()[row as usize] {
                    Binding::Layer(s, lf) => sf.set_layer(s, lf, v)?,
                    Binding::User(uf) => sf.set_user(uf, v)?,
                    _ => None,
                };
                if let Some(edit) = edit {
                    self.state.push_surface_edit(edit);
                }
            }
            Binding::Info(InfoF::Name) => {
                self.state.identity.lock().unwrap_or_else(|e| e.into_inner()).name = str_of(v);
            }
            Binding::Info(_) => return Err(SetError::ReadOnly),
            Binding::Out(block, n, number) => {
                let port = port::output_port(block as usize, n as i32);
                let (cur_g, cur_n) = self.out_pair(row, port);
                let (cur_g, cur_n) = (cur_g.to_owned(), cur_n);
                let (g, k) = if number { (cur_g, v.as_i32().unwrap_or(1)) } else { (str_of(v), cur_n) };
                self.route_output(port, &g, k);
                // Remember the pair as set: the number of an unrouted output, and
                // sources Sidecar can't route.
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

    /// What RECORDER port `port` records, as the WING's (group, number):
    /// a bus, main or matrix signal or a source; else the pair as set (OFF
    /// with its number, or a signal Sidecar can't record).
    fn rec_tap_pair(&self, row: u16, port: usize) -> (&'static str, i32) {
        let shown = match self.engine.usb().tap(port) {
            RecTap::Off => None,
            RecTap::Source(src) => port::source_to_wing(src).filter(|(g, _)| *g != "OFF"),
            RecTap::Strip { kind, number, channel } => {
                let group = match StripKind::from_i32(kind) {
                    Some(StripKind::Bus) => Some("BUS"),
                    Some(StripKind::Main) => Some("MAIN"),
                    Some(StripKind::Matrix) => Some("MTX"),
                    _ => None, // a channel's direct out: no WING group
                };
                group.map(|g| (g, 2 * (number - 1) + channel as i32 + 1))
            }
        };
        match (shown, self.shadow_pair(row, "grp", "in")) {
            (Some(gk), _) => gk,
            (None, Some((g, k))) => {
                let unrecordable = def_items_find(self.sibling(row, "grp"), &g).filter(|g| self.rec_tap_of(g, k).is_none());
                (unrecordable.unwrap_or("OFF"), k)
            }
            (None, None) => ("OFF", 1),
        }
    }

    /// The RECORDER tap of the WING's (group, number); None: OFF, or nothing Sidecar can record.
    fn rec_tap_of(&self, group: &str, n: i32) -> Option<RecTap> {
        let strip = |kind: StripKind| {
            let number = (n + 1) / 2;
            self.strip(kind, u16::try_from(number).ok()?)?;
            Some(RecTap::Strip { kind: kind as i32, number, channel: ((n - 1) % 2).unsigned_abs() as usize })
        };
        match group {
            "OFF" => None,
            "BUS" => strip(StripKind::Bus),
            "MAIN" => strip(StripKind::Main),
            "MTX" => strip(StripKind::Matrix),
            g => port::wing_to_source(g, n).filter(|p| *p >= 0).map(RecTap::Source),
        }
    }

    /// What feeds output `port` as its `/io/out/../grp`, `in` (`row`, either)
    /// show it: a source as set while it names the routed one (LCL 5 is A 5
    /// here, SC 7 is port 7), else what Sidecar routes there; a source
    /// Sidecar can't route as set; else OFF with the number set. Nothing set
    /// and nothing routed: OFF, not the factory WING's patch (LCL 3 BUS 5 and
    /// A 3 OFF are the same port here, and the engine doesn't route it); WING
    /// Edit writes both back on PC -> MIXER, so they must agree.
    fn out_pair(&self, row: u16, port: i32) -> (&'static str, i32) {
        match (self.output_source(port), self.shadow_pair(row, "grp", "in")) {
            (Some(gk), Some((sg, sk)))
                if unroutable_group(row, &sg).is_some()
                    && port::wing_to_source(&sg, sk).is_some_and(|s| s >= 0 && port::wing_to_source(gk.0, gk.1) == Some(s)) =>
            {
                (unroutable_group(row, &sg).unwrap_or(gk.0), sk)
            }
            (Some(gk), _) => gk,
            (None, Some((sg, sk))) => (unroutable_group(row, &sg).unwrap_or("OFF"), sk),
            (None, None) => ("OFF", 1),
        }
    }

    /// What user patch `n` (0-based) copies as the WING's (group, number):
    /// the pair as set while it still names the patched source (LCL 5 and
    /// A 5 are one port here), else the source's own name; OFF keeps the
    /// number set.
    fn user_patch_pair(&self, row: u16, n: usize) -> (&'static str, i32) {
        let src = self.engine.sources().user_patches().get(n).map_or(-1, |p| p.source.load(Relaxed));
        let shadow =
            self.shadow_pair(row, "grp", "in").and_then(|(g, k)| def_items_find(self.sibling(row, "grp"), &g).map(|g| (g, k)));
        match (shadow, port::source_to_wing(src)) {
            (Some((g, k)), _) if src >= 0 && port::wing_to_source(g, k) == Some(src) => (g, k),
            (Some((_, k)), _) if src < 0 => ("OFF", k),
            (_, Some(gk)) => gk,
            _ => ("OFF", 1),
        }
    }

    fn shadow_pair(&self, row: u16, gname: &str, nname: &str) -> Option<(String, i32)> {
        let g = self.sibling(row, gname).and_then(|r| self.state.shadow_get(ShadowKey::Row(r)))?;
        let n = self.sibling(row, nname).and_then(|r| self.state.shadow_get(ShadowKey::Row(r)))?;
        Some((str_of(&g), n.as_i32().unwrap_or(1)))
    }

    /// Routes `group`/`n` (BUS, MAIN, MTX: 2k-1 = left of strip k, 2k = right;
    /// MON 1-4; any input group's source) to hardware output `port`. False if
    /// Sidecar can't.
    fn route_output(&self, port: i32, group: &str, n: i32) -> bool {
        let c = &self.engine.controls;
        let target: Option<(&Arc<Strip>, bool)> = match group {
            "BUS" => self.buses.get(((n - 1) / 2) as usize).and_then(|b| b.as_ref()).map(|s| (s, n % 2 == 0)),
            "MAIN" => self.mains.get(((n - 1) / 2) as usize).map(|s| (s, n % 2 == 0)),
            "MTX" => self.matrices.get(((n - 1) / 2) as usize).map(|s| (s, n % 2 == 0)),
            _ => None,
        };
        // FX SEND n: FX slot (n+1)/2's outboard insert send (manual 11.11 p. 164).
        let send = (group == "SEND").then(|| c.fx.get(((n - 1) / 2) as usize)).flatten().filter(|_| n >= 1);
        // Any source Sidecar has (manual 5.4 OUTPUTS: "route any Source to
        // any digital or analog output"): a port, a playback channel, the USB
        // player, an oscillator, a user signal or patch.
        let source = match (group, target) {
            ("BUS" | "MAIN" | "MTX" | "MON" | "OFF" | "SEND", _) | (_, Some(_)) => None,
            _ => port::wing_to_source(group, n).filter(|s| *s >= 0),
        };
        let known =
            target.is_some() || source.is_some() || send.is_some() || group == "OFF" || (group == "MON" && (1..=4).contains(&n));
        if !known {
            return false;
        }
        // One source per output, as on the console (an FX SEND too).
        self.engine.clear_output(port);
        match (group, target, source) {
            (_, Some((s, right)), _) => {
                if right { &s.params.output_right } else { &s.params.output_left }.store(port, Relaxed);
            }
            (_, _, Some(src)) => {
                self.engine.route_output_source(port, src);
            }
            ("MON", None, None) => monitor_output(c, n).store(port, Relaxed),
            _ => {
                if let Some(f) = send {
                    if n % 2 == 0 { &f.send_right } else { &f.send_left }.store(port, Relaxed);
                }
            }
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
        // Sidecar's EFFECTS screen); a loaded plugin isn't replaced remotely.
        if current == FX_EXT && self.engine.fx(i).is_some_and(|r| r.is_loaded()) {
            return Err(SetError::Unsupported(format!("FX {} holds a plugin", i + 1)));
        }
        if !allowed_in_slot(model, i) {
            return Err(SetError::Unsupported(format!("{} needs a premium slot (FX 1-8)", FX_MODELS[model].name)));
        }
        // The message thread builds the device (ld_engine_collect).
        p.set_model(model, true);
        if model == FX_EXT {
            // The WING's EXTERNAL is the outboard insert (manual 11.11 p. 164);
            // plugins are loaded on Sidecar's EFFECTS screen.
            p.values[ext::SOURCE].store(ext::SOURCE_OUTBOARD);
        }
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
            SF::CustLink => set_b(&p.cust_link, v),
            SF::Mute => set_b(&p.mute, v),
            SF::Fader => set_lvl(&p.fader_db, v),
            SF::Pan => set_f(&p.pan, v),
            SF::Width => set_f(&p.width, v),
            SF::Solo => set_b(&p.solo, v),
            SF::SoloSafe => set_b(&p.solo_safe, v),
            // As the MUTE key: the override holds only while a group, DCA or
            // SIP mutes the strip and MUTEGROUP/SIP OVERRIDE is on.
            SF::MuteOverride => set_b(&s.console.mute_override, v),
            SF::Mon => set_item(&p.monitor_bus, def, v),
            SF::Proc => set_item(&p.process_order, def, v),
            SF::Tap => set_item(&p.tap, def, v),
            SF::Trim => set_f(&p.input.trim_db, v),
            SF::Bal => set_f(&p.input.balance_db, v),
            SF::Inv => set_b(&p.input.invert, v),
            SF::DlyOn => set_b(&p.input.delay_on, v),
            SF::DlyMode => set_item(&p.input.delay_mode, def, v),
            SF::Dly => {
                // 0.1 .. 150 m in the unit set (SAMPLES from 16), as the WING.
                let rate = self.engine.sample_rate();
                let rate = if rate > 0.0 { rate } else { 48000.0 };
                let (lo, hi) = delay_range(p.input.delay_mode.load(Relaxed).clamp(0, 3) as usize, rate);
                if let Some(x) = v.as_f32() {
                    p.input.delay.store(x.clamp(lo, hi));
                }
            }
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
                        // Input channels key from channels, the others from
                        // buses, mains, matrices and aux (keytap.rs).
                        match KeySource::from_wing_name(&str_of(v)) {
                            Some(src @ KeySource::SelfKey) => d.sc_source.store(src.code(), Relaxed),
                            Some(src @ KeySource::Channel(_)) if kind == StripKind::Channel => {
                                d.sc_source.store(src.code(), Relaxed)
                            }
                            Some(src @ KeySource::Strip(..)) if kind != StripKind::Channel => {
                                d.sc_source.store(src.code(), Relaxed)
                            }
                            _ => return Ok(false),
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
                    InsF::Stat => return Err(SetError::ReadOnly),
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
                    SendF::Plink => set_plink(&send.pan_link, &send.pan_reversed, v),
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
                    SendF::Plink => set_plink(&m.pan_link, &m.pan_reversed, v),
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
            SF::DirIn => set_item(&p.direct_source, def, v),
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
            SF::Listen(mode) => {
                // As the LISTEN key: one strip and block at a time.
                let a = self.engine.analysis().controls();
                if v.as_bool() {
                    let band = match self.sibling(row, "$solobd").filter(|_| mode == listen_mode::BAND) {
                        Some(r) => self.get(Node::Row(r)).and_then(|b| b.as_i32()).unwrap_or(1) - 1,
                        None => 0,
                    };
                    a.listen_kind.store(kind as i32, Relaxed);
                    a.listen_id.store(s.id(), Relaxed);
                    a.listen_band.store(band.max(0), Relaxed);
                    a.listen_mode.store(mode, Relaxed);
                } else if self.listening(kind, s, mode) {
                    a.listen_mode.store(listen_mode::OFF, Relaxed);
                }
            }
            SF::ListenBand => {
                let band = v.as_i32().unwrap_or(1);
                self.state.shadow_set(ShadowKey::Row(row), Value::Int(band));
                if self.listening(kind, s, listen_mode::BAND) {
                    self.engine.analysis().controls().listen_band.store(band - 1, Relaxed);
                }
            }
            SF::SrcGain | SF::SrcPhantom => {
                let Some(i) = self.strip_source(kind, n, s) else { return Ok(false) };
                let Some(p) = self.engine.sources().get(i) else { return Ok(false) };
                if field == SF::SrcGain {
                    // The patched source's range and steps.
                    self.engine.sources().set_gain(i, v.as_f32().unwrap_or(0.0))
                } else {
                    set_b(&p.phantom, v)
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

/// The item text of an enum value.
fn item_name(def: &Def, v: &Value) -> Result<&'static str, SetError> {
    let i = value::enum_index(def, v).ok_or(SetError::BadValue)?;
    def.items().get(i).map(|it| it.item).ok_or(SetError::BadValue)
}

/// Frames as seconds (0.1 s steps, so a poll doesn't push every block).
fn seconds(frames: i64, rate: f64) -> f32 {
    if rate <= 0.0 {
        return 0.0;
    }
    ((frames as f64 / rate * 10.0).floor() / 10.0) as f32
}

fn str_of(v: &Value) -> String {
    match v {
        Value::Str(s) => s.clone(),
        Value::Int(i) => i.to_string(),
        Value::Float(f) => f.to_string(),
    }
}

/// A shadowed group of `row`'s (grp, in) pair that is an input group, not
/// the strips', the monitor's or OFF (those always show the engine's state:
/// a stale shadow must not show); the caller checks it still names the
/// routed source.
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
    /// Of the mapped: the host's settings, and the stored values of
    /// parameters without a Sidecar equivalent (the WING's factory value).
    pub settings: usize,
    pub stored: usize,
}

impl Coverage {
    pub fn report(&self) -> String {
        let mut out = format!(
            "WING tree coverage: {} of {} parameters map to Sidecar ({:.1} %)\n  \
             {} of them Sidecar's settings, {} stored values for what Sidecar has no equivalent of\n",
            self.mapped,
            self.total,
            100.0 * self.mapped as f64 / self.total.max(1) as f64,
            self.settings,
            self.stored
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
        // The sweep's custom control function rows are the slots' parameters now.
        if !def.is_leaf() || s.is_detached(r as u16) {
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
        match b[r] {
            Binding::Stored => cov.stored += 1,
            Binding::Host(_) => cov.settings += 1,
            _ => {}
        }
    }
    for (si, slot) in s.slots.iter().enumerate() {
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
        // A custom control's function nodes answer from the show's custom
        // controls (the GPIOs' keep what a client sets).
        let mode = s.cc_mode_row(si as u16).map(|r| b[r as usize]);
        let custom_control = matches!(mode, Some(Binding::User(_)));
        let stored = mode == Some(Binding::Stored);
        for m in &s.families[slot.family as usize].models {
            if custom_control || stored {
                for _ in &m.params {
                    let e = cov.by_group.entry(key.clone()).or_default();
                    e.0 += 1;
                    e.1 += 1;
                    cov.total += 1;
                    cov.mapped += 1;
                    cov.stored += stored as usize;
                }
                continue;
            }
            let spec = tables.and_then(|t| model_index(t, m.token).map(|i| &t[i]));
            for p in &m.params {
                let e = cov.by_group.entry(key.clone()).or_default();
                e.1 += 1;
                cov.total += 1;
                // A parameter our model doesn't have (a page selector) keeps
                // what a client sets.
                let stored = spec.is_some_and(|sp| sp.param_index(p.name).is_none());
                if spec.is_some() {
                    e.0 += 1;
                    cov.mapped += 1;
                }
                if stored {
                    cov.stored += 1;
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
            ("/ch/3/gatesc/tap", Value::str("POST")),
            ("/ch/3/dynsc/tap", Value::str("PFL")),
            ("/ch/3/dynsc/src", Value::str("CH.5")),
            ("/bus/4/dynsc/src", Value::str("MAIN.2")),
            ("/bus/4/dynsc/tap", Value::str("EQ")),
            ("/aux/2/dynsc/src", Value::str("AUX.3")),
            ("/mtx/2/dynsc/src", Value::str("BUS.16")),
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
            ("/aux/2/solosafe", Value::Int(1)),
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
            // Floats land on the parameter's nearest step (the LC's 120 Hz near 120).
            let want = value::quantise(c.def(n).unwrap(), v);
            let got = c.set(n, v).unwrap_or_else(|e| panic!("{path}: {e}"));
            assert_eq!(got, want, "{path}");
            assert_eq!(c.get(n), Some(want), "{path}");
        }
        // The engine saw it.
        let ch3 = c.strip(StripKind::Channel, 3).unwrap();
        assert_eq!(ch3.params.fader_db.load(), -6.5);
        assert_eq!(ch3.params.dca_mask.load(Relaxed), 1);
        assert_eq!(ch3.params.mute_group_mask.load(Relaxed), 4);
        assert_eq!(ch3.name(), "Kick");
        // KEY TAP (TAP_POINTS order).
        assert_eq!((ch3.params.gate.sc_tap.load(Relaxed), ch3.params.dynamics.sc_tap.load(Relaxed)), (7, 5));
        // KEY SOURCE: channels from channels, the others from buses, mains, matrices and aux.
        assert_eq!(ch3.params.dynamics.sc_source.load(Relaxed), 4);
        let bus4 = c.strip(StripKind::Bus, 4).unwrap();
        assert_eq!(KeySource::from_code(bus4.params.dynamics.sc_source.load(Relaxed)), KeySource::Strip(StripKind::Main, 1));
        assert_eq!(bus4.params.dynamics.sc_tap.load(Relaxed), 4);
        assert!(c.strip(StripKind::Aux, 2).unwrap().params.solo_safe.load(Relaxed), "SOLO SAFE on an aux");
        let mtx2 = c.strip(StripKind::Matrix, 2).unwrap();
        assert_eq!(KeySource::from_code(mtx2.params.dynamics.sc_source.load(Relaxed)), KeySource::Strip(StripKind::Bus, 15));
        assert!(!c.set(node(&c, "/bus/4/dynsc/src"), &Value::str("CH.1")).is_ok_and(|v| v == Value::str("CH.1")));
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
        // On the nearest of its 101 steps, as the console keeps it.
        let Some(Value::Float(got)) = c.get(dcy) else { panic!() };
        assert!(got != 3.0 && (got - 3.0).abs() < 0.03, "{got}");
        assert_eq!(e.controls.fx[0].param("dcy").unwrap().load(), got);
        // Premium devices only in slots 1-8.
        assert!(matches!(c.set(node(&c, "/fx/12/mdl"), &Value::str("HALL")), Err(SetError::Unsupported(_) | SetError::BadValue)));
    }

    /// The routing nodes: USR sources and what they copy, sources on outputs,
    /// matrix `dir/in`, sends between any two buses.
    #[test]
    fn user_signals_patches_outputs_direct_in_and_bus_sends() {
        let e = engine();
        let st = WingState::default();
        let c = Console::new(&e, &st);
        let set = |p: &str, v: Value| c.set(node(&c, p), &v).unwrap();
        let get = |p: &str| c.get(node(&c, p)).unwrap();
        let t = e.sources();
        // USR 2: Bus 12 R, POST.
        set("/io/in/USR/2/user/grp", Value::str("BUS"));
        set("/io/in/USR/2/user/in", Value::Int(12));
        set("/io/in/USR/2/user/lr", Value::str("R"));
        set("/io/in/USR/2/user/tap", Value::str("POST"));
        let u = &t.user_signals()[1];
        assert_eq!(u.strip(), Some((StripKind::Bus, 12)));
        assert!(u.post.load(Relaxed) && u.side.load(Relaxed) == sources::user_side::RIGHT);
        assert_eq!((get("/io/in/USR/2/user/grp"), get("/io/in/USR/2/user/tap")), (Value::str("BUS"), Value::str("POST")));
        // Its source settings: pairing (which copies USR 1's settings), name, polarity.
        set("/io/in/USR/1/mode", Value::str("ST"));
        set("/io/in/USR/2/name", Value::str("B12 R"));
        set("/io/in/USR/2/pol", Value::Int(1));
        assert_eq!(t.name(sources::USR_SOURCE_INDEX + 1), "B12 R");
        assert!(t.get(sources::USR_SOURCE_INDEX + 1).unwrap().invert.load(Relaxed));
        assert_eq!(t.get(sources::USR_SOURCE_INDEX).unwrap().mode.load(Relaxed), source_mode::STEREO);
        assert!(c.is_mapped(node(&c, "/io/in/USR/56/user/grp")));
        assert!(schema().resolve("/io/in/USR/30/user/lr", &NoModels).is_none());
        // USR 26: LCL 5 (port 5, shown as set); CRD 3 = playback 3.
        set("/io/in/USR/26/user/grp", Value::str("LCL"));
        set("/io/in/USR/26/user/in", Value::Int(5));
        assert_eq!(t.user_patches()[1].source.load(Relaxed), 4);
        assert_eq!((get("/io/in/USR/26/user/grp"), get("/io/in/USR/26/user/in")), (Value::str("LCL"), Value::Int(5)));
        set("/io/in/USR/27/user/grp", Value::str("CRD"));
        set("/io/in/USR/27/user/in", Value::Int(3));
        assert_eq!(t.user_patches()[2].source.load(Relaxed), PLAYBACK_SOURCE_BASE + 2);
        t.set_user_patch(2, 50); // changed elsewhere: shows the source's own name
        assert_eq!((get("/io/in/USR/27/user/grp"), get("/io/in/USR/27/user/in")), (Value::str("B"), Value::Int(3)));
        set("/io/in/USR/27/user/grp", Value::str("OFF"));
        assert_eq!(t.user_patches()[2].source.load(Relaxed), -1);

        // Outputs take any source (manual 5.4): A 3 <- SC 7 (port 7, shown as
        // set), A 4 <- OSC 2, A 5 <- USR 2; then BUS 3 replaces SC 7.
        set("/io/out/A/3/grp", Value::str("SC"));
        set("/io/out/A/3/in", Value::Int(7));
        assert_eq!(e.output_patch().get(2), 6);
        assert_eq!((get("/io/out/A/3/grp"), get("/io/out/A/3/in")), (Value::str("SC"), Value::Int(7)));
        set("/io/out/A/4/in", Value::Int(2));
        set("/io/out/A/4/grp", Value::str("OSC"));
        assert_eq!(e.output_patch().get(3), sources::OSC_SOURCE_BASE + 1);
        set("/io/out/A/5/in", Value::Int(2));
        set("/io/out/A/5/grp", Value::str("USR"));
        assert_eq!(e.output_patch().get(4), sources::USR_SOURCE_BASE + 1);
        e.output_patch().set(4, sources::USR_SOURCE_BASE + 5); // set elsewhere
        assert_eq!((get("/io/out/A/5/grp"), get("/io/out/A/5/in")), (Value::str("USR"), Value::Int(6)));
        set("/io/out/A/3/grp", Value::str("BUS"));
        assert_eq!(e.output_patch().get(2), -1);
        assert_eq!(c.strip(StripKind::Bus, 4).unwrap().params.output_left.load(Relaxed), 2, "BUS 7 = Bus 4 L");
        set("/io/out/A/3/grp", Value::str("OFF"));
        assert_eq!(c.strip(StripKind::Bus, 4).unwrap().params.output_left.load(Relaxed), -1);

        // Matrix DIRECT IN.
        set("/mtx/3/dir/in", Value::str("MON.SPK"));
        let m3 = c.strip(StripKind::Matrix, 3).unwrap();
        assert_eq!(m3.params.direct_source.load(Relaxed), livedaw_engine::routing::matrix_direct::SPEAKERS);
        assert_eq!(get("/mtx/3/dir/in"), Value::str("MON.SPK"));

        // Bus 3 -> Bus 12 and Bus 16 -> Bus 1 (any bus to any other).
        set("/bus/3/send/12/on", Value::Int(1));
        set("/bus/3/send/12/lvl", Value::Float(-4.0));
        set("/bus/16/send/1/pre", Value::Int(1));
        let b3 = c.strip(StripKind::Bus, 3).unwrap();
        let send = b3.sends.find(11).unwrap();
        assert!(send.enabled.load(Relaxed) && send.level_db.load() == -4.0);
        let b16 = c.strip(StripKind::Bus, 16).unwrap();
        assert_eq!(b16.sends.find(0).unwrap().mode.load(Relaxed), send_mode::PRE);
        assert_eq!(c.binding(node(&c, "/bus/5/send/5/on")), Binding::Stored, "not to itself");
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
        // A bus as a source (manual 2.6): BUS 4 (the number kept) = Bus 2 R; USR 3 a user signal.
        c.set(node(&c, "/ch/4/in/conn/grp"), &Value::str("BUS")).unwrap();
        assert_eq!(c.get(node(&c, "/ch/4/in/conn/grp")), Some(Value::str("BUS")));
        let input = |n: u16| c.strip(StripKind::Channel, n).unwrap().params.input_left.load(Relaxed);
        assert_eq!(input(4), livedaw_engine::routing::strip_source_input(StripKind::Bus, 1, 1));
        c.set(node(&c, "/ch/4/in/conn/in"), &Value::Int(16)).unwrap();
        assert_eq!(input(4), livedaw_engine::routing::strip_source_input(StripKind::Bus, 7, 1));
        c.set(node(&c, "/ch/5/in/conn/grp"), &Value::str("MTX")).unwrap();
        c.set(node(&c, "/ch/5/in/conn/in"), &Value::Int(3)).unwrap();
        assert_eq!(input(5), livedaw_engine::routing::strip_source_input(StripKind::Matrix, 1, 0));
        assert_eq!(c.get(node(&c, "/ch/5/in/conn/in")), Some(Value::Int(3)));
        c.set(node(&c, "/ch/6/in/conn/grp"), &Value::str("USR")).unwrap();
        c.set(node(&c, "/ch/6/in/conn/in"), &Value::Int(3)).unwrap();
        assert_eq!(input(6), sources::USR_SOURCE_BASE + 2);
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
    fn the_console_model_is_in_the_identity() {
        let e = engine();
        let st = WingState::default();
        let c = Console::new(&e, &st);
        let model = node(&c, "/$syscfg/$cnsmdl");
        // The full-size console reports the WING's own model.
        assert_eq!(c.get(model), Some(Value::str("ngc-full")));
        let config = node(&c, "/$syscfg/$cnscfg");
        assert_eq!(c.get(config), Some(Value::str("wing-fullsize")));
        st.set_console_model(ConsoleModel::Compact);
        assert_eq!(c.get(model), Some(Value::str("wing-compact")));
        // WING Edit takes its model from the build type.
        assert_eq!(c.get(config), Some(Value::str("wing-compact")));
        assert_eq!(st.identity().model, "wing-compact");
        st.set_console_model(ConsoleModel::Full);
        assert_eq!(st.identity(), Identity::default());
        assert_eq!(Identity::of_model(ConsoleModel::Compact).model, "wing-compact");
        assert_eq!(Identity::of_model(ConsoleModel::Full), Identity::default());

        // The black full-size WING is "wing-bk" in both; the COMPACT stays itself.
        st.set_black_finish(true);
        assert_eq!((c.get(model), c.get(config)), (Some(Value::str("wing-bk")), Some(Value::str("wing-bk"))));
        st.set_console_model(ConsoleModel::Compact);
        assert_eq!((c.get(model), c.get(config)), (Some(Value::str("wing-compact")), Some(Value::str("wing-compact"))));
        st.set_console_model(ConsoleModel::Full);
        assert_eq!(st.identity().model, "wing-bk");
        assert_eq!(Identity::of_console(ConsoleModel::Full, true).config, "wing-bk");
        assert_eq!(Identity::of_console(ConsoleModel::Compact, true), Identity::of_model(ConsoleModel::Compact));
        st.set_black_finish(false);
        assert_eq!(st.identity(), Identity::default());
    }

    #[test]
    fn unmapped_nodes_keep_their_values() {
        let e = engine();
        let st = WingState::default();
        let c = Console::new(&e, &st);
        let speed = node(&c, "/$ctl/$globals/mousespd"); // Sidecar has no mouse speed
        assert!(!c.is_mapped(speed));
        assert_eq!(c.binding(speed), Binding::Stored);
        assert_eq!(c.get(speed), Some(Value::Float(1.77)), "the WING's factory value");
        c.set(speed, &Value::Float(1.234)).unwrap();
        // On the parameter's steps (0.01).
        let Some(Value::Float(got)) = c.get(speed) else { panic!() };
        assert!((got - 1.23).abs() < 1e-4, "{got}");
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
        assert!(!c.is_mapped(node(&c, "/cfg/rta/rtatap")), "the WING's taps aren't Sidecar's");
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
        // On their steps, as the console reads them back (Q 2.00 is 1.995882).
        assert!((p.tap_eq.bands[2].freq.load() - 8000.0).abs() < 30.0);
        assert!((p.tap_eq.bands[0].q.load() - 1.995882).abs() < 1e-5);
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
        assert_eq!(phones.eq[3].gain_db.load(), 5.0);
        assert!((phones.eq[3].q.load() - 3.0).abs() < 0.03);
        assert!((phones.eq[0].freq.load() - 120.0).abs() < 0.5);
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

        // The MONITOR outputs: MON 1/2 the PHONES, 3/4 the SPEAKERS.
        c.set(node(&c, "/io/out/A/9/in"), &Value::Int(2)).unwrap();
        c.set(node(&c, "/io/out/A/9/grp"), &Value::str("MON")).unwrap();
        c.set(node(&c, "/io/out/A/10/in"), &Value::Int(3)).unwrap();
        c.set(node(&c, "/io/out/A/10/grp"), &Value::str("MON")).unwrap();
        assert_eq!(ctl.phones.output_right.load(Relaxed), 8);
        assert_eq!(ctl.monitor_output_left.load(Relaxed), 9);
        assert_eq!(c.get(node(&c, "/io/out/A/9/in")), Some(Value::Int(2)));
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
        // GAIN in the group's range and steps: LOCAL IN (= A 1-48 here) -2.5 ..
        // +45 dB in 2.5 dB steps, AES50 B -3 .. +45.5 dB in 0.5 dB steps.
        for (path, set, want) in [
            ("/io/in/LCL/2/g", 7.0, 7.5),
            ("/io/in/LCL/2/g", 60.0, 45.0),
            ("/io/in/LCL/2/g", -12.0, -2.5),
            ("/io/in/A/2/g", 6.5, 7.5),
            ("/io/in/B/3/g", 6.5, 6.5),
            ("/io/in/B/3/g", 60.0, 45.5),
            ("/io/in/B/3/g", -12.0, -3.0),
        ] {
            c.set(node(&c, path), &Value::Float(set)).unwrap();
            assert_eq!(c.get(node(&c, path)), Some(Value::Float(want)), "{path} {set}");
        }
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
        // LOCAL IN is A here: the same sources.
        assert!(c.is_mapped(node(&c, "/io/in/LCL/5/name")));
        assert_eq!(c.get(node(&c, "/io/in/LCL/5/name")), Some(Value::str("Snare")));
        // Groups Sidecar doesn't have only share port numbers: their settings are stored.
        assert!(!c.is_mapped(node(&c, "/io/in/USB/5/name")));
        assert_eq!(c.binding(node(&c, "/io/in/USB/5/name")), Binding::Stored);
        // MODE pairs Sidecar's sources.
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
        let aux7 = c.strip(StripKind::Aux, 7).unwrap();
        assert!(aux7.params.solo.load(Relaxed), "through the host's SOLO");
        assert_eq!(c.get(node(&c, "/cfg/solo/$srcsgrp")), Some(Value::Int(3)));
        assert_eq!(c.get(node(&c, "/cfg/solo/$srcsin")), Some(Value::Int(5)));
        // Released through the host; the flag alone (no host chosen yet) reads too.
        c.set(node(&c, "/cfg/solo/$srcsolo"), &Value::Int(0)).unwrap();
        assert!(!aux7.params.solo.load(Relaxed));
        assert_eq!(c.get(node(&c, "/cfg/solo/$srcsolo")), Some(Value::Int(0)));
        m.source_solo_on.store(true, Relaxed);
        assert_eq!(c.get(node(&c, "/cfg/solo/$srcsolo")), Some(Value::Int(1)));
    }

    #[test]
    fn the_osc_group_is_the_oscillators() {
        let e = engine();
        let st = WingState::default();
        let c = Console::new(&e, &st);
        let table = e.sources();
        c.set(node(&c, "/io/in/OSC/2/osc/mode"), &Value::str("PINK")).unwrap();
        c.set(node(&c, "/io/in/OSC/2/osc/lvl"), &Value::Float(-30.0)).unwrap();
        c.set(node(&c, "/io/in/OSC/2/osc/f"), &Value::Float(250.0)).unwrap();
        c.set(node(&c, "/io/in/OSC/2/mute"), &Value::Int(1)).unwrap();
        c.set(node(&c, "/io/in/OSC/2/name"), &Value::str("Noise")).unwrap();
        let osc = table.osc(1).unwrap();
        assert_eq!((osc.mode.load(Relaxed), osc.level_db.load()), (1, -30.0));
        assert!((osc.freq_hz.load() - 250.0).abs() < 0.5);
        let index = sources::OSC_SOURCE_INDEX + 1;
        assert!(table.get(index).unwrap().mute.load(Relaxed));
        assert_eq!(table.name(index), "Noise");
        assert_eq!(c.get(node(&c, "/io/in/OSC/2/osc/mode")), Some(Value::str("PINK")));
        // On its nearest step, as a factory-set WING answers (999.992).
        let f = c.get(node(&c, "/io/in/OSC/1/osc/f")).and_then(|v| v.as_f32()).unwrap();
        assert!((f - 999.992).abs() < 0.001, "{f}");
        // MODE pairs OSC 1 and 2.
        c.set(node(&c, "/io/in/OSC/1/mode"), &Value::str("ST")).unwrap();
        assert_eq!(table.get(sources::OSC_SOURCE_INDEX).unwrap().mode.load(Relaxed), source_mode::STEREO);
        // Channels patch to it.
        c.set(node(&c, "/ch/3/in/conn/grp"), &Value::str("OSC")).unwrap();
        c.set(node(&c, "/ch/3/in/conn/in"), &Value::Int(2)).unwrap();
        let ch3 = c.strip(StripKind::Channel, 3).unwrap();
        assert_eq!(ch3.params.input_left.load(Relaxed), sources::OSC_SOURCE_BASE + 1);
        assert_eq!(c.get(node(&c, "/ch/3/in/conn/grp")), Some(Value::str("OSC")));
        assert_eq!(c.get(node(&c, "/ch/3/in/conn/in")), Some(Value::Int(2)));
    }

    #[test]
    fn listen_band_key_and_xover_solo() {
        let e = engine();
        let st = WingState::default();
        let c = Console::new(&e, &st);
        let a = e.analysis().controls();
        let ch2 = c.strip(StripKind::Channel, 2).unwrap().clone();
        // BAND SOLO on the band chosen first (1 = the low band).
        c.set(node(&c, "/ch/2/eq/$solobd"), &Value::Int(3)).unwrap();
        assert_eq!(a.listen_mode.load(Relaxed), listen_mode::OFF, "choosing a band doesn't listen");
        c.set(node(&c, "/ch/2/eq/$solo"), &Value::Int(1)).unwrap();
        assert_eq!(a.listen_mode.load(Relaxed), listen_mode::BAND);
        assert_eq!((a.listen_kind.load(Relaxed), a.listen_id.load(Relaxed)), (StripKind::Channel as i32, ch2.id()));
        assert_eq!(a.listen_band.load(Relaxed), 2);
        c.set(node(&c, "/ch/2/eq/$solobd"), &Value::Int(5)).unwrap();
        assert_eq!(a.listen_band.load(Relaxed), 4, "the band moves while listening");
        // The UI's LISTEN reads back.
        a.listen_band.store(1, Relaxed);
        assert_eq!(c.get(node(&c, "/ch/2/eq/$solobd")), Some(Value::Int(2)));
        assert_eq!(c.get(node(&c, "/ch/3/eq/$solo")), Some(Value::Int(0)));
        // One block at a time: KEY SOLO replaces BAND SOLO.
        c.set(node(&c, "/ch/2/gatesc/$solo"), &Value::Int(1)).unwrap();
        assert_eq!(a.listen_mode.load(Relaxed), listen_mode::GATE_KEY);
        assert_eq!(c.get(node(&c, "/ch/2/eq/$solo")), Some(Value::Int(0)));
        c.set(node(&c, "/ch/2/dynsc/$solo"), &Value::Int(1)).unwrap();
        assert_eq!(a.listen_mode.load(Relaxed), listen_mode::DYN_KEY);
        // Releasing another strip's LISTEN changes nothing; this one's ends it.
        c.set(node(&c, "/ch/9/dynsc/$solo"), &Value::Int(0)).unwrap();
        assert_eq!(a.listen_mode.load(Relaxed), listen_mode::DYN_KEY);
        c.set(node(&c, "/ch/2/dynsc/$solo"), &Value::Int(0)).unwrap();
        assert_eq!(a.listen_mode.load(Relaxed), listen_mode::OFF);
        // Buses, mains and matrices: XOVER SOLO and BAND SOLO on the 8-band EQ.
        let bus3 = c.strip(StripKind::Bus, 3).unwrap().clone();
        c.set(node(&c, "/bus/3/dynxo/$solo"), &Value::Int(1)).unwrap();
        assert_eq!(a.listen_mode.load(Relaxed), listen_mode::XOVER);
        assert_eq!((a.listen_kind.load(Relaxed), a.listen_id.load(Relaxed)), (StripKind::Bus as i32, bus3.id()));
        c.set(node(&c, "/mtx/4/eq/$solobd"), &Value::Int(8)).unwrap();
        c.set(node(&c, "/mtx/4/eq/$solo"), &Value::Int(1)).unwrap();
        assert_eq!((a.listen_kind.load(Relaxed), a.listen_band.load(Relaxed)), (StripKind::Matrix as i32, 7));
        assert_eq!(c.get(node(&c, "/mtx/4/eq/$solo")), Some(Value::Int(1)));
        assert!(c.is_mapped(node(&c, "/main/1/dynsc/$solo")) && c.is_mapped(node(&c, "/aux/1/eq/$solo")));
    }

    #[test]
    fn insert_state_and_fx_assignment() {
        let e = engine();
        let st = WingState::default();
        let c = Console::new(&e, &st);
        let stat = |path: &str| c.get(node(&c, path)).unwrap();
        assert_eq!(stat("/ch/5/preins/$stat"), Value::str("-"));
        c.set(node(&c, "/ch/5/preins/ins"), &Value::str("FX3")).unwrap();
        assert_eq!(stat("/ch/5/preins/$stat"), Value::str("N/A"), "FX 3 is empty");
        c.set(node(&c, "/fx/3/mdl"), &Value::str("PLATE")).unwrap();
        assert_eq!(stat("/ch/5/preins/$stat"), Value::str("OK"));
        c.set(node(&c, "/fx/4/mdl"), &Value::str("EXT")).unwrap();
        c.set(node(&c, "/bus/2/postins/ins"), &Value::str("FX4")).unwrap();
        assert_eq!(stat("/bus/2/postins/$stat"), Value::str("N/A"), "EXT without a plugin");
        assert_eq!(c.set(node(&c, "/ch/5/preins/$stat"), &Value::str("OK")), Err(SetError::ReadOnly));
        // Who uses each slot, in the WING's strip numbering (bus 2 = 50).
        assert_eq!((stat("/fx/3/$a_chn"), stat("/fx/3/$a_pos")), (Value::Int(5), Value::Int(0)));
        assert_eq!((stat("/fx/4/$a_chn"), stat("/fx/4/$a_pos")), (Value::Int(50), Value::Int(1)));
        assert_eq!(stat("/fx/9/$a_chn"), Value::Int(0));
        c.set(node(&c, "/mtx/8/preins/ins"), &Value::str("FX9")).unwrap();
        assert_eq!(stat("/fx/9/$a_chn"), Value::Int(76));
    }

    #[test]
    fn ext_is_the_outboard_insert() {
        // Manual 11.11 p. 164: SEND (FX SEND on ROUTING > OUTPUTS), TRIM,
        // FX MODE, RETURN, LATENCY; /fx/N/EXT/* in the remote tree.
        let e = engine();
        let st = WingState::default();
        let c = Console::new(&e, &st);
        let p = &e.controls.fx[1];
        c.set(node(&c, "/fx/2/mdl"), &Value::str("EXT")).unwrap();
        assert!(p.ext_outboard(), "the remote's EXT is the outboard insert");
        for (path, v) in [
            ("/fx/2/egrp", Value::str("A")),
            ("/fx/2/ein", Value::Int(3)),
            ("/fx/2/emode", Value::str("M/S")),
            ("/fx/2/lat", Value::Int(5)),
            ("/fx/2/trim", Value::Float(-3.0)),
        ] {
            c.set(node(&c, path), &v).unwrap();
            assert_eq!(c.get(node(&c, path)), Some(v), "{path}");
            assert!(c.is_mapped(node(&c, path)), "{path}");
        }
        assert_eq!(p.values[ext::GROUP].load(), 3.0);
        assert_eq!(p.values[ext::INPUT].load(), 3.0);
        assert_eq!(p.values[ext::MODE].load(), 2.0);
        assert_eq!(p.values[ext::LATENCY].load(), 5.0);
        assert_eq!(p.values[ext::TRIM].load(), -3.0);
        assert_eq!(livedaw_engine::fx::ext_return_of(p), Some(2));
        assert_eq!(livedaw_engine::fx::ext_return_input("A", 7), port::wing_to_source("A", 7));
        for g in livedaw_engine::fx::EXT_RETURN_GROUPS.into_iter().filter(|g| *g != "OFF") {
            for n in [1, 2, 48, 64] {
                assert_eq!(livedaw_engine::fx::ext_return_input(g, n), port::wing_to_source(g, n), "{g} {n}");
            }
        }

        // SEND: an output patched to FX SEND 3 / 4 (slot 2's left / right).
        c.set(node(&c, "/io/out/A/5/grp"), &Value::str("SEND")).unwrap();
        c.set(node(&c, "/io/out/A/5/in"), &Value::Int(3)).unwrap();
        c.set(node(&c, "/io/out/B/2/grp"), &Value::str("SEND")).unwrap();
        c.set(node(&c, "/io/out/B/2/in"), &Value::Int(4)).unwrap();
        assert_eq!((p.send_left.load(Relaxed), p.send_right.load(Relaxed)), (4, 49));
        assert_eq!(c.get(node(&c, "/io/out/LCL/5/grp")), Some(Value::str("SEND")));
        assert_eq!(c.get(node(&c, "/io/out/B/2/in")), Some(Value::Int(4)));
        // One source per output: MAIN 1 L on A 5 takes it from the send.
        c.set(node(&c, "/io/out/A/5/grp"), &Value::str("MAIN")).unwrap();
        c.set(node(&c, "/io/out/A/5/in"), &Value::Int(1)).unwrap();
        assert_eq!(p.send_left.load(Relaxed), -1);
        assert_eq!(e.main(0).unwrap().params.output_left.load(Relaxed), 4);

        // The insert runs with a RETURN; without one the slot reads N/A.
        c.set(node(&c, "/ch/1/preins/ins"), &Value::str("FX2")).unwrap();
        assert_eq!(c.get(node(&c, "/ch/1/preins/$stat")), Some(Value::str("OK")));
        c.set(node(&c, "/fx/2/egrp"), &Value::str("OFF")).unwrap();
        assert_eq!(c.get(node(&c, "/ch/1/preins/$stat")), Some(Value::str("N/A")));
    }

    #[test]
    fn channel_source_gain_and_phantom() {
        let e = engine();
        let st = WingState::default();
        let c = Console::new(&e, &st);
        // CH 3 on A 4 (port 3): its $g / $vph are that source's GAIN / +48V.
        c.set(node(&c, "/ch/3/in/conn/grp"), &Value::str("A")).unwrap();
        c.set(node(&c, "/ch/3/in/conn/in"), &Value::Int(4)).unwrap();
        c.set(node(&c, "/ch/3/in/set/$g"), &Value::Float(24.0)).unwrap();
        c.set(node(&c, "/ch/3/in/set/$vph"), &Value::Int(1)).unwrap();
        let src = e.sources().get(3).unwrap();
        // A 4 is LOCAL IN 4 here: the local preamps' 2.5 dB steps.
        assert_eq!(src.gain_db.load(), 25.0);
        assert!(src.phantom.load(Relaxed));
        src.gain_db.store(10.5);
        assert_eq!(c.get(node(&c, "/ch/3/in/set/$g")), Some(Value::Float(10.5)));
        assert_eq!(c.get(node(&c, "/io/in/A/4/g")), Some(Value::Float(10.5)));
        // The ALT input while in use (INPUT SELECT per channel: a new console's is GLOBAL).
        c.set(node(&c, "/io/autoaltovr"), &Value::Int(0)).unwrap();
        c.set(node(&c, "/ch/3/in/conn/altgrp"), &Value::str("B")).unwrap();
        c.set(node(&c, "/ch/3/in/conn/altin"), &Value::Int(2)).unwrap();
        c.set(node(&c, "/ch/3/in/set/altsrc"), &Value::Int(1)).unwrap();
        c.set(node(&c, "/ch/3/in/set/$g"), &Value::Float(30.0)).unwrap();
        assert_eq!(e.sources().get(49).unwrap().gain_db.load(), 30.0);
        assert_eq!(src.gain_db.load(), 10.5);
        // Each group's range and steps: AES50 B -3 .. +45.5 in 0.5 dB steps.
        c.set(node(&c, "/ch/3/in/set/$g"), &Value::Float(50.0)).unwrap();
        assert_eq!(e.sources().get(49).unwrap().gain_db.load(), 45.5);
        c.set(node(&c, "/ch/3/in/set/$g"), &Value::Float(-10.0)).unwrap();
        assert_eq!(e.sources().get(49).unwrap().gain_db.load(), -3.0);
        // A source that isn't one of Sidecar's (a bus) keeps the value in the shadow store.
        c.set(node(&c, "/ch/9/in/conn/grp"), &Value::str("BUS")).unwrap();
        c.set(node(&c, "/ch/9/in/set/$g"), &Value::Float(6.0)).unwrap();
        assert_eq!(c.get(node(&c, "/ch/9/in/set/$g")), Some(Value::Float(6.0)));
    }

    #[test]
    fn rta_shown_and_monitor_level() {
        let e = engine();
        let st = WingState::default();
        let c = Console::new(&e, &st);
        let a = e.analysis().controls();
        let aux2 = c.strip(StripKind::Aux, 2).unwrap().clone();
        a.strip_kind.store(StripKind::Aux as i32, Relaxed);
        a.strip_id.store(aux2.id(), Relaxed);
        assert_eq!(c.get(node(&c, "/cfg/rta/$src")), Some(Value::Int(42)));
        a.home.decay.store(0, Relaxed);
        a.eq.decay.store(2, Relaxed);
        a.view.store(rta_view::HOME, Relaxed);
        assert_eq!(c.get(node(&c, "/cfg/rta/$dec")), Some(Value::str("SLOW")));
        a.view.store(rta_view::EQ, Relaxed);
        assert_eq!(c.get(node(&c, "/cfg/rta/$dec")), Some(Value::str("FAST")));
        assert!(c.set(node(&c, "/cfg/rta/$src"), &Value::Int(1)).is_err());
        e.controls.phones.level_db.store(-9.0);
        assert_eq!(c.get(node(&c, "/cfg/mon/1/$lvlact")), Some(Value::Float(-9.0)));
    }

    #[test]
    fn library_scene_control() {
        let e = engine();
        let st = WingState::default();
        let c = Console::new(&e, &st);
        let action = node(&c, "/$ctl/lib/$action");
        // Without a host there is nothing to recall.
        assert!(matches!(c.set(action, &Value::str("GO")), Err(SetError::Unsupported(_))));
        st.set_library(Library {
            attached: true,
            show: "Sunday".into(),
            num_scenes: 5,
            current: 1,
            scene: "Verse".into(),
            tag: 7,
        });
        assert_eq!(c.get(node(&c, "/$ctl/lib/$actshow")), Some(Value::str("Sunday")));
        assert_eq!(c.get(node(&c, "/$ctl/lib/$active")), Some(Value::str("Verse")));
        assert_eq!(c.get(node(&c, "/$ctl/lib/$activeid")), Some(Value::Int(7)));
        assert_eq!(c.get(node(&c, "/$ctl/lib/$actidx")), Some(Value::Int(1)));
        c.set(node(&c, "/$ctl/lib/$actionidx"), &Value::Int(3)).unwrap();
        assert_eq!(c.set(action, &Value::str("GO")), Ok(Value::str("IDLE")));
        assert_eq!(st.take_lib_action(), Some(LibAction::Go(3)));
        assert_eq!(st.take_lib_action(), None);
        // NEXT / PREV move the selection within the show.
        c.set(action, &Value::str("NEXT")).unwrap();
        c.set(action, &Value::str("NEXT")).unwrap();
        assert_eq!(st.lib_index.load(Relaxed), 4);
        c.set(action, &Value::str("PREV")).unwrap();
        assert_eq!(c.get(node(&c, "/$ctl/lib/$actionidx")), Some(Value::Int(3)));
        assert_eq!(st.take_lib_action(), None);
        c.set(action, &Value::str("GONEXT")).unwrap();
        c.set(action, &Value::str("GOPREV")).unwrap();
        c.set(action, &Value::str("GOTAG")).unwrap();
        assert_eq!(st.take_lib_action(), Some(LibAction::GoNext));
        assert_eq!(st.take_lib_action(), Some(LibAction::GoPrev));
        assert_eq!(st.take_lib_action(), Some(LibAction::GoTag(3)));
        assert_eq!(c.set(node(&c, "/$ctl/lib/$active"), &Value::str("x")), Err(SetError::ReadOnly));
    }

    #[test]
    fn matrix_pan_link_dca_icon_and_mute_override() {
        let e = engine();
        let st = WingState::default();
        let c = Console::new(&e, &st);
        let ch3 = c.strip(StripKind::Channel, 3).unwrap().clone();
        let m = &ch3.params.matrix[1];
        assert_eq!(c.get(node(&c, "/ch/3/send/MX2/plink")), Some(Value::Int(0)), "not linked by default, as on the WING");
        c.set(node(&c, "/ch/3/send/MX2/plink"), &Value::Int(1)).unwrap();
        assert!(m.pan_link.load(Relaxed));
        c.set(node(&c, "/ch/3/send/MX2/plink"), &Value::Int(0)).unwrap();
        assert!(!m.pan_link.load(Relaxed));
        c.set(node(&c, "/aux/2/send/MX8/plink"), &Value::Int(2)).unwrap();
        let aux2 = c.strip(StripKind::Aux, 2).unwrap();
        assert!(aux2.params.matrix[7].pan_link.load(Relaxed) && aux2.params.matrix[7].pan_reversed.load(Relaxed));
        assert_eq!(c.get(node(&c, "/aux/2/send/MX8/plink")), Some(Value::Int(2)));
        // Buses have no matrix PAN LINK on the WING.
        assert!(schema().resolve("/bus/1/send/MX1/plink", &c).is_none());

        c.set(node(&c, "/dca/4/icon"), &Value::Int(205)).unwrap();
        c.set(node(&c, "/dca/4/led"), &Value::Int(0)).unwrap();
        assert_eq!(e.controls.dcas[3].icon.load(Relaxed), 205);
        assert!(!e.controls.dcas[3].led.load(Relaxed));
        assert_eq!(c.get(node(&c, "/dca/4/led")), Some(Value::Int(0)));
        // SOLO BUS: A (phones), B (speakers), A+B.
        assert_eq!(c.get(node(&c, "/dca/4/mon")), Some(Value::str("A")));
        c.set(node(&c, "/dca/4/mon"), &Value::str("B")).unwrap();
        assert_eq!(e.controls.dcas[3].mon.load(Relaxed), 1);
        assert_eq!(c.get(node(&c, "/dca/4/mon")), Some(Value::str("B")));

        c.set(node(&c, "/bus/5/$muteovr"), &Value::Int(1)).unwrap();
        assert!(c.strip(StripKind::Bus, 5).unwrap().console.mute_override.load(Relaxed));
        assert_eq!(c.get(node(&c, "/bus/5/$muteovr")), Some(Value::Int(1)));
        assert_eq!(c.get(node(&c, "/ch/1/$muteovr")), Some(Value::Int(0)));
    }

    #[test]
    fn customization_link_shows_the_sources_name_colour_and_icon() {
        let e = engine();
        let st = WingState::default();
        let c = Console::new(&e, &st);
        let ch2 = c.strip(StripKind::Channel, 2).unwrap().clone();
        ch2.set_name("OWN");
        ch2.params.colour.store(9, Relaxed);
        c.set(node(&c, "/ch/2/in/conn/grp"), &Value::str("A")).unwrap();
        c.set(node(&c, "/ch/2/in/conn/in"), &Value::Int(4)).unwrap();
        let src = e.sources().get(3).unwrap();
        src.colour.store(14, Relaxed);
        src.icon.store(200, Relaxed);
        e.sources().set_name(3, "KICK");

        assert_eq!(c.get(node(&c, "/ch/2/clink")), Some(Value::Int(1)), "linked by default");
        assert_eq!(c.get(node(&c, "/ch/2/$name")), Some(Value::str("KICK")));
        assert_eq!(c.get(node(&c, "/ch/2/$col")), Some(Value::Int(14)));
        assert_eq!(c.get(node(&c, "/ch/2/$icon")), Some(Value::Int(200)));
        assert_eq!(c.get(node(&c, "/ch/2/col")), Some(Value::Int(9)), "the channel's own colour");

        c.set(node(&c, "/ch/2/clink"), &Value::Int(0)).unwrap();
        assert!(!ch2.params.cust_link.load(Relaxed));
        assert_eq!(c.get(node(&c, "/ch/2/$name")), Some(Value::str("OWN")));
        assert_eq!(c.get(node(&c, "/ch/2/$col")), Some(Value::Int(9)));
        assert!(schema().resolve("/bus/1/clink", &c).is_none());
    }

    #[test]
    fn usb_recorder_and_player() {
        let e = engine();
        let st = WingState::default();
        let c = Console::new(&e, &st);
        let d = e.usb();
        // SAFETY: no audio thread runs.
        unsafe { d.prepare(48000.0, 256) };
        assert_eq!(c.get(node(&c, "/rec/$actstate")), Some(Value::str("STOP")));
        assert_eq!(c.get(node(&c, "/play/$actstate")), Some(Value::str("STOP")));

        // The format and REC PATH read back and wait for the host.
        c.set(node(&c, "/rec/channels"), &Value::str("4")).unwrap();
        c.set(node(&c, "/rec/resolution"), &Value::str("16")).unwrap();
        c.set(node(&c, "/rec/$path"), &Value::str("LIVE/SUNDAY")).unwrap();
        assert_eq!(c.get(node(&c, "/rec/channels")), Some(Value::str("4")));
        assert_eq!(c.get(node(&c, "/rec/$path")), Some(Value::str("LIVE/SUNDAY")));
        let dir = std::env::temp_dir().join(format!("ld-wing-usb-{}", std::process::id()));
        std::fs::create_dir_all(dir.join("LIVE")).unwrap();
        let storage = dir.to_string_lossy().into_owned();
        // The host publishing its own settings doesn't undo a change it hasn't taken.
        st.set_usb_settings(UsbSettings { attached: true, storage: storage.clone(), ..UsbSettings::default() });
        assert_eq!(c.get(node(&c, "/rec/resolution")), Some(Value::str("16")));
        let taken = st.take_usb_settings().unwrap();
        assert_eq!((taken.channels, taken.bits, taken.rec_path.as_str()), (4, 16, "LIVE/SUNDAY"));
        assert_eq!(st.take_usb_settings(), None);
        st.set_usb_settings(UsbSettings { attached: true, storage: storage.clone(), ..UsbSettings::default() });
        assert_eq!(c.get(node(&c, "/rec/channels")), Some(Value::str("2")));

        // REC from STOP: the host names the file.
        let action = node(&c, "/rec/$action");
        assert_eq!(c.set(action, &Value::str("REC")), Ok(Value::str("IDLE")));
        assert_eq!(st.take_usb_action(), Some(UsbAction::Record));
        let file = dir.join("LIVE").join("R_1.wav");
        d.record(&file, RecFormat::default()).unwrap();
        assert_eq!(c.get(node(&c, "/rec/$actstate")), Some(Value::str("REC")));
        assert_eq!(c.get(node(&c, "/rec/$actfile")), Some(Value::str("LIVE/R_1.wav")));
        c.set(action, &Value::str("NEWFILE")).unwrap();
        assert_eq!(st.take_usb_action(), Some(UsbAction::NewFile));
        c.set(action, &Value::str("STOP")).unwrap();
        assert_eq!(c.get(node(&c, "/rec/$actstate")), Some(Value::str("STOP")));

        // The player: transport, PLAYFILE through the host, REPEAT.
        let play = node(&c, "/play/$action");
        c.set(play, &Value::str("PLAY")).unwrap();
        assert_eq!(c.get(node(&c, "/play/$actstate")), Some(Value::str("PLAY")));
        c.set(play, &Value::str("STOP")).unwrap();
        assert_eq!(c.get(node(&c, "/play/$actstate")), Some(Value::str("STOP")));
        c.set(node(&c, "/play/$playfile"), &Value::str("LIVE/R_1.wav")).unwrap();
        c.set(play, &Value::str("PLAYFILE")).unwrap();
        assert_eq!(st.take_usb_action(), Some(UsbAction::PlayFile("LIVE/R_1.wav".into())));
        c.set(node(&c, "/play/repeat"), &Value::Int(1)).unwrap();
        assert!(d.queue().repeat());
        assert!(c.set(node(&c, "/play/$actstate"), &Value::str("PLAY")).is_err());

        // Without a host there is nowhere to record to.
        st.set_usb_settings(UsbSettings::default());
        assert!(matches!(c.set(action, &Value::str("REC")), Err(SetError::Unsupported(_))));
        assert!(matches!(c.set(play, &Value::str("PLAYFILE")), Err(SetError::Unsupported(_))));
        assert!(!c.is_mapped(node(&c, "/play/$artist")));
        d.wait_for_disk(std::time::Duration::from_secs(5));
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn recall_safes() {
        let e = engine();
        let st = WingState::default();
        let c = Console::new(&e, &st);
        // The console's format: a "+" (safe) or " " per strip.
        assert_eq!(c.get(node(&c, "/$ctl/safes/main")), Some(Value::str("    ")));
        c.set(node(&c, "/$ctl/safes/ch"), &Value::str("+ +++")).unwrap();
        let safe = |kind, n| c.strip(kind, n).unwrap().params.recall_safe.load(Relaxed);
        assert!(safe(StripKind::Channel, 1) && !safe(StripKind::Channel, 2) && safe(StripKind::Channel, 5));
        assert_eq!(c.get(node(&c, "/$ctl/safes/ch")), Some(Value::Str(format!("+ +++{}", " ".repeat(35)))));
        // Numbers and ranges, and 0/1 flags, read too.
        c.set(node(&c, "/$ctl/safes/aux"), &Value::str("1,3-5")).unwrap();
        assert_eq!(c.get(node(&c, "/$ctl/safes/aux")), Some(Value::str("+ +++   ")));
        c.set(node(&c, "/$ctl/safes/mtx"), &Value::str("10100000")).unwrap();
        assert_eq!(c.get(node(&c, "/$ctl/safes/mtx")), Some(Value::str("+ +     ")));
        c.strip(StripKind::Bus, 16).unwrap().params.recall_safe.store(true, Relaxed);
        assert_eq!(c.get(node(&c, "/$ctl/safes/bus")), Some(Value::Str(format!("{}+", " ".repeat(15)))));

        // DCAs, mute groups and FX: the show's global safes, through the host.
        c.set(node(&c, "/$ctl/safes/dca"), &Value::str(" +             +")).unwrap();
        c.set(node(&c, "/$ctl/safes/fx"), &Value::str("1-2,9")).unwrap();
        st.set_safes(Safes { attached: true, ..Safes::default() });
        assert_eq!(c.get(node(&c, "/$ctl/safes/dca")), Some(Value::str(" +             +")));
        let taken = st.take_safes().unwrap();
        assert_eq!((taken.dcas, taken.mute_groups, taken.fx), (1 << 1 | 1 << 15, 0, 0b1_0000_0011));
        st.set_safes(Safes { attached: true, mute_groups: 0b101, ..Safes::default() });
        assert_eq!(c.get(node(&c, "/$ctl/safes/mute")), Some(Value::str("+ +     ")));
        assert_eq!(c.get(node(&c, "/$ctl/safes/dca")), Some(Value::str(" ".repeat(16))));
        // Source and output safes (G5.6.3): the engine's, by Sidecar source
        // and output port. LCL n and A n are the same port here.
        let source = node(&c, "/$ctl/safes/source/A");
        assert!(c.is_mapped(source));
        assert_eq!(c.get(source), Some(Value::str(" ".repeat(48))));
        c.set(source, &Value::str("+ +")).unwrap();
        let io = e.io_safes();
        assert!(io.sources.get(0) && !io.sources.get(1) && io.sources.get(2));
        assert_eq!(c.get(node(&c, "/$ctl/safes/source/LCL")), Some(Value::Str(format!("+ +{}", " ".repeat(21)))));
        c.set(node(&c, "/$ctl/safes/source/OSC"), &Value::str(" +")).unwrap();
        let osc2 = livedaw_engine::sources::source_index(livedaw_engine::sources::OSC_SOURCE_BASE + 1).unwrap();
        assert!(io.sources.get(osc2));
        c.set(node(&c, "/$ctl/safes/output/B"), &Value::str("2")).unwrap();
        assert!(io.outputs.get(49) && io.outputs.indices().len() == 1);
        assert_eq!(c.get(node(&c, "/$ctl/safes/output/B")), Some(Value::Str(format!(" +{}", " ".repeat(46)))));
        // Groups Sidecar doesn't have are only kept.
        let usb = node(&c, "/$ctl/safes/source/USB");
        assert!(!c.is_mapped(usb));
        c.set(usb, &Value::str("+")).unwrap();
        assert_eq!(c.get(usb), Some(Value::str("+")));
        assert_eq!(number_list(0b1110_1011), "1,2,4,6-8");
        assert_eq!(parse_number_list("0-3, 7-99", 8), 0b1100_0111);
    }

    #[test]
    fn surface_layers_both_ways() {
        use livedaw_session::custom::CustomControls;
        use livedaw_session::layers::{FaderRef, LayerSet, SpillKind, CENTER, LEFT, RIGHT};
        use livedaw_session::refs::StripRef;
        let e = engine();
        let st = WingState::default();
        let c = Console::new(&e, &st);
        let (mut layers, mut custom) = (LayerSet::default(), CustomControls::default());
        let sel = node(&c, "/$ctl/layer/L/sel");
        // Without a host the nodes are shadow.
        assert!(!c.is_mapped(sel));
        c.set(sel, &Value::Int(3)).unwrap();
        assert_eq!(layers.selected(LEFT), "in-1");
        let sync = |layers: &mut LayerSet, custom: &mut CustomControls| surface::sync(&st, &e, layers, custom, 0);
        sync(&mut layers, &mut custom);
        assert!(c.is_mapped(sel));
        assert_eq!(c.get(sel), Some(Value::Int(1)));

        // Layer keys: a client selects, the show follows.
        c.set(sel, &Value::Int(5)).unwrap();
        assert_eq!(c.get(sel), Some(Value::Int(5)), "shown before the host applies it");
        assert!(sync(&mut layers, &mut custom).layers);
        assert_eq!(layers.selected(LEFT), "bus-1");
        c.set(node(&c, "/$ctl/layer/R/sel"), &Value::Int(3)).unwrap();
        c.set(node(&c, "/$ctl/layer/C/sel"), &Value::Int(12)).unwrap(); // a fixed bank: ignored
        sync(&mut layers, &mut custom);
        assert_eq!((layers.selected(RIGHT), layers.selected(CENTER)), ("inputs", "dca-1"));
        // The scroll position of the shown layer.
        c.set(node(&c, "/$ctl/layer/R/3/ofs"), &Value::Int(8)).unwrap();
        c.set(node(&c, "/$ctl/layer/R/1/ofs"), &Value::Int(4)).unwrap(); // not shown: ignored
        sync(&mut layers, &mut custom);
        assert_eq!(layers.offset(RIGHT), 8);
        assert_eq!(c.get(node(&c, "/$ctl/layer/R/1/ofs")), Some(Value::Int(0)));

        // Local changes show to clients.
        layers.select(CENTER, "out-1");
        let drums = layers.add_user_layer("Drums", vec![FaderRef::Strip(StripRef::new(StripKind::Channel, 3)), FaderRef::Dca(1)]);
        sync(&mut layers, &mut custom);
        assert_eq!(c.get(node(&c, "/$ctl/layer/C/sel")), Some(Value::Int(2)));
        assert_eq!(c.get(node(&c, "/$ctl/layer/L/6/name")), Some(Value::str("Drums")));
        assert_eq!(c.get(node(&c, "/$ctl/layer/C/5/1/type")), Some(Value::str("CH")));
        assert_eq!(c.get(node(&c, "/$ctl/layer/C/5/1/i")), Some(Value::Int(3)));
        assert_eq!(c.get(node(&c, "/$ctl/layer/C/5/2/type")), Some(Value::str("DCA")));
        assert_eq!(c.get(node(&c, "/$ctl/layer/C/5/2/i")), Some(Value::Int(2)));
        assert_eq!(c.get(node(&c, "/$ctl/layer/C/5/3/type")), Some(Value::str("OFF")));
        // The default keys' faders: MAIN 1 is the right section's first.
        assert_eq!(c.get(node(&c, "/$ctl/layer/R/1/1/type")), Some(Value::str("BUS")));
        assert_eq!(c.get(node(&c, "/$ctl/layer/R/1/1/i")), Some(Value::Int(17)));

        // A client edits USER 1: a send replaces the DCA, a bus goes to slot 9
        // (blanks before it), OFF blanks slot 1. Nothing shifts.
        let f = |p: &str, v: Value| c.set(node(&c, p), &v).unwrap();
        f("/$ctl/layer/L/6/2/type", Value::str("SEND"));
        f("/$ctl/layer/L/6/2/i", Value::Int(41));
        f("/$ctl/layer/L/6/2/dst", Value::Int(17));
        f("/$ctl/layer/L/6/9/type", Value::str("BUS"));
        f("/$ctl/layer/L/6/9/i", Value::Int(4));
        f("/$ctl/layer/L/6/1/type", Value::str("OFF"));
        f("/$ctl/layer/L/6/name", Value::str("Band"));
        f("/$ctl/layer/L/7/1/type", Value::str("MIDI"));
        f("/$ctl/layer/L/7/1/i", Value::Int(74));
        f("/$ctl/layer/L/7/1/dst", Value::Int(16));
        assert!(sync(&mut layers, &mut custom).layers);
        let aux1 = StripRef::of(&e.strips(StripKind::Aux)[0]);
        let main1 = StripRef::of(&e.strips(StripKind::Main)[0]);
        let user = layers.user_layer(&drums).unwrap();
        assert_eq!(user.name, "Band");
        let mut expected = vec![FaderRef::Empty; 9];
        expected[1] = FaderRef::Send { from: aux1, to: main1 };
        expected[8] = FaderRef::Strip(StripRef::new(StripKind::Bus, 3));
        assert_eq!(user.faders, expected);
        assert_eq!(layers.user_layers()[1].faders, vec![FaderRef::midi_cc(15, 74)], "USER 2 made");
        assert_eq!(c.get(node(&c, "/$ctl/layer/L/6/1/type")), Some(Value::str("OFF")), "a blank slot is OFF");
        assert_eq!(c.get(node(&c, "/$ctl/layer/L/6/9/type")), Some(Value::str("BUS")), "republished in place");
        assert_eq!(c.get(node(&c, "/$ctl/layer/L/6/3/type")), Some(Value::str("OFF")));
        // A default key becomes an edited key; setting what it holds changes nothing.
        f("/$ctl/layer/C/1/1/type", Value::str("DCA"));
        assert!(!sync(&mut layers, &mut custom).layers);
        f("/$ctl/layer/C/1/1/type", Value::str("FX"));
        f("/$ctl/layer/C/1/1/i", Value::Int(3));
        f("/$ctl/layer/C/1/1/dst", Value::Int(2));
        sync(&mut layers, &mut custom);
        assert!(layers.has_default_override(CENTER, "dca-1"));
        assert_eq!(layers.find_layer(&e, CENTER, "dca-1").faders[0], FaderRef::FxParam { slot: 2, param: 1 });
        // SENDS has no equivalent.
        f("/$ctl/layer/C/1/2/type", Value::str("SENDS"));
        assert!(!sync(&mut layers, &mut custom).layers);

        // Spill: DCA 3 on the left; it shows, and ends with 0.
        e.strips(StripKind::Channel)[4].params.dca_mask.store(1 << 2, Relaxed);
        f("/$ctl/layer/L/spidx", Value::Int(3));
        sync(&mut layers, &mut custom);
        assert_eq!(layers.spill(LEFT), Some(SpillKind::Dca(2)));
        assert_eq!(c.get(sel), Some(Value::Int(22)));
        assert_eq!(c.get(node(&c, "/$ctl/layer/L/$spill/name")), Some(Value::str("DCA 3 SPILL")));
        assert_eq!(c.get(node(&c, "/$ctl/layer/L/$spill/1/i")), Some(Value::Int(5)));
        f("/$ctl/layer/L/$spill/1/i", Value::Int(9)); // read only: ignored
        assert_eq!(c.get(node(&c, "/$ctl/layer/L/$spill/1/i")), Some(Value::Int(5)));
        layers.start_spill(RIGHT, SpillKind::Automix(1));
        sync(&mut layers, &mut custom);
        assert_eq!(c.get(node(&c, "/$ctl/layer/R/spidx")), Some(Value::Int(62)));
        f("/$ctl/layer/L/spidx", Value::Int(0));
        sync(&mut layers, &mut custom);
        assert_eq!((layers.spill(LEFT), layers.selected(LEFT)), (None, "bus-1"));

        // No host: shadow again.
        surface::detach(&st);
        assert!(!c.is_mapped(sel));
    }

    #[test]
    fn surface_user_layer_with_a_fader_twice() {
        use livedaw_session::custom::CustomControls;
        use livedaw_session::layers::{FaderRef, LayerSet};
        use livedaw_session::refs::StripRef;
        let e = engine();
        let st = WingState::default();
        let c = Console::new(&e, &st);
        let (mut layers, mut custom) = (LayerSet::default(), CustomControls::default());
        let sync = |layers: &mut LayerSet, custom: &mut CustomControls| surface::sync(&st, &e, layers, custom, 0);
        let ch5 = FaderRef::Strip(StripRef::of(&e.strips(StripKind::Channel)[4]));
        let ch7 = FaderRef::Strip(StripRef::of(&e.strips(StripKind::Channel)[6]));
        let id = layers.add_user_layer("Dup", vec![ch5, FaderRef::Empty, ch5]);
        sync(&mut layers, &mut custom);
        let get = |p: &str| c.get(node(&c, p));
        // Both copies read as CH 5; the blank between them is OFF.
        for pos in [1, 3] {
            assert_eq!(get(&format!("/$ctl/layer/L/6/{pos}/type")), Some(Value::str("CH")));
            assert_eq!(get(&format!("/$ctl/layer/L/6/{pos}/i")), Some(Value::Int(5)));
        }
        assert_eq!(get("/$ctl/layer/L/6/2/type"), Some(Value::str("OFF")));

        // A client puts CH 5 in the blank slot too: a third copy, the others stay.
        let f = |p: &str, v: Value| c.set(node(&c, p), &v).unwrap();
        f("/$ctl/layer/L/6/2/type", Value::str("CH"));
        f("/$ctl/layer/L/6/2/i", Value::Int(5));
        assert!(sync(&mut layers, &mut custom).layers);
        assert_eq!(layers.user_layer(&id).unwrap().faders, vec![ch5, ch5, ch5]);
        // OFF on one copy blanks just that slot; changing another changes just it.
        f("/$ctl/layer/L/6/1/type", Value::str("OFF"));
        f("/$ctl/layer/L/6/3/i", Value::Int(7));
        assert!(sync(&mut layers, &mut custom).layers);
        assert_eq!(layers.user_layer(&id).unwrap().faders, vec![FaderRef::Empty, ch5, ch7]);
        assert_eq!(get("/$ctl/layer/L/6/2/i"), Some(Value::Int(5)));
        assert_eq!(get("/$ctl/layer/L/6/3/i"), Some(Value::Int(7)));
        // Setting a copy to what it holds changes nothing.
        f("/$ctl/layer/L/6/2/i", Value::Int(5));
        assert!(!sync(&mut layers, &mut custom).layers);
        // The center's USER 1 key shows the same layer.
        assert_eq!(get("/$ctl/layer/C/5/2/i"), Some(Value::Int(5)));
    }

    #[test]
    fn surface_custom_controls_both_ways() {
        use livedaw_session::custom::{CcFunction, CustomControls};
        use livedaw_session::layers::LayerSet;
        let e = engine();
        let st = WingState::default();
        let c = Console::new(&e, &st);
        let (mut layers, mut custom) = (LayerSet::default(), CustomControls::default());
        let mut sync = |custom: &mut CustomControls, bank| surface::sync(&st, &e, &mut layers, custom, bank);
        sync(&mut custom, 2);
        assert_eq!(c.get(node(&c, "/$ctl/user/sel")), Some(Value::Int(3)));
        assert_eq!(c.get(node(&c, "/$ctl/user/1/1/enc/mode")), Some(Value::str("OFF")));

        // Local assignments show to clients.
        let ctl = custom.control_mut(4, 1).unwrap();
        ctl.encoder = CcFunction { category: 9, target: 0, option: 7 };
        ctl.lower = CcFunction { category: 14, target: 3 * 16 + 2, option: 60 };
        ctl.upper = CcFunction { category: 18, target: 0, option: 2 };
        ctl.name = "Synth".into();
        ctl.colour = 6;
        custom.control_mut(21, 3).unwrap().upper = CcFunction { category: 13, target: 0, option: 1 };
        sync(&mut custom, 2);
        let get = |p: &str| c.get(node(&c, p)).unwrap();
        assert_eq!(get("/$ctl/user/5/2/enc/mode"), Value::str("MIDICC"));
        assert_eq!(get("/$ctl/user/5/2/enc/$fname"), Value::str("CC 1:7"));
        assert_eq!(get("/$ctl/user/5/2/bd/mode"), Value::str("MIDINP"));
        assert_eq!(get("/$ctl/user/5/2/bu/mode"), Value::str("OTHER"), "CC BANK");
        assert_eq!(get("/$ctl/user/5/2/enc/name"), Value::str("Synth"));
        assert_eq!(get("/$ctl/user/5/2/col"), Value::Int(6));
        assert_eq!(get("/$ctl/user/daw2/4/bu/mode"), Value::str("DAWBTN"));

        // A client assigns: a new function starts at its first target.
        let f = |p: &str, v: Value| c.set(node(&c, p), &v);
        f("/$ctl/user/1/1/enc/mode", Value::str("FDR")).unwrap();
        f("/$ctl/user/1/1/enc/name", Value::str("Lead")).unwrap();
        f("/$ctl/user/1/1/col", Value::Int(11)).unwrap();
        f("/$ctl/user/1/1/led", Value::Int(1)).unwrap();
        f("/$ctl/user/user/2/bu/mode", Value::str("MUTE")).unwrap();
        f("/$ctl/user/user/2/bu/name", Value::str("Band")).unwrap();
        f("/$ctl/user/5/2/bd/mode", Value::str("MIDICCT")).unwrap();
        f("/$ctl/user/U2/1/bu/mode", Value::str("SCENES")).unwrap();
        assert!(matches!(f("/$ctl/user/1/2/bu/mode", Value::str("SESSIONA")), Err(SetError::Unsupported(_))));
        assert_eq!(f("/$ctl/user/1/1/enc/$fname", Value::str("x")), Err(SetError::ReadOnly));
        f("/$ctl/user/sel", Value::Int(9)).unwrap();
        let r = sync(&mut custom, 2);
        assert!(r.controls);
        assert_eq!(r.cc_bank, Some(8));
        let ch1 = e.strips(StripKind::Channel)[0].id();
        let ctl = custom.control(0, 0).unwrap();
        assert_eq!(ctl.encoder, CcFunction { category: 1, target: ch1, option: 0 });
        assert_eq!((ctl.name.as_str(), ctl.colour), ("Lead", 11));
        let user = custom.control(16, 1).unwrap();
        assert_eq!((user.upper.category, user.name.as_str()), (1, "Band"));
        assert_eq!(
            custom.control(4, 1).unwrap().lower,
            CcFunction { category: 14, target: 2, option: 60 },
            "keeps channel and number"
        );
        assert_eq!(custom.control(25, 0).unwrap().upper, CcFunction { category: 10, target: 1, option: 0 }, "U2: USER 5-8");
        assert_eq!(get("/$ctl/user/sel"), Value::Int(9));
        // The same function again changes nothing.
        f("/$ctl/user/1/1/enc/mode", Value::str("FDR")).unwrap();
        assert!(!sync(&mut custom, 8).controls);
    }

    #[test]
    fn the_compact_key_sets_are_the_compacts() {
        use livedaw_session::custom::{
            CcFunction, CustomControls, COMPACT_MAIN_MATRIX_LAYER, FIRST_COMPACT_DCA_LAYER, FIRST_COMPACT_USER_LAYER,
        };
        use livedaw_session::layers::LayerSet;
        let e = engine();
        let st = WingState::default();
        let c = Console::new(&e, &st);
        let (mut layers, mut custom) = (LayerSet::default(), CustomControls::default());
        let mut sync = |custom: &mut CustomControls| surface::sync(&st, &e, &mut layers, custom, 0);
        sync(&mut custom);
        let get = |p: &str| c.get(node(&c, p));
        let f = |p: &str, v: Value| c.set(node(&c, p), &v);

        // USER 1-4 .. 13-16, MAIN/MTX 5-8 and DCA 1-4 .. 13-16 on either
        // model (the full-size WING's F1-F3 play layers 1-3: it has no U1-U3
        // of its own), both ways.
        for model in [ConsoleModel::Full, ConsoleModel::Compact] {
            st.set_console_model(model);
            for p in ["/$ctl/user/U1/1/bu/mode", "/$ctl/user/U4/4/col", "/$ctl/user/MM/2/bu/name", "/$ctl/user/D4/4/bu/$fname"] {
                assert!(c.is_mapped(node(&c, p)), "{p}");
            }
        }
        st.set_console_model(ConsoleModel::Full);
        f("/$ctl/user/U1/2/bu/mode", Value::str("MUTE")).unwrap();
        sync(&mut custom);
        assert_eq!(custom.control(FIRST_COMPACT_USER_LAYER, 1).unwrap().upper.category, 1);
        assert!(custom.control(17, 1).unwrap().is_default(), "no layer of the 4-Channel section's own");

        st.set_console_model(ConsoleModel::Compact);
        assert_eq!(get("/$ctl/user/U1/2/bu/mode"), Some(Value::str("MUTE")));
        let ctl = custom.control_mut(FIRST_COMPACT_USER_LAYER + 3, 3).unwrap();
        ctl.upper = CcFunction { category: 10, target: 2, option: 0 };
        ctl.name = "SONG 2".into();
        ctl.colour = 9;
        sync(&mut custom);
        assert_eq!(get("/$ctl/user/U4/4/bu/mode"), Some(Value::str("SCENES")));
        assert_eq!(get("/$ctl/user/U4/4/bu/$fname"), Some(Value::str("|SCENE 2")));
        assert_eq!(get("/$ctl/user/U4/4/bu/tag"), Some(Value::Int(2)));
        assert_eq!(get("/$ctl/user/U4/4/bu/name"), Some(Value::str("SONG 2")));
        assert_eq!(get("/$ctl/user/U4/4/col"), Some(Value::Int(9)));
        f("/$ctl/user/MM/1/bu/mode", Value::str("MUTE")).unwrap();
        f("/$ctl/user/MM/1/bu/name", Value::str("PA")).unwrap();
        f("/$ctl/user/D2/3/bu/mode", Value::str("DCAMUTE")).unwrap();
        f("/$ctl/user/D2/3/bu/dca", Value::Int(7)).unwrap();
        f("/$ctl/user/D2/3/col", Value::Int(4)).unwrap();
        f("/$ctl/user/D2/3/led", Value::Int(1)).unwrap();
        assert!(sync(&mut custom).controls);
        let mm = custom.control(COMPACT_MAIN_MATRIX_LAYER, 0).unwrap();
        assert_eq!((mm.upper.category, mm.name.as_str()), (1, "PA"));
        let dca = custom.control(FIRST_COMPACT_DCA_LAYER + 1, 2).unwrap();
        assert_eq!((dca.upper, dca.colour), (CcFunction { category: 3, target: 6, option: 0 }, 4));
        // They have an upper key only.
        assert!(
            schema().resolve("/$ctl/user/U1/1/bd/mode", &NoModels).is_none() || !c.is_mapped(node(&c, "/$ctl/user/U1/1/bd/mode"))
        );
        assert!(
            schema().resolve("/$ctl/user/MM/1/enc/mode", &NoModels).is_none()
                || !c.is_mapped(node(&c, "/$ctl/user/MM/1/enc/mode"))
        );
    }

    #[test]
    fn custom_control_functions_have_their_own_nodes() {
        use livedaw_session::custom::{
            other, CcFunction, CustomControls, CC_BANK_CATEGORY, FX_PARAMETER_CATEGORY, INSERT_POST_CATEGORY,
            INSERT_PRE_CATEGORY, OTHER_CATEGORY, OVERVIEW_PAGE_CATEGORY,
        };
        use livedaw_session::layers::LayerSet;
        let e = engine();
        let st = WingState::default();
        let c = Console::new(&e, &st);
        let (mut layers, mut custom) = (LayerSet::default(), CustomControls::default());
        let mut sync = |custom: &mut CustomControls| surface::sync(&st, &e, &mut layers, custom, 0);
        sync(&mut custom);
        let get = |p: &str| c.get(node(&c, p));
        let f = |p: &str, v: Value| c.set(node(&c, p), &v);
        let s = schema();
        let ch = |n: usize| StripKind::Channel as i32 * 100_000 + e.strips(StripKind::Channel)[n - 1].id();

        // The mode's nodes come and go with it; their ids are the WING's
        // (WING Edit's: one per position, as the processing models').
        assert!(s.resolve("/$ctl/user/1/2/bu/ch", &c).is_none());
        f("/$ctl/user/1/2/bu/mode", Value::str("INS1")).unwrap();
        let ch_node = node(&c, "/$ctl/user/1/2/bu/ch");
        assert!(matches!(ch_node, Node::Param { index: 1, .. }));
        assert_eq!(s.id(ch_node), 0x247d9df8);
        assert_eq!(s.by_id(0x247d9df8, &c), Some(ch_node));
        assert!(c.is_mapped(ch_node));
        assert_eq!(get("/$ctl/user/1/2/bu/ch"), Some(Value::Int(1)), "a new function starts at CH 1");
        f("/$ctl/user/1/2/bu/ch", Value::Int(5)).unwrap();
        f("/$ctl/user/1/2/bd/mode", Value::str("INS2")).unwrap();
        f("/$ctl/user/1/2/bd/ch", Value::Int(41)).unwrap(); // AUX 1
                                                            // FX PAR on a key: FX 3's MIX, then its parameter 4.
        f("/$ctl/user/2/1/bu/mode", Value::str("FXPAR")).unwrap();
        f("/$ctl/user/2/1/bu/fx", Value::str("FX3")).unwrap();
        assert_eq!(get("/$ctl/user/2/1/bu/par"), Some(Value::Int(41)), "MIX");
        f("/$ctl/user/2/1/bd/mode", Value::str("FXPAR")).unwrap();
        f("/$ctl/user/2/1/bd/fx", Value::str("FX9")).unwrap();
        f("/$ctl/user/2/1/bd/par", Value::Int(4)).unwrap();
        // OTHER: DAW LAYER, CC BANK 5, TALKBACK B.
        f("/$ctl/user/user/1/bu/mode", Value::str("OTHER")).unwrap();
        f("/$ctl/user/user/1/bu/other", Value::str("DAWSW")).unwrap();
        f("/$ctl/user/U2/3/bu/mode", Value::str("OTHER")).unwrap();
        f("/$ctl/user/U2/3/bu/other", Value::str("CCBANK")).unwrap();
        f("/$ctl/user/U2/3/bu/CCBK", Value::str("5")).unwrap();
        f("/$ctl/user/D1/1/bu/mode", Value::str("OTHER")).unwrap();
        f("/$ctl/user/D1/1/bu/other", Value::str("TBB")).unwrap();
        // OVERVIEW PAGE: the right section's bank 3.
        f("/$ctl/user/3/4/bu/mode", Value::str("VIEWPAGE")).unwrap();
        f("/$ctl/user/3/4/bu/area", Value::str("R")).unwrap();
        f("/$ctl/user/3/4/bu/bank", Value::Int(3)).unwrap();
        // An encoder: a fixed send to BUS 7 from CH 9; a send to a main is refused.
        f("/$ctl/user/4/1/enc/mode", Value::str("FSND")).unwrap();
        f("/$ctl/user/4/1/enc/ch", Value::Int(9)).unwrap();
        f("/$ctl/user/4/1/enc/send", Value::str("BUS7")).unwrap();
        assert!(matches!(f("/$ctl/user/4/1/enc/send", Value::str("MAIN2")), Err(SetError::Unsupported(_))));
        assert!(sync(&mut custom).controls);

        let fx3 = custom.control(1, 0).unwrap();
        assert_eq!(fx3.upper, CcFunction { category: FX_PARAMETER_CATEGORY, target: 2, option: -1 });
        assert_eq!(fx3.lower, CcFunction { category: FX_PARAMETER_CATEGORY, target: 8, option: 3 });
        let ins = custom.control(0, 1).unwrap();
        assert_eq!(ins.upper, CcFunction { category: INSERT_PRE_CATEGORY, target: ch(5), option: 0 });
        assert_eq!(ins.lower.category, INSERT_POST_CATEGORY);
        assert_eq!(ins.lower.target, StripKind::Aux as i32 * 100_000 + e.strips(StripKind::Aux)[0].id());
        assert_eq!(
            custom.control(16, 0).unwrap().upper,
            CcFunction { category: OTHER_CATEGORY, target: 0, option: other::DAW_LAYER }
        );
        assert_eq!(custom.control(25, 2).unwrap().upper, CcFunction { category: CC_BANK_CATEGORY, target: 0, option: 4 });
        assert_eq!(custom.control(29, 0).unwrap().upper.option, other::TALKBACK_B);
        assert_eq!(custom.control(2, 3).unwrap().upper, CcFunction { category: OVERVIEW_PAGE_CATEGORY, target: 2, option: 2 });
        assert_eq!(custom.control(3, 0).unwrap().encoder, CcFunction { category: 5, target: ch(9), option: 6 });

        // ... and they read back from the show, after the next publish too.
        sync(&mut custom);
        assert_eq!(get("/$ctl/user/1/2/bu/ch"), Some(Value::Int(5)));
        assert_eq!(get("/$ctl/user/1/2/bd/ch"), Some(Value::Int(41)));
        assert_eq!(get("/$ctl/user/2/1/bd/fx"), Some(Value::str("FX9")));
        assert_eq!(get("/$ctl/user/2/1/bd/par"), Some(Value::Int(4)));
        assert_eq!(get("/$ctl/user/user/1/bu/other"), Some(Value::str("DAWSW")));
        assert_eq!(get("/$ctl/user/U2/3/bu/mode"), Some(Value::str("OTHER")));
        assert_eq!(get("/$ctl/user/U2/3/bu/other"), Some(Value::str("CCBANK")));
        assert_eq!(get("/$ctl/user/U2/3/bu/CCBK"), Some(Value::str("5")));
        assert_eq!(get("/$ctl/user/3/4/bu/area"), Some(Value::str("R")));
        assert_eq!(get("/$ctl/user/4/1/enc/send"), Some(Value::str("BUS7")));
        assert_eq!(get("/$ctl/user/1/2/bu/$fname"), Some(Value::str("|5 INS1")));
        // The factory DAW layers' DAWBTN: btn, the same node as the sweep's.
        assert_eq!(get("/$ctl/user/daw1/1/bu/btn"), Some(Value::str("T1")));
        assert_eq!(get("/$ctl/user/daw1/1/bd/btn"), Some(Value::str("T4")));
        assert!(matches!(node(&c, "/$ctl/user/daw1/1/bu/btn"), Node::Param { index: 1, .. }));
        f("/$ctl/user/daw1/1/bu/btn", Value::str("AU4")).unwrap();
        sync(&mut custom);
        assert_eq!(
            custom.control(20, 0).unwrap().upper,
            CcFunction { category: 13, target: 5, option: 20 + 9 + 16 + 8 + 15 + 3 }
        );
        // The children of a key: its mode, name and function name, then the mode's nodes.
        let names: Vec<String> =
            s.children(node(&c, "/$ctl/user/2/1/bd"), &c).into_iter().map(|n| s.name(n, &c).into_owned()).collect();
        assert_eq!(names, ["mode", "name", "$fname", "fx", "par"]);

        // Without a show the nodes keep what a client sets.
        surface::detach(&st);
        f("/$ctl/user/5/1/bu/mode", Value::str("FXPAR")).unwrap();
        f("/$ctl/user/5/1/bu/par", Value::Int(12)).unwrap();
        assert_eq!(get("/$ctl/user/5/1/bu/par"), Some(Value::Int(12)));
    }

    #[test]
    fn fader_start_daw_vpot_keys_lighting_encoders_and_function_names() {
        use livedaw_session::custom::{
            CcFunction, CustomControls, DAW_VPOT_KEY_CATEGORY, ENCODER_LIGHTING_CATEGORY, FADER_SPILL_CATEGORY,
            FADER_START_CATEGORY,
        };
        use livedaw_session::layers::LayerSet;
        let e = engine();
        let st = WingState::default();
        let c = Console::new(&e, &st);
        let (mut layers, mut custom) = (LayerSet::default(), CustomControls::default());
        let mut sync = |custom: &mut CustomControls| surface::sync(&st, &e, &mut layers, custom, 0);
        sync(&mut custom);
        let get = |p: &str| c.get(node(&c, p)).unwrap();
        let f = |p: &str, v: Value| c.set(node(&c, p), &v);
        let mtx3 = StripKind::Matrix as i32 * 100_000 + e.strips(StripKind::Matrix)[2].id();

        // FADER START on MTX 3 (strip 71), DAW V-POTS M7 turned on a key, a
        // matrix spill, and the encoders' OTHER (GLOW BRIGHTNESS).
        f("/$ctl/user/1/1/bu/mode", Value::str("FSTART")).unwrap();
        f("/$ctl/user/1/1/bu/ch", Value::Int(71)).unwrap();
        f("/$ctl/user/1/1/bd/mode", Value::str("DAWENC")).unwrap();
        f("/$ctl/user/1/1/bd/enc", Value::str("M7")).unwrap();
        f("/$ctl/user/1/2/bu/mode", Value::str("SPILL")).unwrap();
        f("/$ctl/user/1/2/bu/tgt", Value::str("MTX4")).unwrap();
        f("/$ctl/user/1/2/bu/area", Value::str("R")).unwrap();
        f("/$ctl/user/1/2/enc/mode", Value::str("OTHER")).unwrap();
        f("/$ctl/user/1/2/enc/other", Value::str("BRIGLOW")).unwrap();
        assert!(sync(&mut custom).controls);
        let c1 = custom.control(0, 0).unwrap();
        assert_eq!(c1.upper, CcFunction { category: FADER_START_CATEGORY, target: mtx3, option: 0 });
        assert_eq!(c1.lower, CcFunction { category: DAW_VPOT_KEY_CATEGORY, target: 24 + 6, option: 0 });
        let c2 = custom.control(0, 1).unwrap();
        assert_eq!(c2.upper, CcFunction { category: FADER_SPILL_CATEGORY, target: 603, option: 2 });
        assert_eq!(c2.encoder, CcFunction { category: ENCODER_LIGHTING_CATEGORY, target: 0, option: 1 });
        assert_eq!(get("/$ctl/user/1/1/bu/ch"), Value::Int(71));
        assert_eq!(get("/$ctl/user/1/1/bd/enc"), Value::str("M7"));
        assert_eq!(get("/$ctl/user/1/2/bu/tgt"), Value::str("MTX4"));
        assert_eq!(get("/$ctl/user/1/2/enc/other"), Value::str("BRIGLOW"));

        // The function names, as the console writes them (WING Edit 3.3.4).
        assert_eq!(get("/$ctl/user/1/1/bu/$fname"), Value::str("|MX3 START"));
        assert_eq!(get("/$ctl/user/1/1/bd/$fname"), Value::str("|DAW M7"));
        assert_eq!(get("/$ctl/user/1/2/bu/$fname"), Value::str("|MTX4 > R"));
        assert_eq!(get("/$ctl/user/1/2/enc/$fname"), Value::str("GLOW"));
        assert_eq!(get("/$ctl/user/1/3/bu/$fname"), Value::str(""), "not assigned");
        // The factory DAW layers (the dump of a factory-initialized WING).
        for (path, name) in [
            ("daw1/1/bu", "|DAW STOP"),
            ("daw1/1/bd", "|DAW REWIND"),
            ("daw1/2/bd", "|DAW FAST FWD"),
            ("daw1/3/bd", "|DAW *TIME"),
            ("daw1/4/bd", "|DAW UNDO"),
            ("daw3/3/bd", "|DAW READ/OFF"),
            ("daw4/1/bu", "|DAW *NAME"),
            ("daw4/2/bd", "|DAW V-POTS"),
            ("daw4/3/bd", "|DAW RECRDY"),
            ("daw4/4/bu", "|DAW ZOOM"),
        ] {
            assert_eq!(get(&format!("/$ctl/user/{path}/$fname")), Value::str(name), "{path}");
        }
        for (mode, field, value, name) in [
            ("MUTE", "ch", Value::Int(1), "|1 MUTE"),
            ("SOLO", "ch", Value::Int(51), "|B3 SOLO"),
            ("SOF", "ch", Value::Int(49), "|B1 |SOF"),
            ("INS1", "ch", Value::Int(41), "|A1 INS1"),
            ("MGRP", "mgrp", Value::str("MGRP.8"), "|MGRP MGRP.8"),
            ("DCAMUTE", "dca", Value::Int(2), "|DCA "),
            ("FXPAR", "fx", Value::str("FX3"), "|FX3 MIX"),
            ("CHPAGE", "ch", Value::Int(49), "|B1 |HOME"),
            ("FDRPAGE", "area", Value::str("L"), "|FDR L/1"),
            ("VIEWPAGE", "area", Value::str("C"), "|VIEW C/1"),
            ("OTHER", "other", Value::str("TBA"), "TALK A"),
            ("OTHER", "other", Value::str("FDROFF"), "FDR -oo"),
            ("SHOWCTL", "action", Value::str("GONEXT"), "|SCN GONEXT"),
            ("SCENES", "tag", Value::Int(1), "|SCENE 1"),
            ("MIDICCP", "cc", Value::Int(7), "CC 1:7"),
            ("MIDIPGM", "ch", Value::Int(2), "PGM 2:8"), // a MIDI key keeps its number (CC 7: program 8)
            ("USBPR", "usbpr", Value::str("RSTOP"), "|REC STOP"),
            ("DAWBTN", "btn", Value::str("OT4"), "|DAW *TIME"),
            ("PAGE", "pg", Value::str("SRC"), "SRC"),
            ("PAGE", "pg", Value::str("2TRK"), "2TRK"),
        ] {
            f("/$ctl/user/2/1/bu/mode", Value::str(mode)).unwrap();
            f(&format!("/$ctl/user/2/1/bu/{field}"), value).unwrap();
            sync(&mut custom);
            assert_eq!(get("/$ctl/user/2/1/bu/$fname"), Value::str(name), "{mode}");
        }
        // GLOBAL PAGE's SRC, OUTS and 2TRK are ROUTING's SOURCES, OUTPUTS and the USB recorder.
        for (pg, option) in [("CHINS", 3), ("SRC", 14), ("OUTS", 15), ("2TRK", 16)] {
            f("/$ctl/user/2/1/bu/pg", Value::str(pg)).unwrap();
            sync(&mut custom);
            assert_eq!(custom.control(1, 0).unwrap().upper.option, option, "{pg}");
            assert_eq!(get("/$ctl/user/2/1/bu/pg"), Value::str(pg));
        }
        for (mode, name) in
            [("FDR", "|1 FDR"), ("PAN", "|1 PAN"), ("SSND", "SEND |B1"), ("FSND", "|1 -> B1"), ("MIDICC", "CC 1:0")]
        {
            f("/$ctl/user/2/2/enc/mode", Value::str(mode)).unwrap();
            sync(&mut custom);
            assert_eq!(get("/$ctl/user/2/2/enc/$fname"), Value::str(name), "{mode}");
        }

        // COLOR > LIGHT (`led`): the colour stays while the light is off.
        f("/$ctl/user/3/1/col", Value::Int(5)).unwrap();
        sync(&mut custom);
        assert_eq!((custom.control(2, 0).unwrap().colour, get("/$ctl/user/3/1/led")), (-5, Value::Int(0)));
        f("/$ctl/user/3/1/led", Value::Int(1)).unwrap();
        sync(&mut custom);
        assert_eq!(custom.control(2, 0).unwrap().colour, 5);
        f("/$ctl/user/U2/1/led", Value::Int(1)).unwrap();
        f("/$ctl/user/3/1/led", Value::Int(0)).unwrap();
        sync(&mut custom);
        assert_eq!((custom.control(2, 0).unwrap().colour, get("/$ctl/user/3/1/col")), (-5, Value::Int(5)));
        assert_eq!(custom.control(25, 0).unwrap().colour, 1, "a light on without a colour: colour 1");
        // spidx 49-56: a matrix's spill, both ways.
        use livedaw_session::layers::{SpillKind, LEFT};
        f("/$ctl/layer/L/spidx", Value::Int(51)).unwrap();
        surface::sync(&st, &e, &mut layers, &mut custom, 0);
        assert_eq!(layers.spill(LEFT), Some(SpillKind::Matrix(2)));
        assert_eq!(get("/$ctl/layer/L/$spill/name"), Value::str("MTX 3 SPILL"));
        layers.start_spill(LEFT, SpillKind::Matrix(7));
        surface::sync(&st, &e, &mut layers, &mut custom, 0);
        assert_eq!(get("/$ctl/layer/L/spidx"), Value::Int(56));
    }

    #[test]
    fn the_virtual_section_is_the_compacts_virtual_layers() {
        use livedaw_session::custom::CustomControls;
        use livedaw_session::layers::{LayerSet, VIRTUAL_SECTION};
        let e = engine();
        let st = WingState::default();
        let c = Console::new(&e, &st);
        let (mut layers, mut custom) = (LayerSet::default(), CustomControls::default());
        layers.set_model(ConsoleModel::Compact);
        let mut sync = |layers: &mut LayerSet| surface::sync(&st, &e, layers, &mut custom, 0);
        sync(&mut layers);
        let get = |p: &str| c.get(node(&c, p));
        let f = |p: &str, v: Value| c.set(node(&c, p), &v);
        // Empty and unnamed at first, as on the console.
        assert_eq!(get("/$ctl/layer/VRT/1/name"), Some(Value::str("")));
        assert_eq!(get("/$ctl/layer/VRT/1/1/type"), Some(Value::str("OFF")));
        assert_eq!(get("/$ctl/layer/VRT/sel"), Some(Value::Int(1)));
        // A client names and fills layer 3 and selects it.
        f("/$ctl/layer/VRT/3/name", Value::str("Drums")).unwrap();
        f("/$ctl/layer/VRT/3/2/type", Value::str("CH")).unwrap();
        f("/$ctl/layer/VRT/3/2/i", Value::Int(5)).unwrap();
        f("/$ctl/layer/VRT/sel", Value::Int(3)).unwrap();
        f("/$ctl/layer/VRT/3/ofs", Value::Int(8)).unwrap();
        assert!(sync(&mut layers).layers);
        let vrt = layers.virtual_layer(2).unwrap();
        assert_eq!(vrt.name, "Drums");
        assert_eq!(vrt.faders.len(), 2);
        assert!(vrt.faders[0].is_empty());
        assert_eq!(layers.selected(VIRTUAL_SECTION), "vrt-3");
        assert_eq!(layers.offset(VIRTUAL_SECTION), 8);
        assert_eq!(get("/$ctl/layer/VRT/3/2/i"), Some(Value::Int(5)));
        assert_eq!(get("/$ctl/layer/VRT/3/ofs"), Some(Value::Int(8)));
        // The full-size console keeps the layers, but has no virtual section.
        layers.set_model(ConsoleModel::Full);
        sync(&mut layers);
        assert_eq!(get("/$ctl/layer/VRT/3/name"), Some(Value::str("Drums")));
        assert_eq!(get("/$ctl/layer/VRT/sel"), Some(Value::Int(1)));
        f("/$ctl/layer/VRT/sel", Value::Int(5)).unwrap();
        sync(&mut layers);
        assert_eq!(layers.selected(VIRTUAL_SECTION), "vrt-3");
    }

    #[test]
    fn sends_the_tree_cant_tell_apart_survive_a_write_back() {
        use livedaw_session::custom::CustomControls;
        use livedaw_session::layers::{FaderRef, LayerSet};
        use livedaw_session::refs::StripRef;
        let e = engine();
        let st = WingState::default();
        let c = Console::new(&e, &st);
        let (mut layers, mut custom) = (LayerSet::default(), CustomControls::default());
        let ch1 = StripRef::of(&e.strips(StripKind::Channel)[0]);
        let bus2 = StripRef::of(&e.strips(StripKind::Bus)[1]);
        let faders = vec![FaderRef::SelSend { to: bus2, bus_custom: false }, FaderRef::BusSend { from: ch1, to: bus2 }];
        layers.add_user_layer("Sends", faders.clone());
        let mut sync = |layers: &mut LayerSet| surface::sync(&st, &e, layers, &mut custom, 0);
        sync(&mut layers);
        // The selected channel's send reads OFF, the BUS CUSTOMIZATION send as a SEND.
        let get = |p: &str| c.get(node(&c, p)).unwrap();
        assert_eq!(get("/$ctl/layer/L/6/1/type"), Value::str("OFF"));
        assert_eq!(get("/$ctl/layer/L/6/2/type"), Value::str("SEND"));
        // A client writing back what it read (WING Edit's PC -> MIXER) changes nothing.
        for pos in 1..=2 {
            for f in ["type", "i", "dst"] {
                let path = format!("/$ctl/layer/L/6/{pos}/{f}");
                c.set(node(&c, &path), &get(&path)).unwrap();
            }
        }
        assert!(!sync(&mut layers).layers);
        assert_eq!(layers.user_layers()[0].faders, faders);
    }

    #[test]
    fn coverage_counts() {
        let cov = coverage();
        // The custom controls' function nodes (10,800: every mode's of every
        // key and encoder) are the schema's model parameters.
        assert_eq!(cov.total, 79326, "{}", cov.report());
        // Everything answers but DAW CONFIGURATION (see `unbound_feature`).
        assert_eq!(cov.mapped, 79325, "{}", cov.report());
        // 89 settings: the COMPACT's mfdr, cscmode, autosel_CMPCT, mfdrkeep, busspill, mainspill,
        // mtxspill and dcacc and the surface lock (cnslock) among them.
        assert_eq!((cov.settings, cov.stored), (89, 5472), "{}", cov.report());
        for (g, (m, t)) in &cov.by_group {
            assert!(m == t || g == "/$ctl/daw", "{g}: {m} of {t}");
        }
        assert_eq!(cov.by_group["/$ctl/daw"], (14, 15));
        // The engine-backed share: everything but the settings and the stored values.
        // (The COMPACT's key sets count here on either model; the routing: every
        // bus to every other bus, USR 1-56 and their user/..., the matrices' dir/in.)
        assert_eq!(cov.mapped - cov.settings - cov.stored, 73764);
        let b = bindings();
        let row = |p: &str| match schema().resolve(p, &NoModels) {
            Some(Node::Row(r)) => r as usize,
            _ => panic!("{p}"),
        };
        assert_eq!(b[row("/$ctl/daw/config")], Binding::None);
        assert_eq!(b[row("/cards/wlive/1/$stat/state")], Binding::Stored);
        assert_eq!(b[row("/$ctl/layer/CMPCT/sel")], Binding::Stored);
        assert!(matches!(b[row("/cfg/mtr/mtrpage/in")], Binding::Host(_)));
        assert!(matches!(b[row("/cfg/mtr/mainpos")], Binding::Host(_)));
    }

    fn get_text(c: &Console, path: &str) -> String {
        let n = node(c, path);
        value::display(c.def(n).unwrap(), &c.get(n).unwrap())
    }

    #[test]
    fn stored_parameters_answer_the_wings_factory_values() {
        let e = engine();
        let st = WingState::default();
        let c = Console::new(&e, &st);
        // As a factory-initialized WING answered (its dump).
        for (path, text) in [
            ("/$stat/usbvolname", "NO DRIVE"),
            ("/$syscfg/opt_mod", "DANTE"),
            ("/$syscfg/$hwversion", "SX45-XU2"),
            ("/$globals/usbacfg", "48/48"),
            ("/cards/wmadi/mode", "SFP1"),
            ("/io/in/$MON/1/name", "PHONES"),
            ("/io/in/$MON/3/icon", "503"),
            ("/io/in/$SEND/3/name", "FX SEND 2"),
            ("/io/in/USB/1/name", "USB 1/2"),
            ("/$ctl/lib/$scenes", ""),
            ("/play/$songs", ""),
            ("/fx/1/$emode", "ST"),
            ("/$ctl/$globals/mousespd", "1.77"),
            ("/$ctl/layer/CMPCT/sel", "1"),
            ("/bus/1/send/1/lvl", "-oo"), // a bus to itself
            ("/ch/1/$presolo", "0"),
            ("/cards/wlive/2/$stat/sessionlist", "2027-01-06 00:07:16 +10"),
        ] {
            assert_eq!(get_text(&c, path), text, "{path}");
            assert_eq!(c.binding(node(&c, path)), Binding::Stored, "{path}");
        }
        // The dumped console's own values (its address) aren't factory values.
        assert_eq!(get_text(&c, "/$syscfg/ip0"), "0");
        // Writable ones keep what a client sets; read-only ones stay.
        let usb = node(&c, "/$globals/usbacfg");
        assert_eq!(c.set(usb, &Value::str("8/8")), Ok(Value::str("8/8")));
        assert_eq!(c.get(usb), Some(Value::str("8/8")));
        assert_eq!(c.set(node(&c, "/cards/$type"), &Value::str("x")), Err(SetError::ReadOnly));
        // Every stored parameter the dump has answers its factory value.
        let s = schema();
        let mut checked = 0;
        for (r, b) in bindings().iter().enumerate() {
            if *b == Binding::Stored
                && s.defs[s.rows[r].def as usize].is_leaf()
                && st.shadow_get(ShadowKey::Row(r as u16)).is_none()
            {
                if let Some(v) = factory_value(r as u16) {
                    assert_eq!(c.get(Node::Row(r as u16)).as_ref(), Some(v));
                    checked += 1;
                }
            }
        }
        assert!(checked > 5100, "{checked}");
        // Unbound parameters answer the factory value too (where the strip is missing).
        let e8 = Engine::new(0);
        e8.set_num_channels(8);
        let c8 = Console::new(&e8, &st);
        assert_eq!(get_text(&c8, "/ch/20/fdr"), "-oo");
    }

    #[test]
    fn host_settings_both_ways() {
        let e = engine();
        let st = WingState::default();
        let c = Console::new(&e, &st);
        // Sidecar's defaults until the host publishes.
        assert_eq!(get_text(&c, "/cfg/mtr/mtrpage/in"), "PRE");
        assert_eq!(get_text(&c, "/cfg/mtr/mtrsfc/bus"), "POST");
        assert_eq!(get_text(&c, "/$ctl/daw/conn"), "USB");
        assert_eq!(get_text(&c, "/$ctl/cfg/seldblclick"), "BUSFX");
        assert_eq!(get_text(&c, "/$ctl/$globals/datefmt"), "YMD");
        // The host's values.
        st.set_host_setting("setup.meterTapPre.1", "1");
        st.set_host_setting("ui.selDblClick", "0");
        st.set_host_setting("ui.dateFormat", "DD-MM-YYYY");
        st.set_host_setting("daw.on", "true");
        st.set_host_setting("not.a.wing.setting", "1");
        assert_eq!(get_text(&c, "/cfg/mtr/mtrpage/bus"), "PRE");
        assert_eq!(get_text(&c, "/cfg/mtr/mtrsfc/bus"), "PRE", "one tap for the METERS screen and the surface");
        assert_eq!(get_text(&c, "/$ctl/cfg/seldblclick"), "HOME");
        assert_eq!(get_text(&c, "/$ctl/$globals/datefmt"), "DMY");
        assert_eq!(get_text(&c, "/$ctl/daw/on"), "1");
        // A client's changes reach the host as its settings.
        let set = |p: &str, v: Value| c.set(node(&c, p), &v);
        set("/cfg/mtr/mtrsfc/main", Value::str("PRE")).unwrap();
        set("/$ctl/cfg/seldblclick", Value::str("OFF")).unwrap();
        set("/$ctl/midi/enscenes", Value::str("USB")).unwrap();
        set("/$ctl/cfg/lights/meters", Value::Int(70)).unwrap();
        set("/$ctl/$globals/timefmt", Value::str("12H")).unwrap();
        set("/cfg/mtr/mainmtr", Value::str("MAIN.3")).unwrap();
        // Every MAIN METER SOURCE: the mains, matrices, phones, speakers and the selected channel.
        set("/cfg/mtr/mainmtr", Value::str("MTX.2")).unwrap();
        // The host's own value doesn't undo a change it hasn't taken.
        st.set_host_setting("setup.meterTapPre.2", "0");
        assert_eq!(get_text(&c, "/cfg/mtr/mtrpage/main"), "PRE");
        let mut taken = Vec::new();
        while let Some(kv) = st.take_host_setting() {
            taken.push(kv);
        }
        let taken: Vec<(&str, &str)> = taken.iter().map(|(k, v)| (*k, v.as_str())).collect();
        assert_eq!(
            taken,
            [
                ("setup.meterTapPre.2", "1"),
                ("ui.selDblClick", "2"),
                ("midi.enscenes", "2"),
                ("setup.brightness.meters", "70"),
                ("ui.clock24", "0"),
                ("ui.mainMeter", "6"),
            ]
        );
        st.set_host_setting("setup.meterTapPre.2", "0");
        assert_eq!(get_text(&c, "/cfg/mtr/mtrpage/main"), "POST");
        assert!(crate::host::keys().contains(&"daw.preset"));
        for (item, setting) in [("MON.PH", "13"), ("MON.SPK", "0"), ("SEL_CH", "14"), ("MTX.8", "12")] {
            st.set_host_setting("ui.mainMeter", setting);
            assert_eq!(get_text(&c, "/cfg/mtr/mainmtr"), item);
        }

        // MAIN METER TAP is a setting (AUTO, as on the console, until set).
        assert_eq!(get_text(&c, "/cfg/mtr/mainpos"), "AUTO");
        assert_eq!(set("/cfg/mtr/mainpos", Value::str("PRE")), Ok(Value::str("PRE")));
        assert_eq!(st.take_host_setting(), Some(("ui.mainMeterTap", "1".to_owned())));
        // The SETUP rows of features Sidecar doesn't have are settings too (kept only).
        assert_eq!(get_text(&c, "/$ctl/$globals/tapflash"), "ON");
        assert_eq!(set("/$ctl/$globals/tapflash", Value::str("8X")), Ok(Value::str("8X")));
        assert_eq!(st.take_host_setting(), Some(("setup.inert.tapTempoFlash", "1".to_owned())));
        assert_eq!(get_text(&c, "/$ctl/$globals/lockmtr"), "0");
        // The surface lock: SHOW METER PAGE WHEN LOCKED and the read-only lock string.
        st.set_host_setting("ui.meterPageWhenLocked", "1");
        assert_eq!(get_text(&c, "/$ctl/$globals/lockmtr"), "1");
        assert_eq!(get_text(&c, "/$ctl/$stat/cnslock"), "");
        st.set_host_setting("ui.surfaceLock", "1001001000000000000");
        assert_eq!(get_text(&c, "/$ctl/$stat/cnslock"), "1001001000000000000");
        assert_eq!(set("/$ctl/$stat/cnslock", Value::str("")), Err(SetError::ReadOnly));
        st.set_host_setting("ui.meterPageWhenLocked", "0");
        st.set_host_setting("setup.inert.mouseDisablesTouch", "1");
        assert_eq!(get_text(&c, "/$ctl/$globals/mousetchdis"), "1");
        for path in ["/$ctl/cfg/sfdr2solo", "/$ctl/osc/ronly", "/$syscfg/tcplock"] {
            assert!(set(path, Value::Int(1)).is_ok(), "{path}");
            assert!(st.take_host_setting().is_some_and(|(key, value)| key.starts_with("setup.inert.") && value == "1"), "{path}");
        }
        // The clock rate is the audio device's.
        assert_eq!(get_text(&c, "/$globals/clkrate"), "48000.0");
        assert!(matches!(set("/$globals/clkrate", Value::Float(44100.0)), Err(SetError::Unsupported(_))));
    }

    #[test]
    fn local_and_player_sources() {
        let e = engine();
        let st = WingState::default();
        let c = Console::new(&e, &st);
        let table = e.sources();
        let set = |p: &str, v: Value| c.set(node(&c, p), &v).unwrap();
        // LCL 3 is port 3 (A 3).
        set("/io/in/LCL/3/name", Value::str("Vox"));
        set("/io/in/LCL/3/col", Value::Int(5));
        set("/io/in/LCL/3/vph", Value::Int(1));
        assert_eq!(table.name(2), "Vox");
        assert_eq!(table.get(2).unwrap().colour.load(Relaxed), 5);
        assert!(table.get(2).unwrap().phantom.load(Relaxed));
        assert_eq!(get_text(&c, "/io/in/A/3/name"), "Vox");
        // The USB player is the PLAY group.
        let player = sources::USB_PLAYER_SOURCE_INDEX;
        set("/io/in/PLAY/1/mode", Value::str("ST"));
        set("/io/in/PLAY/2/name", Value::str("Walk-in"));
        set("/io/in/PLAY/2/mute", Value::Int(1));
        assert_eq!(table.name(player + 1), "Walk-in");
        assert!(table.get(player + 1).unwrap().mute.load(Relaxed));
        assert_eq!(table.get(player).unwrap().mode.load(Relaxed), source_mode::STEREO);
        assert_eq!(get_text(&c, "/io/in/PLAY/2/mode"), "ST");
        // Channels patch to it.
        set("/ch/6/in/conn/grp", Value::str("PLAY"));
        set("/ch/6/in/conn/in", Value::Int(2));
        let ch6 = c.strip(StripKind::Channel, 6).unwrap();
        assert_eq!(ch6.params.input_left.load(Relaxed), livedaw_engine::USB_PLAYER_SOURCE_BASE + 1);
        assert_eq!(get_text(&c, "/ch/6/in/conn/grp"), "PLAY");
        assert_eq!(get_text(&c, "/ch/6/$name"), "Walk-in");
    }

    #[test]
    fn local_outputs_and_recorder_taps() {
        let e = engine();
        let st = WingState::default();
        let c = Console::new(&e, &st);
        let set = |p: &str, v: Value| c.set(node(&c, p), &v).unwrap();
        // LOCAL OUT 3 is port 3 (A 3).
        set("/io/out/LCL/3/in", Value::Int(5));
        set("/io/out/LCL/3/grp", Value::str("BUS"));
        assert_eq!(c.strip(StripKind::Bus, 3).unwrap().params.output_left.load(Relaxed), 2);
        assert_eq!((get_text(&c, "/io/out/A/3/grp"), get_text(&c, "/io/out/A/3/in")), ("BUS".into(), "5".into()));
        // RECORDER ports: OFF by default (as on the WING), any bus, main, matrix or source.
        let usb = e.usb();
        assert_eq!((get_text(&c, "/io/out/REC/2/grp"), get_text(&c, "/io/out/REC/2/in")), ("OFF".into(), "1".into()));
        set("/io/out/REC/1/in", Value::Int(3));
        set("/io/out/REC/1/grp", Value::str("MTX"));
        assert_eq!(usb.tap(0), RecTap::Strip { kind: StripKind::Matrix as i32, number: 2, channel: 0 });
        set("/io/out/REC/2/grp", Value::str("B"));
        set("/io/out/REC/2/in", Value::Int(4));
        assert_eq!(usb.tap(1), RecTap::Source(51));
        assert_eq!((get_text(&c, "/io/out/REC/2/grp"), get_text(&c, "/io/out/REC/2/in")), ("B".into(), "4".into()));
        set("/io/out/REC/3/grp", Value::str("PLAY"));
        assert_eq!(usb.tap(2), RecTap::Source(livedaw_engine::USB_PLAYER_SOURCE_BASE));
        // FX sends can't be recorded: off, shown as set.
        set("/io/out/REC/4/grp", Value::str("SEND"));
        assert_eq!(usb.tap(3), RecTap::Off);
        assert_eq!(get_text(&c, "/io/out/REC/4/grp"), "SEND");
        set("/io/out/REC/4/grp", Value::str("OFF"));
        assert_eq!(get_text(&c, "/io/out/REC/4/grp"), "OFF");
        // Outputs Sidecar doesn't have are stored.
        assert_eq!(c.binding(node(&c, "/io/out/USB/1/grp")), Binding::Stored);
    }

    #[test]
    fn buses_mains_and_matrices_as_sources() {
        let e = engine();
        let st = WingState::default();
        let c = Console::new(&e, &st);
        let bus2 = c.strip(StripKind::Bus, 2).unwrap();
        bus2.set_name("Drums");
        bus2.params.colour.store(7, Relaxed);
        bus2.params.icon.store(300, Relaxed);
        c.set(node(&c, "/bus/2/tags"), &Value::str("#D4,band")).unwrap();
        for signal in [3, 4] {
            assert_eq!(get_text(&c, &format!("/io/in/$BUS/{signal}/name")), "Drums");
            assert_eq!(get_text(&c, &format!("/io/in/$BUS/{signal}/col")), "7");
            assert_eq!(get_text(&c, &format!("/io/in/$BUS/{signal}/icon")), "300");
            assert_eq!(get_text(&c, &format!("/io/in/$BUS/{signal}/mode")), "ST");
            assert_eq!(get_text(&c, &format!("/io/in/$BUS/{signal}/tags")), "#D4,band");
        }
        assert_eq!(get_text(&c, "/io/in/$MTX/16/col"), get_text(&c, "/mtx/8/col"));
        c.strip(StripKind::Main, 4).unwrap().set_name("Sub");
        assert_eq!(get_text(&c, "/io/in/$MAIN/8/name"), "Sub");
        assert_eq!(c.set(node(&c, "/io/in/$BUS/1/name"), &Value::str("x")), Err(SetError::ReadOnly));
    }

    #[test]
    fn channel_delay_is_described_in_meters() {
        let e = engine();
        let st = WingState::default();
        let c = Console::new(&e, &st);
        let d = c.def(node(&c, "/ch/3/in/set/dly")).unwrap();
        assert_eq!((d.unit, d.steps), (crate::schema::unit::METERS, 1500));
        assert!((d.min - 0.1).abs() < 1e-4 && (d.max - 150.0).abs() < 1e-3);
    }
}
