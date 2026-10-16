//! A show: the console setup plus an ordered list of scenes, each of which can
//! hold snippets.
//!
//! * Show    - the whole file: console structure (channels, buses, plugins,
//!   patching), layers and every scene.
//! * Scene   - a snapshot of the console. Recalling it applies the sections in
//!   its recall scope to every strip that isn't recall-safe, optionally gliding
//!   faders, pans and send levels over a fade time.
//! * Snippet - a partial snapshot that belongs to a scene: chosen strips x chosen
//!   sections. Fired on its own, or automatically straight after its scene.
//!
//! Scene and snippet recall never create or destroy channels or plugins, so
//! recalling during a show can't cause dropouts. Message thread only.

use crate::console::{self, section, ApplyResult, FadeParam, FadeTarget, PluginBridge};
use crate::layers::LayerSet;
use crate::refs::{refs_from_string, refs_to_string, StripRef};
use crate::xml::{self, Element};
use livedaw_engine::Engine;
use std::fs;
use std::io::Write as _;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

pub const SHOW_VERSION: i32 = 2;
pub const MAX_FADE_SECONDS: f64 = 60.0;

#[derive(Debug, Clone)]
pub struct Snippet {
    pub name: String,
    pub sections: u32,
    pub strips: Vec<StripRef>,
    pub fire_with_scene: bool,
    pub state: Option<Element>,
}

impl Default for Snippet {
    fn default() -> Self {
        Self {
            name: String::new(),
            sections: section::FADER | section::SENDS,
            strips: Vec::new(),
            fire_with_scene: false,
            state: None,
        }
    }
}

#[derive(Debug, Clone)]
pub struct Scene {
    pub name: String,
    pub notes: String,
    pub scope: u32,
    pub fade_seconds: f64,
    pub state: Option<Element>,
    pub snippets: Vec<Snippet>,
}

impl Default for Scene {
    fn default() -> Self {
        Self {
            name: String::new(),
            notes: String::new(),
            scope: section::ALL & !section::ROUTING,
            fade_seconds: 0.0,
            state: None,
            snippets: Vec::new(),
        }
    }
}

struct Fade {
    param: FadeParam,
    start: f32,
    target: f32,
}

pub struct Show {
    pub name: String,
    pub layers: LayerSet,
    scenes: Vec<Scene>,
    current_scene: i32,
    fades: Vec<Fade>,
    fade_start: Instant,
    fade_duration: Duration,
}

impl Default for Show {
    fn default() -> Self {
        Self {
            name: "Untitled show".into(),
            layers: LayerSet::default(),
            scenes: Vec::new(),
            current_scene: -1,
            fades: Vec::new(),
            fade_start: Instant::now(),
            fade_duration: Duration::ZERO,
        }
    }
}

impl Show {
    pub fn new() -> Self {
        Self::default()
    }

    // ---- scenes ----------------------------------------------------------------------

    pub fn num_scenes(&self) -> usize {
        self.scenes.len()
    }

    pub fn scenes(&self) -> &[Scene] {
        &self.scenes
    }

    pub fn scene(&self, index: i32) -> Option<&Scene> {
        usize::try_from(index).ok().and_then(|i| self.scenes.get(i))
    }

    pub fn scene_mut(&mut self, index: i32) -> Option<&mut Scene> {
        usize::try_from(index).ok().and_then(|i| self.scenes.get_mut(i))
    }

    pub fn snippet_mut(&mut self, scene: i32, snippet: i32) -> Option<&mut Snippet> {
        let i = usize::try_from(snippet).ok()?;
        self.scene_mut(scene)?.snippets.get_mut(i)
    }

    pub fn snippet(&self, scene: i32, snippet: i32) -> Option<&Snippet> {
        let i = usize::try_from(snippet).ok()?;
        self.scene(scene)?.snippets.get(i)
    }

    pub fn current_scene(&self) -> i32 {
        self.current_scene
    }

    /// Captures the console as a new scene (appended, or inserted at `index`).
    pub fn add_scene(&mut self, engine: &Engine, bridge: &mut dyn PluginBridge, name: &str, index: i32) -> i32 {
        let scene =
            Scene { name: name.into(), state: Some(console::capture(engine, section::ALL, None, bridge)), ..Scene::default() };
        let n = self.scenes.len() as i32;
        let index = if (0..=n).contains(&index) { index } else { n };
        self.scenes.insert(index as usize, scene);
        self.current_scene = index;
        index
    }

    pub fn store_scene(&mut self, engine: &Engine, bridge: &mut dyn PluginBridge, index: i32) {
        if self.scene(index).is_some() {
            let state = console::capture(engine, section::ALL, None, bridge);
            if let Some(scene) = self.scene_mut(index) {
                scene.state = Some(state);
            }
            self.current_scene = index;
        }
    }

    pub fn remove_scene(&mut self, index: i32) {
        if self.scene(index).is_none() {
            return;
        }
        self.scenes.remove(index as usize);
        if self.current_scene == index {
            self.current_scene = -1;
        } else if self.current_scene > index {
            self.current_scene -= 1;
        }
    }

    pub fn move_scene(&mut self, from: i32, to: i32) {
        let n = self.scenes.len() as i32;
        if self.scene(from).is_none() || !(0..n).contains(&to) || from == to {
            return;
        }
        let scene = self.scenes.remove(from as usize);
        self.scenes.insert(to as usize, scene);
        let cur = self.current_scene;
        if cur == from {
            self.current_scene = to;
        } else if from < cur && to >= cur {
            self.current_scene -= 1;
        } else if from > cur && to <= cur {
            self.current_scene += 1;
        }
    }

    /// Recalls a scene (and its auto-fire snippets).
    pub fn recall_scene(&mut self, engine: &Engine, bridge: &mut dyn PluginBridge, index: i32) -> ApplyResult {
        let mut result = ApplyResult::default();
        let Some(scene) = self.scene(index) else { return result };
        let Some(state) = scene.state.clone() else { return result };
        let (scope, fade_seconds) = (scene.scope, scene.fade_seconds);
        let snippets: Vec<Snippet> = scene.snippets.iter().filter(|s| s.fire_with_scene && s.state.is_some()).cloned().collect();

        self.cancel_fade();

        let fade = fade_seconds > 0.0;
        let mut targets = Vec::new();
        result = console::apply(engine, &state, scope, None, true, fade.then_some(&mut targets), bridge);

        // Auto-fire snippets apply on top of the scene (they ignore recall safes, like
        // a manual snippet). Their continuous values override the scene's fade targets.
        for snippet in &snippets {
            let Some(st) = &snippet.state else { continue };
            result.merge(console::apply(
                engine,
                st,
                snippet.sections,
                Some(&snippet.strips),
                false,
                fade.then_some(&mut targets),
                bridge,
            ));
        }

        if fade {
            self.start_fade(targets, fade_seconds);
        }
        self.current_scene = index;
        result
    }

    pub fn recall_next(&mut self, engine: &Engine, bridge: &mut dyn PluginBridge) -> ApplyResult {
        let index = (self.current_scene + 1).min(self.scenes.len() as i32 - 1);
        self.recall_scene(engine, bridge, index)
    }

    pub fn recall_previous(&mut self, engine: &Engine, bridge: &mut dyn PluginBridge) -> ApplyResult {
        let index = (self.current_scene - 1).max(0);
        self.recall_scene(engine, bridge, index)
    }

    // ---- snippets --------------------------------------------------------------------

    pub fn add_snippet(
        &mut self,
        engine: &Engine,
        bridge: &mut dyn PluginBridge,
        scene: i32,
        name: &str,
        sections: u32,
        strips: Vec<StripRef>,
    ) -> i32 {
        if self.scene(scene).is_none() {
            return -1;
        }
        let state = console::capture(engine, sections, Some(&strips), bridge);
        let Some(sc) = self.scene_mut(scene) else { return -1 };
        sc.snippets.push(Snippet { name: name.into(), sections, strips, fire_with_scene: false, state: Some(state) });
        sc.snippets.len() as i32 - 1
    }

    pub fn store_snippet(&mut self, engine: &Engine, bridge: &mut dyn PluginBridge, scene: i32, snippet: i32) {
        let Some(sn) = self.snippet(scene, snippet) else { return };
        let state = console::capture(engine, sn.sections, Some(&sn.strips), bridge);
        if let Some(sn) = self.snippet_mut(scene, snippet) {
            sn.state = Some(state);
        }
    }

    pub fn remove_snippet(&mut self, scene: i32, snippet: i32) {
        if self.snippet(scene, snippet).is_some() {
            if let Some(sc) = self.scene_mut(scene) {
                sc.snippets.remove(snippet as usize);
            }
        }
    }

    pub fn recall_snippet(&mut self, engine: &Engine, bridge: &mut dyn PluginBridge, scene: i32, snippet: i32) -> ApplyResult {
        match self.snippet(scene, snippet) {
            Some(Snippet { state: Some(state), sections, strips, .. }) => {
                console::apply(engine, state, *sections, Some(strips), false, None, bridge)
            }
            _ => ApplyResult::default(),
        }
    }

    // ---- fades -----------------------------------------------------------------------

    pub fn is_fading(&self) -> bool {
        !self.fades.is_empty()
    }

    fn start_fade(&mut self, targets: Vec<FadeTarget>, seconds: f64) {
        self.fades.clear();
        for t in targets {
            // A later target for the same parameter (e.g. from a snippet) wins.
            if let Some(existing) = self.fades.iter_mut().find(|f| f.param.same(&t.param)) {
                existing.target = t.target;
            } else if let Some(v) = t.param.value() {
                self.fades.push(Fade { start: v.load(), param: t.param, target: t.target });
            }
        }
        self.fade_start = Instant::now();
        self.fade_duration = Duration::from_secs_f64(seconds.clamp(0.0, MAX_FADE_SECONDS));
    }

    /// Jumps to the end of a running fade so the previous scene is fully applied.
    pub fn cancel_fade(&mut self) {
        for f in self.fades.drain(..) {
            if let Some(v) = f.param.value() {
                v.store(f.target);
            }
        }
    }

    /// Advances running fades (call at ~60 Hz). Returns true while fading.
    pub fn tick(&mut self) -> bool {
        self.tick_at(Instant::now())
    }

    pub fn tick_at(&mut self, now: Instant) -> bool {
        if self.fades.is_empty() {
            return false;
        }
        let elapsed = now.saturating_duration_since(self.fade_start).as_secs_f64();
        let t = (elapsed / self.fade_duration.as_secs_f64().max(0.001)).clamp(0.0, 1.0) as f32;
        for f in &self.fades {
            if let Some(v) = f.param.value() {
                v.store(f.start + t * (f.target - f.start));
            }
        }
        if t >= 1.0 {
            self.fades.clear();
        }
        !self.fades.is_empty()
    }

    // ---- files -----------------------------------------------------------------------

    pub fn to_xml(&self, engine: &Engine, bridge: &mut dyn PluginBridge) -> Element {
        let mut root = Element::new("LiveDAWShow");
        root.set("version", SHOW_VERSION).set("name", &self.name).set("currentScene", self.current_scene);
        root.add(console::save_console(engine, bridge));
        root.add(self.layers.to_xml());

        let scenes = root.add_new("Scenes");
        for scene in &self.scenes {
            let sx = scenes.add_new("Scene");
            sx.set("name", &scene.name)
                .set("notes", &scene.notes)
                .set("scope", scene.scope as i32)
                .set("fade", scene.fade_seconds);
            if let Some(st) = &scene.state {
                sx.add(st.clone());
            }
            let snippets = sx.add_new("Snippets");
            for sn in &scene.snippets {
                let nx = snippets.add_new("Snippet");
                nx.set("name", &sn.name)
                    .set("sections", sn.sections as i32)
                    .set("strips", refs_to_string(&sn.strips))
                    .set_bool("fireWithScene", sn.fire_with_scene);
                if let Some(st) = &sn.state {
                    nx.add(st.clone());
                }
            }
        }
        root
    }

    /// Loads a show (or a version 1 session file). With `load_plugins` false
    /// (safe mode) the console is restored without instantiating plugins.
    /// Ok carries warnings (e.g. plugins that failed to load); Err means nothing was loaded.
    pub fn load_from_xml(
        &mut self,
        engine: &Engine,
        bridge: &mut dyn PluginBridge,
        xml: &Element,
        load_plugins: bool,
    ) -> Result<Vec<String>, String> {
        self.cancel_fade();

        // Version 1 files were a bare console ("session").
        if xml.has_tag("LiveDAWSession") {
            self.scenes.clear();
            self.layers.reset();
            self.current_scene = -1;
            self.name = "Imported session".into();
            let console = Element { name: "Console".into(), ..xml.clone() };
            return Ok(console::load_console(engine, &console, bridge, load_plugins));
        }

        if !xml.has_tag("LiveDAWShow") {
            return Err("Not a Live DAW show file".into());
        }
        if xml.int_or("version", 0) > SHOW_VERSION {
            return Err("This show was saved by a newer version of Live DAW".into());
        }
        let Some(console_xml) = xml.child("Console") else {
            return Err("The show file has no console section".into());
        };

        self.scenes.clear();
        self.name = xml.str_or("name", "Untitled show").to_owned();
        match xml.child("Layers") {
            Some(l) => self.layers.from_xml(l),
            None => self.layers.reset(),
        }

        let warnings = console::load_console(engine, console_xml, bridge, load_plugins);

        if let Some(scenes) = xml.child("Scenes") {
            for sx in scenes.children_named("Scene") {
                let defaults = Scene::default();
                let mut scene = Scene {
                    name: sx.str_or("name", "").to_owned(),
                    notes: sx.str_or("notes", "").to_owned(),
                    scope: sx.int_or("scope", defaults.scope as i32) as u32,
                    fade_seconds: sx.double_or("fade", 0.0).clamp(0.0, MAX_FADE_SECONDS),
                    state: sx.child("State").cloned(),
                    snippets: Vec::new(),
                };
                if let Some(snippets) = sx.child("Snippets") {
                    for nx in snippets.children_named("Snippet") {
                        scene.snippets.push(Snippet {
                            name: nx.str_or("name", "").to_owned(),
                            sections: nx.int_or("sections", 0) as u32,
                            strips: refs_from_string(nx.str_or("strips", "")),
                            fire_with_scene: nx.bool_or("fireWithScene", false),
                            state: nx.child("State").cloned(),
                        });
                    }
                }
                self.scenes.push(scene);
            }
        }

        self.current_scene = xml.int_or("currentScene", -1).clamp(-1, self.scenes.len() as i32 - 1);
        Ok(warnings)
    }

    pub fn load_from_text(
        &mut self,
        engine: &Engine,
        bridge: &mut dyn PluginBridge,
        text: &str,
        load_plugins: bool,
    ) -> Result<Vec<String>, String> {
        let xml = xml::parse(text).map_err(|e| format!("Not a valid show file ({e})"))?;
        self.load_from_xml(engine, bridge, &xml, load_plugins)
    }

    /// Writes atomically (temp file + rename) and keeps the previous version as
    /// `<file>.bak`, so a crash or power cut during a save never leaves a corrupt show.
    pub fn save_to_file(&self, engine: &Engine, bridge: &mut dyn PluginBridge, path: &Path) -> Result<(), String> {
        let text = self.to_xml(engine, bridge).to_document();
        write_atomically(path, &text)
    }

    /// Loads a show file, falling back to the backup made by the previous save.
    pub fn load_from_file(
        &mut self,
        engine: &Engine,
        bridge: &mut dyn PluginBridge,
        path: &Path,
        load_plugins: bool,
    ) -> Result<Vec<String>, String> {
        let parsed = read_show(path).or_else(|_| read_show(&backup_path(path)));
        match parsed {
            Ok(xml) => self.load_from_xml(engine, bridge, &xml, load_plugins),
            Err(_) => Err(format!("Could not read {}", path.display())),
        }
    }

    /// Clears scenes and resets the console to `num_channels` inputs.
    pub fn clear(&mut self, engine: &Engine, num_channels: usize) {
        self.cancel_fade();
        self.scenes.clear();
        self.current_scene = -1;
        self.name = "Untitled show".into();
        self.layers.reset();
        engine.reset();
        engine.set_num_inputs(num_channels);
    }
}

fn read_show(path: &Path) -> Result<Element, String> {
    let text = fs::read_to_string(path).map_err(|e| e.to_string())?;
    xml::parse(&text).map_err(|e| e.to_string())
}

pub fn backup_path(path: &Path) -> PathBuf {
    let mut name = path.file_name().map(|n| n.to_os_string()).unwrap_or_default();
    name.push(".bak");
    path.with_file_name(name)
}

/// Writes `text` to `path` via a verified temporary file in the same folder.
pub fn write_atomically(path: &Path, text: &str) -> Result<(), String> {
    let dir = path.parent().filter(|p| !p.as_os_str().is_empty()).unwrap_or(Path::new("."));
    let file_name = path.file_name().ok_or_else(|| format!("Invalid file name: {}", path.display()))?;

    if path.is_file() {
        // Best effort: a failed backup must not prevent saving.
        let _ = fs::copy(path, backup_path(path));
    }

    let mut temp_name = std::ffi::OsString::from(".");
    temp_name.push(file_name);
    temp_name.push(format!(".{}.tmp", std::process::id()));
    let temp = dir.join(temp_name);

    let write = || -> std::io::Result<()> {
        let mut f = fs::File::create(&temp)?;
        f.write_all(text.as_bytes())?;
        f.sync_all()
    };
    if let Err(e) = write() {
        let _ = fs::remove_file(&temp);
        return Err(format!("Could not write {}: {e}", temp.display()));
    }

    // Verify what hit the disk before it replaces the real file.
    if read_show(&temp).is_err() {
        let _ = fs::remove_file(&temp);
        return Err("Verification of the saved show failed".into());
    }

    if let Err(e) = fs::rename(&temp, path) {
        let _ = fs::remove_file(&temp);
        return Err(format!("Could not replace {}: {e}", path.display()));
    }
    Ok(())
}
