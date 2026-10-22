//! LIBRARY presets as files (the WING's FX and CHAN tabs, manual pp. 59-60).
//!
//! * FX preset (`.lfx`): an FX slot's device and parameter values, or its
//!   plugin's description and state. It loads into any slot; a premium
//!   device only into slots 1-8, a plugin's state only into a slot that hosts
//!   the same plugin (as scene recall: presets never create plugins).
//! * Channel preset (`.lchn`): a strip's settings with its scope (the
//!   sections it loads; UPDATE SCOPE changes it). It loads onto another strip
//!   of the same family: input and aux channels, or buses, mains and matrices.
//!
//! Files are XML, written atomically like show files.

use crate::console::{self, section, ApplyResult, InsertMode, PluginBridge};
use crate::refs::StripRef;
use crate::show::write_atomically;
use crate::xml::{self, Element};
use livedaw_dsp::wing::{model_index, FX_MODELS};
use livedaw_engine::params::NUM_FX;
use livedaw_engine::{Engine, StripKind};
use std::fs;
use std::path::Path;

pub const PRESET_VERSION: i32 = 1;
pub const FX_EXTENSION: &str = "lfx";
pub const CHANNEL_EXTENSION: &str = "lchn";

/// What a preset file holds.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PresetType {
    Fx,
    Channel,
}

/// A preset's header: its type, name, what it holds (the FX device's name,
/// or the strip kind it was saved from) and, for channel presets, its scope.
#[derive(Clone, Debug, PartialEq)]
pub struct PresetInfo {
    pub kind: PresetType,
    pub name: String,
    pub detail: String,
    pub scope: u32,
    pub strip_kind: Option<StripKind>,
}

/// The sections a channel preset can hold (the console's CHAN scope).
pub const CHANNEL_SECTIONS: u32 = section::ALL;
/// What a new channel preset loads: all of it but the input patch (CONN is
/// dim on the console's CHAN tab).
pub const DEFAULT_CHANNEL_SCOPE: u32 = section::ALL & !section::ROUTING;

fn root(kind: PresetType, name: &str) -> Element {
    let mut e = Element::new("LiveDAWPreset");
    e.set("type", if kind == PresetType::Fx { "fx" } else { "chan" }).set("version", PRESET_VERSION).set("name", name);
    e
}

/// Reads a preset's header.
pub fn info(preset: &Element) -> Option<PresetInfo> {
    if !preset.has_tag("LiveDAWPreset") || preset.int_or("version", 0) > PRESET_VERSION {
        return None;
    }
    let name = preset.str_or("name", "").to_owned();
    match preset.str_or("type", "") {
        "fx" => {
            let slot = preset.child("Slot")?;
            let detail = match console::plugin_element(slot) {
                Some(p) => p.str_or("name", "PLUGIN").to_owned(),
                None => slot.attr("mdl").and_then(|t| model_index(FX_MODELS, t)).map_or("NONE", |m| FX_MODELS[m].name).to_owned(),
            };
            Some(PresetInfo { kind: PresetType::Fx, name, detail, scope: 0, strip_kind: None })
        }
        "chan" => {
            let strip = preset.child("Strip")?;
            let kind = StripRef::parse(strip.str_or("ref", "")).kind;
            Some(PresetInfo {
                kind: PresetType::Channel,
                name,
                detail: kind.as_str().to_owned(),
                scope: preset.int_or("scope", DEFAULT_CHANNEL_SCOPE as i32) as u32 & CHANNEL_SECTIONS,
                strip_kind: Some(kind),
            })
        }
        _ => None,
    }
}

// ---- FX presets ---------------------------------------------------------------------------

/// SAVE on the FX tab: FX slot `slot` as a preset.
pub fn capture_fx(engine: &Engine, bridge: &mut dyn PluginBridge, slot: usize, name: &str) -> Option<Element> {
    if slot >= NUM_FX {
        return None;
    }
    let mut preset = root(PresetType::Fx, name);
    let mut x = console::capture_fx_slot(engine, slot, bridge);
    x.attrs.retain(|(k, _)| k != "i");
    preset.add(x);
    Some(preset)
}

/// LOAD on the FX tab: the preset into FX slot `slot`. Ok carries warnings
/// (a plugin that isn't loaded there, a premium device in slots 9-16).
pub fn apply_fx(engine: &Engine, bridge: &mut dyn PluginBridge, preset: &Element, slot: usize) -> Result<Vec<String>, String> {
    let info = info(preset).ok_or("Not a Sidecar preset")?;
    if info.kind != PresetType::Fx {
        return Err("Not an FX preset".into());
    }
    if slot >= NUM_FX {
        return Err(format!("FX {} doesn't exist", slot + 1));
    }
    let x = preset.child("Slot").ok_or("The preset holds no FX slot")?;
    // Like a recall: a plugin's state only goes into the same plugin.
    let mut warnings = Vec::new();
    console::apply_fx_slot(engine, x, slot, Some(InsertMode::StateOnly), bridge, &mut warnings);
    Ok(warnings)
}

// ---- channel presets ------------------------------------------------------------------------

fn family(kind: StripKind) -> u8 {
    if kind.is_input() {
        0
    } else {
        1
    }
}

/// SAVE on the CHAN tab: a strip's settings (all sections) with `scope` as
/// what the preset loads.
pub fn capture_channel(
    engine: &Engine,
    bridge: &mut dyn PluginBridge,
    strip: &StripRef,
    scope: u32,
    name: &str,
) -> Option<Element> {
    let state = console::capture(engine, CHANNEL_SECTIONS, Some(std::slice::from_ref(strip)), bridge);
    let s = state.children_named("Strip").next()?.clone();
    let mut preset = root(PresetType::Channel, name);
    preset.set("scope", scope & CHANNEL_SECTIONS);
    preset.add(s);
    Some(preset)
}

/// UPDATE SCOPE: what the preset loads from now on.
pub fn set_channel_scope(preset: &mut Element, scope: u32) {
    preset.set("scope", scope & CHANNEL_SECTIONS);
}

/// LOAD on the CHAN tab: the preset onto `target` (its own scope, or `scope`).
pub fn apply_channel(
    engine: &Engine,
    bridge: &mut dyn PluginBridge,
    preset: &Element,
    target: &StripRef,
    scope: Option<u32>,
) -> Result<ApplyResult, String> {
    let info = info(preset).ok_or("Not a Sidecar preset")?;
    if info.kind != PresetType::Channel {
        return Err("Not a channel preset".into());
    }
    let saved = info.strip_kind.ok_or("The preset holds no strip")?;
    if family(saved) != family(target.kind) {
        let what = |k: StripKind| if k.is_input() { "input and aux channels" } else { "buses, mains and matrices" };
        return Err(format!("A preset of {} doesn't load onto {}", what(saved), what(target.kind)));
    }
    let mut strip = preset.child("Strip").ok_or("The preset holds no strip")?.clone();
    strip.set("ref", target);
    let mut state = Element::new("State");
    state.set("version", console::CONSOLE_VERSION).set("sections", CHANNEL_SECTIONS);
    state.add(strip);
    let sections = scope.unwrap_or(info.scope) & CHANNEL_SECTIONS;
    let result = console::apply(engine, &state, sections, Some(std::slice::from_ref(target)), false, None, bridge);
    if result.strips_applied == 0 {
        return Err(format!("{target} doesn't exist"));
    }
    Ok(result)
}

// ---- files -------------------------------------------------------------------------------

pub fn save(path: &Path, preset: &Element) -> Result<(), String> {
    if let Some(dir) = path.parent().filter(|d| !d.as_os_str().is_empty()) {
        fs::create_dir_all(dir).map_err(|e| format!("Could not create {}: {e}", dir.display()))?;
    }
    write_atomically(path, &preset.to_document())
}

pub fn load(path: &Path) -> Result<Element, String> {
    let text = fs::read_to_string(path).map_err(|e| format!("Could not read {}: {e}", path.display()))?;
    let xml = xml::parse(&text).map_err(|e| format!("Not a valid preset ({e})"))?;
    info(&xml).ok_or_else(|| "Not a Sidecar preset".to_owned())?;
    Ok(xml)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn headers() {
        let mut fx = root(PresetType::Fx, "Plate");
        fx.add_new("Slot").set("mdl", FX_MODELS[3].token);
        let i = info(&fx).unwrap();
        assert_eq!((i.kind, i.name.as_str(), i.detail.as_str()), (PresetType::Fx, "Plate", FX_MODELS[3].name));

        let mut ch = root(PresetType::Channel, "Kick");
        ch.add_new("Strip").set("ref", "ch:4");
        let i = info(&ch).unwrap();
        assert_eq!((i.kind, i.scope, i.strip_kind), (PresetType::Channel, DEFAULT_CHANNEL_SCOPE, Some(StripKind::Channel)));
        set_channel_scope(&mut ch, section::EQ | 1 << 20);
        assert_eq!(info(&ch).unwrap().scope, section::EQ);

        assert!(info(&Element::new("LiveDAWShow")).is_none());
        let mut newer = root(PresetType::Fx, "x");
        newer.set("version", PRESET_VERSION + 1);
        assert!(info(&newer).is_none());
    }
}
