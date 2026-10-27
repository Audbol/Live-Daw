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
    /// A string node's text as is.
    Text,
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

/// `/cfg/mtr/mainmtr` items to `ui.mainMeter` (MeterBank.h's `mainmeter`).
const MAIN_METER: &[(usize, &str)] = &[
    (0, "1"),
    (1, "2"),
    (2, "3"),
    (3, "4"),
    (4, "5"),
    (5, "6"),
    (6, "7"),
    (7, "8"),
    (8, "9"),
    (9, "10"),
    (10, "11"),
    (11, "12"),
    (12, "13"),
    (13, "0"),
    (14, "14"),
];

/// Every bridged setting.
pub const HOST_SETTINGS: &[HostSetting] = &[
    // METERS > SETTINGS (prefs::meterPageTapKey) and SETUP > SURFACE > METERING
    // (prefs::meterTapKey, the surface's meters): CHANNEL, BUS, MAIN, MATRIX, DCA.
    s("/cfg/mtr/mtrpage/in", "setup.meterPageTapPre.0", "1", PRE_FLAG),
    s("/cfg/mtr/mtrpage/bus", "setup.meterPageTapPre.1", "0", PRE_FLAG),
    s("/cfg/mtr/mtrpage/main", "setup.meterPageTapPre.2", "0", PRE_FLAG),
    s("/cfg/mtr/mtrpage/mtx", "setup.meterPageTapPre.3", "0", PRE_FLAG),
    s("/cfg/mtr/mtrpage/dca", "setup.meterPageTapPre.4", "1", PRE_FLAG),
    s("/cfg/mtr/mtrsfc/in", "setup.meterTapPre.0", "1", PRE_FLAG),
    s("/cfg/mtr/mtrsfc/bus", "setup.meterTapPre.1", "0", PRE_FLAG),
    s("/cfg/mtr/mtrsfc/main", "setup.meterTapPre.2", "0", PRE_FLAG),
    s("/cfg/mtr/mtrsfc/mtx", "setup.meterTapPre.3", "0", PRE_FLAG),
    s("/cfg/mtr/mtrsfc/dca", "setup.meterTapPre.4", "1", PRE_FLAG),
    // MAIN METER SOURCE (manual 5.5 p. 47; MAIN.1-4, MTX.1-8, MON.PH, MON.SPK, SEL_CH):
    // 0 the speakers, 1-4 MAIN n, 5-12 MTX n, 13 the phones, 14 the selected strip.
    // MAIN METER TAP: AUTO, PRE, POST.
    s("/cfg/mtr/mainmtr", "ui.mainMeter", "1", Enc::Map(MAIN_METER)),
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
    // SETUP > SURFACE on a COMPACT (WING manual 5.5): KEEP MAIN FADER (the keys
    // don't move fader 13); BUS, MAIN and MATRIX SPILL, whether a key's press
    // spills (and its hold turns on Sends on Fader) or the other way round
    // (factory: BUS and MAIN send on a press, MATRIX spills); USE DCA BUTTONS
    // 1-16 AS CC, the DCA keys play the sets DCA 1-4 .. 13-16. DCA SPILL ON
    // SELECT, the COMPACT's one DCA spill setting, is `dcaspill` above (a DCA
    // strip's SELECT spills it; a DCA key always spills and shows its DCA).
    s("/$ctl/cfg/mfdrkeep", "ui.compactKeepMainFader", "0", Enc::Bool),
    s("/$ctl/cfg/busspill", "ui.compactBusSpill", "0", Enc::Bool),
    s("/$ctl/cfg/mainspill", "ui.compactMainSpill", "0", Enc::Bool),
    s("/$ctl/cfg/mtxspill", "ui.compactMatrixSpill", "1", Enc::Bool),
    s("/$ctl/cfg/dcacc", "ui.compactDcaCustom", "0", Enc::Bool),
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
    // PATCH, GLOW and LAMP (no such lights here: kept, and the encoders'
    // OTHER BRIPATCH, BRIGLOW, BRILAMP set them).
    s("/$ctl/cfg/lights/patch", "setup.brightness.patch", "0", Enc::Int),
    s("/$ctl/cfg/lights/glow", "setup.brightness.glow", "0", Enc::Int),
    s("/$ctl/cfg/lights/lamp", "setup.brightness.lamp", "0", Enc::Int),
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
    // Surface lock (manual chapter 10, hold HOME): SHOW METER PAGE WHEN LOCKED,
    // and the lock string (read-only: '1' for each key held to lock, chars 1-7
    // the keys left of the display from HOME, 18-19 those right of it; empty
    // while unlocked). The host keeps it while it runs.
    s("/$ctl/$globals/lockmtr", "ui.meterPageWhenLocked", "0", Enc::Bool),
    s("/$ctl/$stat/cnslock", "ui.surfaceLock", "", Enc::Text),
    // The recorder screen's SETTINGS (the WING-LIVE's, manual 8.1 p. 85-86):
    // SHOW METERS, AUTO INPUT SELECT (OFF, 1, 2: Sidecar has one recorder,
    // both cards read back as 1) with the [STOP] / [PLAY] / [RECORD] INPUT
    // choices KEEP, MAIN, ALT; and the player's MODE PLAY, A->B, LOOP (fig.
    // 75), one for both cards.
    s("/cards/wlive/meters", "recorder.showMeters", "1", Enc::Bool),
    s("/cards/wlive/autoin", "recorder.autoInput", "0", Enc::Map(&[(0, "0"), (1, "1"), (2, "1")])),
    s("/cards/wlive/auto_stop", "recorder.autoStop", "0", Enc::Index),
    s("/cards/wlive/auto_play", "recorder.autoPlay", "0", Enc::Index),
    s("/cards/wlive/auto_rec", "recorder.autoRec", "0", Enc::Index),
    s("/cards/wlive/1/cfg/playmode", "recorder.playMode", "0", Enc::Index),
    s("/cards/wlive/2/cfg/playmode", "recorder.playMode", "0", Enc::Index),
    // Under the keys they had while their rows did nothing (`setup.inert.*`):
    // TAP TEMPO FLASH, SEND PAGE ACTIVATES SOLO (VIEW > SENDS solos the
    // destination chosen); and features Sidecar doesn't have (a mouse, remote
    // locks), kept as settings, shown and switched as on the console.
    s("/$ctl/$globals/tapflash", "setup.inert.tapTempoFlash", "2", Enc::Index),
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
        Enc::Text => Some(Value::Str(text)),
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
        Enc::Text => match v {
            Value::Str(s) => s.clone(),
            _ => return Err(SetError::BadValue),
        },
    };
    state.change_host_value(h.key, text);
    Ok(())
}

#[cfg(test)]
mod tests {
    use crate::map::{Console, WingState};
    use crate::schema::schema;
    use crate::value::{self, Value};
    use livedaw_engine::Engine;

    fn text(c: &Console, path: &str) -> String {
        let n = schema().resolve(path, c).unwrap_or_else(|| panic!("{path}"));
        value::display(c.def(n).unwrap(), &c.get(n).unwrap())
    }

    #[test]
    fn recorder_settings_are_host_settings() {
        // The WING-LIVE SETTINGS (manual 8.1 p. 85-86) and the cards' MODE.
        let e = Engine::new(0);
        let st = WingState::default();
        let c = Console::new(&e, &st);
        assert_eq!(text(&c, "/cards/wlive/autoin"), "OFF");
        assert_eq!(text(&c, "/cards/wlive/auto_play"), "KEEP");
        assert_eq!(text(&c, "/cards/wlive/meters"), "1");
        assert_eq!(text(&c, "/cards/wlive/1/cfg/playmode"), "PLAY");
        st.set_host_setting("recorder.autoInput", "1");
        st.set_host_setting("recorder.autoPlay", "2");
        st.set_host_setting("recorder.playMode", "1");
        assert_eq!(text(&c, "/cards/wlive/autoin"), "1");
        assert_eq!(text(&c, "/cards/wlive/auto_play"), "ALT");
        assert_eq!(text(&c, "/cards/wlive/1/cfg/playmode"), "A->B");
        assert_eq!(text(&c, "/cards/wlive/2/cfg/playmode"), "A->B", "one mode for both cards");

        let set = |p: &str, v: &str| c.set(schema().resolve(p, &c).unwrap(), &Value::str(v));
        set("/cards/wlive/autoin", "2").unwrap();
        set("/cards/wlive/auto_rec", "MAIN").unwrap();
        set("/cards/wlive/2/cfg/playmode", "LOOP").unwrap();
        let taken: Vec<_> = std::iter::from_fn(|| st.take_host_setting()).collect();
        let taken: Vec<(&str, &str)> = taken.iter().map(|(k, v)| (*k, v.as_str())).collect();
        assert_eq!(taken, [("recorder.autoInput", "1"), ("recorder.autoRec", "1"), ("recorder.playMode", "2")]);
        assert_eq!(text(&c, "/cards/wlive/autoin"), "1", "card 2 reads back as Sidecar's one recorder");
    }
}
