//! WING snapshot (`.snap`) and show (`.show`) files.
//!
//! A snapshot is JSON: a few description fields (`type`, `creator_*`,
//! `created`...), optional `scopes`, and the parameter tree in four sections:
//! `ae_data` (the audio engine: the root's nodes), `ce_data` (`/$ctl`, the
//! control surface), `ae_globals` (`/$globals`) and `ce_globals`
//! (`/$ctl/$globals`). Numbered nodes are objects keyed "1", "2"...; 0/1
//! switches are booleans; faders at -oo are -144; enums are their item text;
//! read-only and `$` nodes aren't saved. (Protocol document appendix "WING
//! Snapshot and JSON Data Structure", snapshots saved by consoles on
//! firmware 2.1 and 3.1, and shows saved by consoles and by WING Edit.)
//!
//! A show (`showfile.1`) lists scenes that refer to snapshot files by path
//! (relative to the show, or console paths such as `I:/library/...`).

use crate::json::{self, Json};
use crate::map::{bindings, Binding, Console, SetError, WingState};
use crate::schema::{schema, Node, NodeType};
use crate::value::Value;
use livedaw_engine::Engine;
use livedaw_session::console::{load_console, save_console, NoPlugins};
use livedaw_session::Show;
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

/// The snapshot format written on export (firmware 3.1's).
pub const SNAPSHOT_TYPE: &str = "snapshot.11";

#[derive(Clone, Debug)]
pub struct ImportOptions {
    /// Grow the console to the snapshot's channel count (the WING's 40).
    pub grow_channels: bool,
}

impl Default for ImportOptions {
    fn default() -> Self {
        Self { grow_channels: true }
    }
}

/// What an import did.
#[derive(Clone, Debug, Default)]
pub struct ImportReport {
    pub file_type: String,
    pub creator: String,
    /// Values applied to the engine.
    pub applied: usize,
    /// Values kept for nodes Live DAW has no equivalent for (they don't
    /// change the sound; they are written back on export).
    pub kept: usize,
    /// ... grouped by subtree.
    pub kept_by_group: BTreeMap<String, usize>,
    /// Paths the WING tree doesn't have (another firmware).
    pub unknown: Vec<String>,
    pub unknown_count: usize,
    /// Values that couldn't be applied, and why.
    pub errors: Vec<String>,
    pub error_count: usize,
    pub channels_added: usize,
}

impl ImportReport {
    /// A human summary for the log.
    pub fn summary(&self) -> String {
        let mut s = format!(
            "WING {} ({}): {} values applied, {} kept without a Live DAW equivalent",
            if self.file_type.is_empty() { "snapshot" } else { &self.file_type },
            if self.creator.is_empty() { "unknown creator" } else { &self.creator },
            self.applied,
            self.kept
        );
        if self.channels_added > 0 {
            s.push_str(&format!(", {} channels added", self.channels_added));
        }
        if self.unknown_count > 0 {
            s.push_str(&format!(", {} unknown paths", self.unknown_count));
        }
        if self.error_count > 0 {
            s.push_str(&format!(", {} not applied", self.error_count));
        }
        s
    }

    /// The summary plus what wasn't applied, one item per line.
    pub fn details(&self) -> String {
        let mut s = self.summary();
        if !self.kept_by_group.is_empty() {
            s.push_str("\nNot applied (no Live DAW equivalent, kept for export):");
            for (g, n) in &self.kept_by_group {
                s.push_str(&format!("\n  {g}: {n}"));
            }
        }
        for e in &self.errors {
            s.push_str(&format!("\nNot applied: {e}"));
        }
        if self.error_count > self.errors.len() {
            s.push_str(&format!("\n  ... and {} more", self.error_count - self.errors.len()));
        }
        for u in &self.unknown {
            s.push_str(&format!("\nUnknown: {u}"));
        }
        if self.unknown_count > self.unknown.len() {
            s.push_str(&format!("\n  ... and {} more", self.unknown_count - self.unknown.len()));
        }
        s
    }
}

const MAX_LISTED: usize = 40;

/// The subtree a path is reported under ("/ch/N/peq").
fn group_of(path: &str) -> String {
    let parts: Vec<&str> = path.split('/').filter(|p| !p.is_empty()).collect();
    let take = match parts.first() {
        Some(&("ch" | "aux" | "bus" | "main" | "mtx" | "io")) => 3,
        _ => 2,
    }
    .min(parts.len().saturating_sub(1))
    .max(1);
    let mut g = String::new();
    for p in parts.iter().take(take) {
        g.push('/');
        let numbered = p.parse::<u32>().is_ok() || (p.starts_with("MX") && p[2..].parse::<u32>().is_ok());
        g.push_str(if numbered { "N" } else { p });
    }
    g
}

/// A JSON leaf as a value for `def`.
fn json_value(ty: NodeType, j: &Json) -> Option<Value> {
    Some(match (ty, j) {
        (_, Json::Bool(b)) => Value::Int(*b as i32),
        (NodeType::Int, Json::Num(n)) => Value::Int(n.round() as i32),
        (_, Json::Num(n)) => Value::Float(*n as f32),
        (_, Json::Str(s)) => Value::Str(s.clone()),
        _ => return None,
    })
}

struct Importer<'a, 'c> {
    c: &'a Console<'c>,
    report: ImportReport,
    /// Source modes, applied once the channels are patched.
    deferred: Vec<(Node, Value)>,
}

impl Importer<'_, '_> {
    fn walk(&mut self, node: Node, obj: &Json, path: &str) {
        let s = schema();
        let mut entries: Vec<&(String, Json)> = obj.entries().iter().collect();
        // The model first (its parameters depend on it); a patch number before its group.
        entries.sort_by_key(|(k, _)| match k.as_str() {
            "mdl" => 0,
            "in" if obj.get("grp").is_some() => 1,
            _ => 2,
        });
        for (key, val) in entries {
            let child_path = format!("{path}/{key}");
            let Some(child) = s.child(node, key, self.c) else {
                self.report.unknown_count += 1;
                if self.report.unknown.len() < MAX_LISTED {
                    self.report.unknown.push(child_path);
                }
                continue;
            };
            let Some(def) = s.def(child, self.c) else { continue };
            if let Json::Obj(_) = val {
                if !def.is_leaf() {
                    self.walk(child, val, &child_path);
                }
                continue;
            }
            if !def.is_leaf() || def.read_only {
                continue;
            }
            let Some(v) = json_value(def.ty, val) else { continue };
            if let Node::Row(r) = child {
                if let Binding::SourceMode(..) = bindings()[r as usize] {
                    self.deferred.push((child, v));
                    continue;
                }
            }
            let mapped = self.c.is_mapped(child);
            match self.c.set(child, &v) {
                Ok(_) if mapped => self.report.applied += 1,
                Ok(_) => {
                    self.report.kept += 1;
                    *self.report.kept_by_group.entry(group_of(&child_path)).or_default() += 1;
                }
                Err(SetError::ReadOnly) => {}
                Err(e) => {
                    self.report.error_count += 1;
                    if self.report.errors.len() < MAX_LISTED {
                        self.report.errors.push(format!("{child_path} = {} ({e})", json::to_string(val)));
                    }
                }
            }
        }
    }
}

/// The sections of a snapshot and the nodes they hold.
fn sections() -> Vec<(&'static str, Node)> {
    let s = schema();
    let mut out = vec![("ae_data", Node::Root)];
    let r = |p: &str| s.resolve(p, &crate::schema::NoModels);
    if let Some(n) = r("/$ctl") {
        out.push(("ce_data", n));
    }
    if let Some(n) = r("/$globals") {
        out.push(("ae_globals", n));
    }
    if let Some(n) = r("/$ctl/$globals") {
        out.push(("ce_globals", n));
    }
    out
}

/// Applies a parsed snapshot to the engine (message thread: it may add channels).
pub fn import_json(engine: &Engine, state: &WingState, snap: &Json, opts: &ImportOptions) -> Result<ImportReport, String> {
    if !matches!(snap, Json::Obj(_)) || snap.get("ae_data").is_none() && snap.get("ce_data").is_none() {
        return Err("not a WING snapshot (no ae_data / ce_data)".into());
    }
    let mut channels_added = 0;
    if opts.grow_channels {
        let wanted = snap.get("ae_data").and_then(|a| a.get("ch")).map_or(0, |ch| ch.entries().len());
        let have = engine.num_channels();
        if wanted > have {
            engine.set_num_channels(wanted.min(40));
            channels_added = engine.num_channels() - have;
        }
    }
    let c = Console::new(engine, state);
    let mut imp = Importer { c: &c, report: ImportReport::default(), deferred: Vec::new() };
    imp.report.channels_added = channels_added;
    imp.report.file_type = snap.get("type").and_then(Json::as_str).unwrap_or("").to_owned();
    let creator: Vec<&str> =
        ["creator_model", "creator_fw", "creator_name"].iter().filter_map(|k| snap.get(k).and_then(Json::as_str)).collect();
    imp.report.creator = creator.join(" ");
    for (key, node) in sections() {
        if let Some(obj) = snap.get(key) {
            let base = if node == Node::Root { String::new() } else { schema().path(node, &c) };
            imp.walk(node, obj, &base);
        }
    }
    for (node, v) in std::mem::take(&mut imp.deferred) {
        match c.set(node, &v) {
            Ok(_) => imp.report.applied += 1,
            Err(e) => {
                imp.report.error_count += 1;
                imp.report.errors.push(format!("{} ({e})", schema().path(node, &c)));
            }
        }
    }
    Ok(imp.report)
}

pub fn import_text(engine: &Engine, state: &WingState, text: &str, opts: &ImportOptions) -> Result<ImportReport, String> {
    import_json(engine, state, &json::parse(text)?, opts)
}

/// Imports a `.snap` file into the console.
pub fn import_file(engine: &Engine, state: &WingState, path: &Path, opts: &ImportOptions) -> Result<ImportReport, String> {
    let text = std::fs::read_to_string(path).map_err(|e| format!("{}: {e}", path.display()))?;
    import_text(engine, state, &text, opts)
}

// ---- export ----------------------------------------------------------------------

fn export_value(ty: NodeType, def: &crate::schema::Def, v: &Value) -> Json {
    match ty {
        NodeType::Int if def.min_int == 0 && def.max_int == 1 => Json::Bool(v.as_bool()),
        NodeType::Int => Json::Num(v.as_i32().unwrap_or(0) as f64),
        NodeType::Str | NodeType::StrEnum => Json::Str(match v {
            Value::Str(s) => s.clone(),
            other => crate::value::display(def, other),
        }),
        _ => Json::Num(v.as_f32().unwrap_or(0.0) as f64),
    }
}

fn export_node(c: &Console, node: Node) -> Json {
    let s = schema();
    let mut kv = Vec::new();
    for ch in s.children(node, c) {
        let name = s.name(ch, c);
        if name.starts_with('$') {
            continue;
        }
        let Some(def) = s.def(ch, c) else { continue };
        if def.is_leaf() {
            if def.read_only {
                continue;
            }
            if let Some(v) = c.get(ch) {
                kv.push((name.into_owned(), export_value(def.ty, def, &v)));
            }
        } else {
            let sub = export_node(c, ch);
            if !sub.entries().is_empty() {
                kv.push((name.into_owned(), sub));
            }
        }
    }
    Json::Obj(kv)
}

/// "YYYY-MM-DD HH:MM:SS" (UTC) for `secs` since 1970.
pub fn timestamp(secs: u64) -> String {
    let days = (secs / 86400) as i64;
    let rem = secs % 86400;
    // Civil date from days (H. Hinnant's algorithm).
    let z = days + 719468;
    let era = z.div_euclid(146097);
    let doe = z - era * 146097;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = yoe + era * 400 + i64::from(m <= 2);
    format!("{y:04}-{m:02}-{d:02} {:02}:{:02}:{:02}", rem / 3600, rem % 3600 / 60, rem % 60)
}

/// The console as a WING snapshot. Experimental: built from the documented
/// format and the tree; not validated on a console or in WING Edit.
pub fn export_json(engine: &Engine, state: &WingState) -> Json {
    let c = Console::new(engine, state);
    let id = state.identity();
    let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map_or(0, |d| d.as_secs());
    let mut kv: Vec<(String, Json)> = vec![
        ("type".into(), Json::Str(SNAPSHOT_TYPE.into())),
        ("creator_fw".into(), Json::Str(id.firmware.clone())),
        ("creator_sn".into(), Json::Str(id.serial.clone())),
        ("creator_model".into(), Json::Str(id.model.clone())),
        ("creator_version".into(), Json::Str(format!("Live DAW {}", env!("CARGO_PKG_VERSION")))),
        ("creator_name".into(), Json::Str(id.name.clone())),
        ("created".into(), Json::Str(timestamp(now))),
        ("active_show".into(), Json::Str(String::new())),
        ("active_scene".into(), Json::Str(String::new())),
    ];
    for (key, node) in sections() {
        let mut obj = export_node(&c, node);
        if key == "ae_data" {
            // The root's '$' nodes are skipped by export_node; ce_data is /$ctl.
            if let Json::Obj(entries) = &mut obj {
                entries.retain(|(k, _)| !k.starts_with('$'));
            }
        }
        kv.push((key.into(), obj));
    }
    Json::Obj(kv)
}

pub fn export_text(engine: &Engine, state: &WingState) -> String {
    json::to_string(&export_json(engine, state))
}

/// Writes the console as a `.snap` file (written to a temporary file, then renamed).
pub fn export_file(engine: &Engine, state: &WingState, path: &Path) -> Result<(), String> {
    let text = export_text(engine, state);
    let tmp = path.with_extension("snap.tmp");
    std::fs::write(&tmp, text).map_err(|e| format!("{}: {e}", tmp.display()))?;
    std::fs::rename(&tmp, path).map_err(|e| format!("{}: {e}", path.display()))
}

// ---- shows ------------------------------------------------------------------------

#[derive(Clone, Debug, PartialEq)]
pub struct ShowScene {
    pub name: String,
    /// SNAP, SNIP, PRESET, CLIP...
    pub kind: String,
    pub file: String,
    pub tag: String,
    pub skip: bool,
    pub link: bool,
    pub info: String,
}

/// The scenes of a `.show` file, in order.
pub fn parse_show(text: &str) -> Result<Vec<ShowScene>, String> {
    let j = json::parse(text)?;
    let scenes = j.get("scenes").ok_or("not a WING show (no scenes)")?;
    let mut numbered: Vec<(u32, &Json)> =
        scenes.entries().iter().filter_map(|(k, v)| k.parse::<u32>().ok().map(|n| (n, v))).collect();
    numbered.sort_by_key(|(n, _)| *n);
    let s = |v: &Json, k: &str| v.get(k).and_then(Json::as_str).unwrap_or("").to_owned();
    Ok(numbered
        .into_iter()
        .map(|(_, v)| ShowScene {
            name: s(v, "name"),
            kind: s(v, "type"),
            file: s(v, "file"),
            tag: s(v, "tag"),
            skip: v.get("skip").and_then(Json::as_bool).unwrap_or(false),
            link: v.get("link").and_then(Json::as_bool).unwrap_or(false),
            info: s(v, "info"),
        })
        .collect())
}

/// Finds a scene's file: relative to the show, or a console path
/// ("I:/library/...") looked up next to the show by its trailing components.
pub fn resolve_scene_file(show_dir: &Path, file: &str) -> Option<PathBuf> {
    let norm = file.replace('\\', "/");
    let parts: Vec<&str> = norm.split('/').filter(|p| !p.is_empty() && !p.ends_with(':')).collect();
    let direct = show_dir.join(&norm);
    if !norm.contains(':') && direct.is_file() {
        return Some(direct);
    }
    for start in 0..parts.len() {
        let mut p = show_dir.to_path_buf();
        for part in &parts[start..] {
            p.push(part);
        }
        if p.is_file() {
            return Some(p);
        }
    }
    None
}

#[derive(Clone, Debug, Default)]
pub struct ShowImportReport {
    pub scenes_added: usize,
    pub skipped: Vec<String>,
    pub details: Vec<String>,
}

impl ShowImportReport {
    pub fn summary(&self) -> String {
        let mut s = format!("WING show: {} scenes added", self.scenes_added);
        if !self.skipped.is_empty() {
            s.push_str(&format!(", {} skipped", self.skipped.len()));
        }
        for k in &self.skipped {
            s.push_str(&format!("\nSkipped: {k}"));
        }
        for d in &self.details {
            s.push_str(&format!("\n{d}"));
        }
        s
    }
}

/// Builds a scratch engine with the live console's structure and state.
fn scratch_copy(engine: &Engine) -> (Engine, livedaw_session::xml::Element) {
    let base = save_console(engine, &mut NoPlugins);
    let scratch = Engine::new(0);
    load_console(&scratch, &base, &mut NoPlugins, false);
    (scratch, base)
}

/// Adds the snapshot scenes of a `.show` file to `show` (after its scenes).
/// Each snapshot is applied to a scratch copy of the console and captured
/// there, so the live console doesn't change. Snippets, presets and clips
/// are skipped.
pub fn import_show(show: &mut Show, engine: &Engine, state: &WingState, path: &Path) -> Result<ShowImportReport, String> {
    let text = std::fs::read_to_string(path).map_err(|e| format!("{}: {e}", path.display()))?;
    let scenes = parse_show(&text)?;
    let dir = path.parent().unwrap_or(Path::new("."));
    let mut report = ShowImportReport::default();
    // Channels first, so scenes reference the same strips as the console.
    let wanted = 40.max(engine.num_channels());
    if engine.num_channels() < wanted {
        engine.set_num_channels(wanted);
    }
    let (scratch, base) = scratch_copy(engine);
    for sc in scenes {
        let label = if sc.name.is_empty() { sc.file.clone() } else { sc.name.clone() };
        if !sc.kind.eq_ignore_ascii_case("SNAP") {
            report.skipped.push(format!("{label} ({} scenes aren't imported)", sc.kind));
            continue;
        }
        let Some(file) = resolve_scene_file(dir, &sc.file) else {
            report.skipped.push(format!("{label} ({} not found)", sc.file));
            continue;
        };
        let snap_text = match std::fs::read_to_string(&file) {
            Ok(t) => t,
            Err(e) => {
                report.skipped.push(format!("{label} ({e})"));
                continue;
            }
        };
        load_console(&scratch, &base, &mut NoPlugins, false);
        let scratch_state = WingState::new(state.identity());
        match import_text(&scratch, &scratch_state, &snap_text, &ImportOptions::default()) {
            Ok(r) => {
                let index = show.num_scenes() as i32;
                let i = show.add_scene(&scratch, &mut NoPlugins, &label, index);
                if let Some(scene) = show.scene_mut(i) {
                    scene.tag = sc.tag.trim_start_matches('#').parse().unwrap_or(0);
                    scene.skip = sc.skip;
                    scene.link_next = sc.link;
                    scene.notes = sc.info.clone();
                }
                report.scenes_added += 1;
                report.details.push(format!("{label}: {}", r.summary()));
            }
            Err(e) => report.skipped.push(format!("{label} ({e})")),
        }
    }
    Ok(report)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn timestamps() {
        assert_eq!(timestamp(0), "1970-01-01 00:00:00");
        assert_eq!(timestamp(1_792_256_400), "2026-10-17 17:00:00");
    }

    #[test]
    fn groups() {
        assert_eq!(group_of("/ch/12/peq/1g"), "/ch/N/peq");
        assert_eq!(group_of("/io/in/A/3/g"), "/io/in/A");
        assert_eq!(group_of("/$ctl/layer/L/1"), "/$ctl/layer");
        assert_eq!(group_of("/ch/3/send/MX2/plink"), "/ch/N/send");
    }
}
