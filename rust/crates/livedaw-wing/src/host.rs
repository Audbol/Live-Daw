//! The host's settings that are WING parameters: SETUP's surface, metering,
//! solo, MIDI and DAW options (`/$ctl/$globals`, `/$ctl/cfg`, `/$ctl/midi`,
//! `/$ctl/daw`, `/cfg/mtr`).
//!
//! The application keeps them in its settings file, as text under a key
//! ("ui.faderSpeed" = "1"); this module knows which WING node each key is and
//! how its values translate. The host publishes the values it has
//! ([`WingState::set_host_setting`], unset keys read as Sidecar's default
//! here) and stores what clients change ([`WingState::take_host_setting`]);
//! without a host a client reads back what it set.

use crate::map::{SetError, WingState};
use crate::schema::Def;
use crate::value::{self, Value};
use std::collections::{HashMap, VecDeque};

/// How a node's value is stored in the setting.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Enc {
    /// 0 / 1 as "0" / "1".
    Bool,
    /// An enum's item index ("2" for FAST of SLOW, MED, FAST).
    Index,
    /// An integer as is.
    Int,
    /// (item index, setting text) pairs; items without a pair can't be set.
    Map(&'static [(usize, &'static str)]),
    /// An enum's item as its own text ("MAIN.1", "BUS").
    Item,
}

/// One setting: the WING node, the application's key, Sidecar's default and the encoding.
#[derive(Clone, Copy, Debug)]
pub struct HostSetting {
    pub path: &'static str,
    pub key: &'static str,
    pub default: &'static str,
    pub enc: Enc,
}

const fn s(path: &'static str, key: &'static str, default: &'static str, enc: Enc) -> HostSetting {
    HostSetting { path, key, default, enc }
}

/// PRE (item 0) / POST as a "PRE" flag.
const PRE_FLAG: Enc = Enc::Map(&[(0, "1"), (1, "0")]);

/// Every bridged setting. Several nodes can share a key: the WING sets the
/// METERS screen's and the surface's meter taps apart, Sidecar has one tap
/// per strip type for both.
pub const HOST_SETTINGS: &[HostSetting] = &[
    // SETUP > SURFACE > METERING (prefs::meterTapKey): CHANNEL, BUS, MAIN, MATRIX, DCA.
    s("/cfg/mtr/mtrpage/in", "setup.meterTapPre.0", "1", PRE_FLAG),
    s("/cfg/mtr/mtrpage/bus", "setup.meterTapPre.1", "0", PRE_FLAG),
    s("/cfg/mtr/mtrpage/main", "setup.meterTapPre.2", "0", PRE_FLAG),
    s("/cfg/mtr/mtrpage/mtx", "setup.meterTapPre.3", "0", PRE_FLAG),
    s("/cfg/mtr/mtrpage/dca", "setup.meterTapPre.4", "1", PRE_FLAG),
    s("/cfg/mtr/mtrsfc/in", "setup.meterTapPre.0", "1", PRE_FLAG),
    s("/cfg/mtr/mtrsfc/bus", "setup.meterTapPre.1", "0", PRE_FLAG),
    s("/cfg/mtr/mtrsfc/main", "setup.meterTapPre.2", "0", PRE_FLAG),
    s("/cfg/mtr/mtrsfc/mtx", "setup.meterTapPre.3", "0", PRE_FLAG),
    s("/cfg/mtr/mtrsfc/dca", "setup.meterTapPre.4", "1", PRE_FLAG),
    // MAIN METER: 0 the monitor (SPEAKERS), 1-4 MAIN n; MAIN METER TAP: AUTO, PRE, POST.
    s("/cfg/mtr/mainmtr", "ui.mainMeter", "1", Enc::Map(&[(0, "1"), (1, "2"), (2, "3"), (3, "4"), (13, "0")])),
    s("/cfg/mtr/mainpos", "ui.mainMeterTap", "0", Enc::Index),
    // SETUP > GENERAL.
    s("/$ctl/$globals/cf_load", "setup.confirmLibraryLoad", "1", Enc::Bool),
    s("/$ctl/$globals/showscene", "ui.alwaysShowScene", "0", Enc::Bool),
    s("/$ctl/$globals/usewheel", "ui.useCursorWheel", "1", Enc::Bool),
    s("/$ctl/$globals/fdrsel", "ui.touchFaderSelect", "0", Enc::Bool),
    s("/$ctl/$globals/fdrres", "setup.touchFaderRes", "2", Enc::Index),
    s("/$ctl/$globals/timefmt", "ui.clock24", "1", Enc::Map(&[(0, "1"), (1, "0")])),
    s("/$ctl/$globals/datefmt", "ui.dateFormat", "YYYY-MM-DD", Enc::Map(&[(0, "YYYY-MM-DD"), (1, "DD-MM-YYYY")])),
    // SETUP > SURFACE.
    s("/$ctl/$globals/fdrspd", "ui.faderSpeed", "1", Enc::Index),
    s("/$ctl/$globals/srcdisp", "ui.sourceOnScribble", "1", Enc::Bool),
    s("/$ctl/cfg/showfdr", "ui.faderValueOnScribble", "0", Enc::Bool),
    s("/$ctl/cfg/usrmode", "ui.f1f3AsCustomControls", "0", Enc::Index),
    s("/$ctl/cfg/autoview", "ui.screenFollowsChStrip", "0", Enc::Bool),
    s("/$ctl/cfg/csctouch", "ui.chStripTouchSelect", "1", Enc::Bool),
    s("/$ctl/cfg/seldblclick", "ui.selDblClick", "1", Enc::Map(&[(0, "2"), (1, "0"), (2, "1")])),
    s("/$ctl/cfg/sofbutton", "ui.sofButton", "0", Enc::Index),
    s("/$ctl/cfg/sofframe", "ui.showSofFrame", "1", Enc::Bool),
    s("/$ctl/cfg/sofmode", "ui.alternativeSof", "0", Enc::Bool),
    s("/$ctl/cfg/soffdr", "ui.rightSectionSof", "0", Enc::Index),
    s("/$ctl/cfg/fdrbanking", "ui.fullFaderPaging", "0", Enc::Bool),
    s("/$ctl/cfg/autosel_L", "ui.autoSelectL", "0", Enc::Bool),
    s("/$ctl/cfg/autosel_C", "ui.autoSelectC", "0", Enc::Bool),
    s("/$ctl/cfg/autosel_R", "ui.autoSelectR", "0", Enc::Bool),
    s("/$ctl/cfg/layerlinkl", "ui.userLayerLinkLC", "0", Enc::Bool),
    s("/$ctl/cfg/layerlinkr", "ui.userLayerLinkCR", "0", Enc::Bool),
    s("/$ctl/cfg/dcaspill", "ui.dcaSpill", "0", Enc::Bool),
    s("/$ctl/cfg/fdrscrnlink", "setup.faderScreenLayerLink", "0", Enc::Bool),
    // The WING COMPACT's surface (CONSOLE MODEL COMPACT): CH AUTOSELECT of its one
    // section, what fader 13 controls and what its 16 keys do.
    s("/$ctl/cfg/autosel_CMPCT", "ui.autoSelectCompact", "0", Enc::Bool),
    s("/$ctl/cfg/mfdr", "ui.compactMainFader", "MAIN.1", Enc::Item),
    s("/$ctl/cfg/cscmode", "ui.compactKeyMode", "BUS", Enc::Item),
    // SETUP > AUDIO > SOLO SYSTEM.
    s("/$ctl/cfg/soloexcl", "ui.soloExclusive", "1", Enc::Bool),
    s("/$ctl/cfg/solofsel", "ui.soloFollowsSelect", "0", Enc::Bool),
    s("/$ctl/cfg/selfsolo", "ui.selectFollowsSolo", "1", Enc::Bool),
    s("/$ctl/cfg/sof2solo", "ui.sofActivatesSolo", "0", Enc::Bool),
    // SETUP > GENERAL's brightness tiles (setup.brightness.<id>, percent).
    s("/$ctl/cfg/lights/main", "setup.brightness.mainLcd", "80", Enc::Int),
    s("/$ctl/cfg/lights/chedit", "setup.brightness.btnLcd", "80", Enc::Int),
    s("/$ctl/cfg/lights/leds", "setup.brightness.btnsLeds", "90", Enc::Int),
    s("/$ctl/cfg/lights/btns", "setup.brightness.backlight", "25", Enc::Int),
    s("/$ctl/cfg/lights/meters", "setup.brightness.meters", "40", Enc::Int),
    s("/$ctl/cfg/lights/rgbleds", "setup.brightness.colorLed", "25", Enc::Int),
    s("/$ctl/cfg/lights/chlcds", "setup.brightness.scribble", "60", Enc::Int),
    s("/$ctl/cfg/lights/chlcdctr", "setup.brightness.scrCont", "50", Enc::Int),
    // SETUP > REMOTE > MIDI REMOTE CONTROL: OFF, DIN, USB.
    s("/$ctl/midi/enchctl", "midi.enchctl", "0", Enc::Index),
    s("/$ctl/midi/enfxctl", "midi.enfxctl", "0", Enc::Index),
    s("/$ctl/midi/encustctl", "midi.encustctl", "0", Enc::Index),
    s("/$ctl/midi/ensysex", "midi.ensysex", "0", Enc::Index),
    s("/$ctl/midi/enmidicc", "midi.enmidicc", "0", Enc::Index),
    s("/$ctl/midi/enscenes", "midi.enscenes", "0", Enc::Index),
    s("/$ctl/midi/enshowctl", "midi.enshowctl", "0", Enc::Index),
    s("/$ctl/midi/enscenetx", "midi.enscenetx", "0", Enc::Index),
    // SETUP > DAW.
    s("/$ctl/daw/on", "daw.on", "0", Enc::Bool),
    s("/$ctl/daw/conn", "daw.conn", "1", Enc::Index),
    s("/$ctl/daw/emul", "daw.emul", "0", Enc::Index),
    s("/$ctl/daw/ccup", "daw.ccup", "0", Enc::Bool),
    s("/$ctl/daw/disjog", "daw.disjog", "1", Enc::Bool),
    s("/$ctl/daw/preset", "daw.preset", "0", Enc::Index),
    // SETUP rows of features Sidecar doesn't have (custom control tap keys that
    // don't flash, a screen lock, a mouse, SEND PAGE solo, remote locks): kept
    // as settings (`setup.inert.*`), shown and switched as on the console.
    s("/$ctl/$globals/tapflash", "setup.inert.tapTempoFlash", "2", Enc::Index),
    s("/$ctl/$globals/lockmtr", "setup.inert.meterPageWhenLocked", "0", Enc::Bool),
    s("/$ctl/$globals/mousetchdis", "setup.inert.mouseDisablesTouch", "0", Enc::Bool),
    s("/$ctl/cfg/sfdr2solo", "setup.inert.sendPageActivatesSolo", "0", Enc::Bool),
    s("/$ctl/osc/ronly", "setup.inert.lockOsc", "0", Enc::Bool),
    s("/$syscfg/tcplock", "setup.inert.lockTcp", "0", Enc::Bool),
];

/// The setting of a node, by its path.
pub fn find(path: &str) -> Option<u8> {
    HOST_SETTINGS.iter().position(|h| h.path == path).map(|i| i as u8)
}

/// The settings' keys (each once), for the host to publish.
pub fn keys() -> Vec<&'static str> {
    let mut out: Vec<&'static str> = Vec::new();
    for h in HOST_SETTINGS {
        if !out.contains(&h.key) {
            out.push(h.key);
        }
    }
    out
}

/// The values the host published and the changes clients made.
#[derive(Default)]
pub struct HostValues {
    values: HashMap<&'static str, String>,
    /// Keys a client changed that the host hasn't taken yet.
    pending: VecDeque<&'static str>,
}

/// "true" / "false" as the settings file may also hold them.
fn normalise(text: &str) -> &str {
    match text.trim() {
        "true" => "1",
        "false" => "0",
        t => t,
    }
}

fn static_key(key: &str) -> Option<&'static str> {
    HOST_SETTINGS.iter().find(|h| h.key == key).map(|h| h.key)
}

impl WingState {
    /// Host: publishes a setting's value (unless a client's change to it is still to be taken).
    pub fn set_host_setting(&self, key: &str, value: &str) {
        let Some(key) = static_key(key) else { return };
        let mut h = self.host.lock().unwrap_or_else(|e| e.into_inner());
        if !h.pending.contains(&key) {
            h.values.insert(key, value.to_owned());
        }
    }

    /// Host: the next setting a client changed, with its new value.
    pub fn take_host_setting(&self) -> Option<(&'static str, String)> {
        let mut h = self.host.lock().unwrap_or_else(|e| e.into_inner());
        let key = h.pending.pop_front()?;
        let value = h.values.get(key).cloned().unwrap_or_default();
        Some((key, value))
    }

    /// Host: forgets the published values and the pending changes.
    pub fn clear_host_settings(&self) {
        *self.host.lock().unwrap_or_else(|e| e.into_inner()) = HostValues::default();
    }

    fn host_value(&self, h: &HostSetting) -> String {
        let v = self.host.lock().unwrap_or_else(|e| e.into_inner());
        v.values.get(h.key).map(|t| normalise(t).to_owned()).unwrap_or_else(|| h.default.to_owned())
    }

    fn change_host_value(&self, key: &'static str, text: String) {
        let mut h = self.host.lock().unwrap_or_else(|e| e.into_inner());
        h.values.insert(key, text);
        if !h.pending.contains(&key) {
            h.pending.push_back(key);
        }
    }
}

/// A setting as the node's value.
pub fn get(state: &WingState, index: u8, def: &Def) -> Option<Value> {
    let h = HOST_SETTINGS.get(index as usize)?;
    let text = state.host_value(h);
    let index_value = |i: usize| value::item_value(def, i);
    match h.enc {
        Enc::Bool => Some(Value::Int((text != "0" && !text.is_empty()) as i32)),
        Enc::Int => text.parse::<i32>().ok().or_else(|| h.default.parse().ok()).map(Value::Int),
        Enc::Index => text.parse::<usize>().ok().and_then(index_value).or_else(|| index_value(h.default.parse().ok()?)),
        Enc::Map(pairs) => {
            let item = |t: &str| pairs.iter().find(|(_, s)| *s == t).map(|(i, _)| *i);
            item(&text).or_else(|| item(h.default)).and_then(index_value)
        }
        Enc::Item => {
            let item = |t: &str| def.items().iter().position(|it| it.item.eq_ignore_ascii_case(t.trim()));
            item(&text).or_else(|| item(h.default)).and_then(index_value)
        }
    }
}

/// Sets a setting from a node value (already coerced to the node's type).
pub fn set(state: &WingState, index: u8, def: &Def, v: &Value) -> Result<(), SetError> {
    let h = HOST_SETTINGS.get(index as usize).ok_or(SetError::NotFound)?;
    let text = match h.enc {
        Enc::Bool => (if v.as_bool() { "1" } else { "0" }).to_owned(),
        Enc::Int => v.as_i32().ok_or(SetError::BadValue)?.to_string(),
        Enc::Index => value::enum_index(def, v).ok_or(SetError::BadValue)?.to_string(),
        Enc::Map(pairs) => {
            let i = value::enum_index(def, v).ok_or(SetError::BadValue)?;
            match pairs.iter().find(|(k, _)| *k == i) {
                Some((_, t)) => (*t).to_owned(),
                None => {
                    let item = def.items().get(i).map_or("", |it| it.item);
                    return Err(SetError::Unsupported(format!("Sidecar has no {item} here")));
                }
            }
        }
        Enc::Item => {
            let i = value::enum_index(def, v).ok_or(SetError::BadValue)?;
            def.items().get(i).ok_or(SetError::BadValue)?.item.to_owned()
        }
    };
    state.change_host_value(h.key, text);
    Ok(())
}
