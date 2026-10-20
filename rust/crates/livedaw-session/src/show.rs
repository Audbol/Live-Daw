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
//! As on the WING's LIBRARY screen, a scene can carry a tag (#1-#128, for
//! recall by tag), be skipped (GO PREV/NEXT and recalls pass over it) and be
//! linked to the scene below it (recalling it recalls that one too; the link
//! follows the scene, so after a move it links to its new neighbour).
//!
//! A scene's recall scope can name strips, DCAs, mute groups and FX slots as
//! well as sections (`scope::Scope`). GLOBAL safes keep DCAs, mute groups, FX
//! slots, sections and the console-wide settings out of scene recalls, beside
//! the per-strip recall safe. PARTIAL LOAD recalls a chosen scope (NO GLOB:
//! ignoring the safes), RESTORE the whole scene, PARTIAL UPDATE stores a
//! chosen scope into it. Snippet REC / STOP records which parameters changed.
//!
//! Scene and snippet recall never create or destroy channels or plugins, so
//! recalling during a show can't cause dropouts. Message thread only.

use crate::console::{self, section, ApplyResult, FadeParam, FadeTarget, PluginBridge};
use crate::custom::CustomControls;
use crate::layers::LayerSet;
use crate::legacy::{self, Converter};
use crate::refs::{refs_from_string, refs_to_string, StripRef};
use crate::scope::{self, ChangeCounts, GlobalSafe, RecordedChanges, Recording, Scope};
use crate::xml::{self, Element};
use livedaw_engine::Engine;
use std::collections::HashMap;
use std::fs;
use std::io::Write as _;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

pub const SHOW_VERSION: i32 = 3;
pub const MAX_FADE_SECONDS: f64 = 60.0;
/// Scene tags are #1..#128 (0 = no tag).
pub const MAX_SCENE_TAG: i32 = 128;

#[derive(Debug, Clone)]
pub struct Snippet {
    pub name: String,
    pub sections: u32,
    pub strips: Vec<StripRef>,
    /// DCAs, mute groups and FX slots it recalls (bit n = item n+1), and the
    /// console-wide settings (see `scope::Scope`).
    pub dcas: u32,
    pub mute_groups: u32,
    pub fx: u32,
    pub console: bool,
    pub fire_with_scene: bool,
    /// The number of parameters REC recorded (0: not recorded).
    pub recorded_parameters: usize,
    /// Each `<Strip>` holds the sections it recalls (a recorded snippet's
    /// strips can differ).
    pub state: Option<Element>,
}

impl Default for Snippet {
    fn default() -> Self {
        Self {
            name: String::new(),
            sections: section::FADER | section::SENDS,
            strips: Vec::new(),
            dcas: 0,
            mute_groups: 0,
            fx: 0,
            console: false,
            fire_with_scene: false,
            recorded_parameters: 0,
            state: None,
        }
    }
}

impl Snippet {
    /// What the snippet recalls.
    pub fn scope(&self) -> Scope {
        Scope {
            sections: self.sections,
            strips: Some(self.strips.clone()),
            dcas: self.dcas,
            mute_groups: self.mute_groups,
            fx: self.fx,
            console: self.console,
        }
    }

    fn set_scope(&mut self, scope: &Scope) {
        // Its DCAs, mute groups and FX slots recall with their sections.
        self.sections = (scope.sections | (scope::global_sections(scope) & !section::NAME)) & section::ALL;
        self.strips = scope.strips.clone().unwrap_or_default();
        self.dcas = scope.dcas & scope::ALL_DCAS;
        self.mute_groups = scope.mute_groups & scope::ALL_MUTE_GROUPS;
        self.fx = scope.fx & scope::ALL_FX;
        self.console = scope.console;
    }

    /// The parameter count shown for it: what REC recorded, else the values it holds.
    pub fn parameter_count(&self) -> usize {
        if self.recorded_parameters > 0 {
            return self.recorded_parameters;
        }
        let Some(state) = &self.state else { return 0 };
        let empty = Element::new("");
        state.elements().map(|e| scope::count_differences(&empty, e)).sum()
    }
}

#[derive(Debug, Clone)]
pub struct Scene {
    pub name: String,
    pub notes: String,
    /// The sections the scene recalls (its scope's CONTENTS).
    pub scope: u32,
    /// The rest of the recall scope: strips (`None` = all), DCAs, mute groups,
    /// FX slots and the console-wide settings (see [`Scene::recall_scope`]).
    pub scope_strips: Option<Vec<StripRef>>,
    pub scope_dcas: u32,
    pub scope_mute_groups: u32,
    pub scope_fx: u32,
    pub scope_console: bool,
    pub fade_seconds: f64,
    /// EDIT TAG: #1..#128, 0 = none.
    pub tag: i32,
    /// SKIP: GO PREV/NEXT and recalls ignore the scene.
    pub skip: bool,
    /// LINK: recalling this scene also recalls the one below it.
    pub link_next: bool,
    /// SEND MIDI: MIDI data sent each time the scene is recalled, as the
    /// console's EDIT MIDI TEXT STRING has it (hexadecimal bytes, "U:" / "D:"
    /// for the USB / DIN port; the application parses and sends it).
    pub midi: String,
    pub state: Option<Element>,
    pub snippets: Vec<Snippet>,
}

impl Default for Scene {
    fn default() -> Self {
        Self {
            name: String::new(),
            notes: String::new(),
            scope: section::ALL & !section::ROUTING,
            scope_strips: None,
            scope_dcas: scope::ALL_DCAS,
            scope_mute_groups: scope::ALL_MUTE_GROUPS,
            scope_fx: scope::ALL_FX,
            scope_console: true,
            fade_seconds: 0.0,
            tag: 0,
            skip: false,
            link_next: false,
            midi: String::new(),
            state: None,
            snippets: Vec::new(),
        }
    }
}

impl Scene {
    /// What recalling the scene applies.
    pub fn recall_scope(&self) -> Scope {
        Scope {
            sections: self.scope,
            strips: self.scope_strips.clone(),
            dcas: self.scope_dcas,
            mute_groups: self.scope_mute_groups,
            fx: self.scope_fx,
            console: self.scope_console,
        }
    }

    pub fn set_recall_scope(&mut self, scope: &Scope) {
        let scope = scope.clone().sanitised();
        self.scope = scope.sections;
        self.scope_strips = scope.strips;
        self.scope_dcas = scope.dcas;
        self.scope_mute_groups = scope.mute_groups;
        self.scope_fx = scope.fx;
        self.scope_console = scope.console;
    }
}

/// A valid scene tag (0 = none).
pub fn clamp_scene_tag(tag: i32) -> i32 {
    if (1..=MAX_SCENE_TAG).contains(&tag) {
        tag
    } else {
        0
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
    /// The CUSTOM CONTROLS assignments (CC EDIT).
    pub custom: CustomControls,
    /// GLOBAL safes beyond the strips' recall safe.
    pub global_safe: GlobalSafe,
    /// The scope PARTIAL LOAD and PARTIAL UPDATE last used (they keep it, 3.0).
    pub partial_scope: Scope,
    scenes: Vec<Scene>,
    recording: Option<Recording>,
    recorded: Option<(RecordedChanges, ChangeCounts)>,
    current_scene: i32,
    /// SEND MIDI of the scenes the last recall recalled, in order.
    pending_midi: Vec<String>,
    fades: Vec<Fade>,
    fade_start: Instant,
    fade_duration: Duration,
}

impl Default for Show {
    fn default() -> Self {
        Self {
            name: "Untitled show".into(),
            layers: LayerSet::default(),
            custom: CustomControls::default(),
            global_safe: GlobalSafe::default(),
            partial_scope: Scope::default(),
            scenes: Vec::new(),
            recording: None,
            recorded: None,
            current_scene: -1,
            pending_midi: Vec::new(),
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

    /// PARTIAL UPDATE: stores only what `scope` covers into the scene; the
    /// rest of it stays as it was.
    pub fn store_scene_partial(&mut self, engine: &Engine, bridge: &mut dyn PluginBridge, index: i32, scope: &Scope) {
        if self.scene(index).is_none() {
            return;
        }
        let partial = scope::capture_scoped(engine, scope, bridge);
        if let Some(scene) = self.scene_mut(index) {
            match &mut scene.state {
                Some(state) => scope::merge_state(state, &partial),
                None => scene.state = Some(partial),
            }
        }
        self.partial_scope = scope.clone();
        self.current_scene = index;
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

    /// Recalls a scene (and its auto-fire snippets), then the scenes it is
    /// linked to. A skipped scene is not recalled (and ends a chain of links).
    /// In a chain each scene applies in turn, so a later one wins where they
    /// overlap and only the last one's fade glides. The current scene becomes
    /// the last one recalled.
    pub fn recall_scene(&mut self, engine: &Engine, bridge: &mut dyn PluginBridge, index: i32) -> ApplyResult {
        let mut result = ApplyResult::default();
        self.pending_midi.clear();
        // Each scene at most once (indices only grow).
        for i in index.. {
            let Some(scene) = self.scene(i).filter(|s| !s.skip) else { break };
            let linked = scene.link_next;
            result.merge(self.recall_one(engine, bridge, i));
            if !linked {
                break;
            }
        }
        result
    }

    /// SEND MIDI of the scenes the last recall (`recall_scene`, GO NEXT /
    /// PREV, by tag) recalled, linked ones included; taking them clears them.
    pub fn take_pending_midi(&mut self) -> Vec<String> {
        std::mem::take(&mut self.pending_midi)
    }

    /// The first scene tagged `tag` that isn't skipped, or -1.
    pub fn find_scene_by_tag(&self, tag: i32) -> i32 {
        if clamp_scene_tag(tag) == 0 {
            return -1;
        }
        self.scenes.iter().position(|s| s.tag == tag && !s.skip).map_or(-1, |i| i as i32)
    }

    /// Recalls the first scene tagged `tag` (custom controls / MIDI); nothing if none.
    pub fn recall_by_tag(&mut self, engine: &Engine, bridge: &mut dyn PluginBridge, tag: i32) -> ApplyResult {
        match self.find_scene_by_tag(tag) {
            -1 => ApplyResult::default(),
            index => self.recall_scene(engine, bridge, index),
        }
    }

    fn recall_one(&mut self, engine: &Engine, bridge: &mut dyn PluginBridge, index: i32) -> ApplyResult {
        let mut result = ApplyResult::default();
        if let Some(midi) = self.scene(index).map(|s| s.midi.clone()).filter(|m| !m.trim().is_empty()) {
            self.pending_midi.push(midi);
        }
        let Some(scene) = self.scene(index) else { return result };
        let Some(state) = scene.state.clone() else { return result };
        let (scope, fade_seconds) = (scene.recall_scope(), scene.fade_seconds);
        let snippets: Vec<Snippet> = scene.snippets.iter().filter(|s| s.fire_with_scene && s.state.is_some()).cloned().collect();

        self.cancel_fade();

        let fade = fade_seconds > 0.0;
        let mut targets = Vec::new();
        let safe = self.global_safe.clone();
        result = scope::apply_scoped(engine, &state, &scope, Some(&safe), true, fade.then_some(&mut targets), bridge);

        // Auto-fire snippets apply on top of the scene (they ignore recall safes, like
        // a manual snippet). Their continuous values override the scene's fade targets.
        for snippet in &snippets {
            let Some(st) = &snippet.state else { continue };
            result.merge(scope::apply_scoped(engine, st, &snippet.scope(), None, false, fade.then_some(&mut targets), bridge));
        }

        if fade {
            self.start_fade(targets, fade_seconds);
        }
        self.current_scene = index;
        result
    }

    /// PARTIAL LOAD (`scope`; `ignore_safes` = NO GLOB) or RESTORE (`None`:
    /// the whole scene, safes included). Only this scene: no links, no
    /// auto-fire snippets, no fade.
    pub fn recall_scene_with(
        &mut self,
        engine: &Engine,
        bridge: &mut dyn PluginBridge,
        index: i32,
        scope: Option<&Scope>,
        ignore_safes: bool,
    ) -> ApplyResult {
        let Some(state) = self.scene(index).and_then(|s| s.state.clone()) else { return ApplyResult::default() };
        self.cancel_fade();
        let safe = self.global_safe.clone();
        let result = match scope {
            Some(scope) => {
                self.partial_scope = scope.clone();
                let safe = (!ignore_safes).then_some(&safe);
                scope::apply_scoped(engine, &state, scope, safe, !ignore_safes, None, bridge)
            }
            None => scope::apply_scoped(engine, &state, &Scope::console(section::ALL), None, false, None, bridge),
        };
        self.current_scene = index;
        result
    }

    /// GO NEXT: the next scene that isn't skipped (at the end, the last one again).
    pub fn recall_next(&mut self, engine: &Engine, bridge: &mut dyn PluginBridge) -> ApplyResult {
        let n = self.scenes.len() as i32;
        let playable = |i: &i32| !self.scenes[*i as usize].skip;
        let index = ((self.current_scene + 1).max(0)..n).find(playable).or_else(|| (0..n).rev().find(playable));
        index.map_or_else(ApplyResult::default, |i| self.recall_scene(engine, bridge, i))
    }

    /// GO PREV: the previous scene that isn't skipped (at the start, the first one again).
    pub fn recall_previous(&mut self, engine: &Engine, bridge: &mut dyn PluginBridge) -> ApplyResult {
        let n = self.scenes.len() as i32;
        let playable = |i: &i32| !self.scenes[*i as usize].skip;
        let index = (0..self.current_scene.min(n)).rev().find(playable).or_else(|| (0..n).find(playable));
        index.map_or_else(ApplyResult::default, |i| self.recall_scene(engine, bridge, i))
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
        self.add_snippet_scoped(engine, bridge, scene, name, &Scope::strips(sections, strips))
    }

    /// A new snippet of what `scope` covers (strips, sections, DCAs, mute
    /// groups, FX slots, console-wide settings).
    pub fn add_snippet_scoped(
        &mut self,
        engine: &Engine,
        bridge: &mut dyn PluginBridge,
        scene: i32,
        name: &str,
        scope: &Scope,
    ) -> i32 {
        if self.scene(scene).is_none() {
            return -1;
        }
        let scope = Scope { strips: Some(scope.strips.clone().unwrap_or_default()), ..scope.clone() }.sanitised();
        let state = scope::capture_scoped(engine, &scope, bridge);
        let Some(sc) = self.scene_mut(scene) else { return -1 };
        let mut sn = Snippet { name: name.into(), state: Some(state), ..Snippet::default() };
        sn.set_scope(&scope);
        sc.snippets.push(sn);
        sc.snippets.len() as i32 - 1
    }

    /// UPDATE: captures the snippet's parameters again (each strip only the
    /// sections it holds).
    pub fn store_snippet(&mut self, engine: &Engine, bridge: &mut dyn PluginBridge, scene: i32, snippet: i32) {
        let Some(sn) = self.snippet(scene, snippet) else { return };
        let held: HashMap<StripRef, u32> = sn
            .state
            .iter()
            .flat_map(|st| st.children_named("Strip"))
            .map(|e| (StripRef::parse(e.str_or("ref", "")), e.int_or("sections", 0) as u32))
            .collect();
        let changes = RecordedChanges {
            strips: sn
                .strips
                .iter()
                .map(|r| (*r, held.get(r).map_or(sn.sections, |m| m & sn.sections)))
                .filter(|(_, m)| *m != 0)
                .collect(),
            dcas: sn.dcas,
            mute_groups: sn.mute_groups,
            fx: sn.fx,
            console: sn.console,
            sections: sn.sections,
            count: 0,
        };
        let state = changes.capture(engine, bridge);
        if let Some(sn) = self.snippet_mut(scene, snippet) {
            sn.state = Some(state);
        }
    }

    /// ADD ITEMS: captures what `items` covers into the snippet (contents
    /// without strips add to every strip it has).
    pub fn add_snippet_items(&mut self, engine: &Engine, bridge: &mut dyn PluginBridge, scene: i32, snippet: i32, items: &Scope) {
        let Some(sn) = self.snippet(scene, snippet) else { return };
        let mut items = items.clone().sanitised();
        if items.strips.as_ref().is_none_or(Vec::is_empty) {
            items.strips = Some(sn.strips.clone());
        }
        let partial = scope::capture_scoped(engine, &items, bridge);
        let Some(sn) = self.snippet_mut(scene, snippet) else { return };
        match &mut sn.state {
            Some(state) => scope::merge_state(state, &partial),
            None => sn.state = Some(partial),
        }
        sn.sections |= items.sections | (scope::global_sections(&items) & !section::NAME);
        for r in items.strips.unwrap_or_default() {
            if !sn.strips.contains(&r) {
                sn.strips.push(r);
            }
        }
        sn.dcas |= items.dcas;
        sn.mute_groups |= items.mute_groups;
        sn.fx |= items.fx;
        sn.console |= items.console;
        sn.recorded_parameters = 0;
    }

    /// REMOVE ITEMS: drops what `items` covers from the snippet (contents
    /// without strips from every strip). A strip left with nothing goes.
    pub fn remove_snippet_items(&mut self, scene: i32, snippet: i32, items: &Scope) {
        let Some(sn) = self.snippet_mut(scene, snippet) else { return };
        let all = items.strips.as_ref().is_none_or(Vec::is_empty);
        let chosen = |r: &StripRef| all || items.strips.as_ref().is_some_and(|s| s.contains(r));
        let mut remaining = 0u32;
        let mut kept = Vec::new();
        if let Some(state) = &mut sn.state {
            state.children.retain_mut(|c| {
                let crate::xml::Node::Element(e) = c else { return true };
                if e.name != "Strip" {
                    return true;
                }
                let r = StripRef::parse(e.str_or("ref", ""));
                let mut mask = e.int_or("sections", 0) as u32;
                if chosen(&r) {
                    mask &= !items.sections;
                    e.set("sections", mask);
                }
                if mask & section::ALL == 0 {
                    return false;
                }
                remaining |= mask;
                kept.push(r);
                true
            });
            sn.strips.retain(|r| kept.contains(r));
            sn.sections &= remaining;
        } else if items.sections & sn.sections == sn.sections {
            sn.strips.retain(|r| !chosen(r));
        }
        sn.dcas &= !items.dcas;
        sn.mute_groups &= !items.mute_groups;
        sn.fx &= !items.fx;
        sn.console &= !items.console;
        sn.recorded_parameters = 0;
    }

    pub fn remove_snippet(&mut self, scene: i32, snippet: i32) {
        if self.snippet(scene, snippet).is_some() {
            if let Some(sc) = self.scene_mut(scene) {
                sc.snippets.remove(snippet as usize);
            }
        }
    }

    pub fn recall_snippet(&mut self, engine: &Engine, bridge: &mut dyn PluginBridge, scene: i32, snippet: i32) -> ApplyResult {
        self.recall_snippet_with(engine, bridge, scene, snippet, None)
    }

    /// PARTIAL LOAD of a snippet: only what is in both its scope and `limit`.
    pub fn recall_snippet_with(
        &mut self,
        engine: &Engine,
        bridge: &mut dyn PluginBridge,
        scene: i32,
        snippet: i32,
        limit: Option<&Scope>,
    ) -> ApplyResult {
        let Some(sn) = self.snippet(scene, snippet) else { return ApplyResult::default() };
        let Some(state) = sn.state.clone() else { return ApplyResult::default() };
        let mut scope = sn.scope();
        if let Some(l) = limit {
            scope.sections &= l.sections;
            if let (Some(own), Some(only)) = (&mut scope.strips, &l.strips) {
                own.retain(|r| only.contains(r));
            }
            scope.dcas &= l.dcas;
            scope.mute_groups &= l.mute_groups;
            scope.fx &= l.fx;
            scope.console &= l.console;
            self.partial_scope = l.clone();
        }
        scope::apply_scoped(engine, &state, &scope, None, false, None, bridge)
    }

    // ---- snippet recording (REC / STOP) ------------------------------------------------

    /// REC: remembers the console to record the parameters changed from now on.
    pub fn start_recording(&mut self, engine: &Engine) {
        self.recording = Some(Recording::start(engine));
        self.recorded = None;
    }

    pub fn is_recording(&self) -> bool {
        self.recording.is_some()
    }

    /// STOP: keeps what changed for SAVE. Returns the number of changed parameters.
    pub fn stop_recording(&mut self, engine: &Engine) -> usize {
        let Some(rec) = self.recording.take() else { return self.recorded_count(engine) };
        let changes = rec.changes(engine);
        let n = changes.0.count;
        self.recorded = Some(changes);
        n
    }

    /// The parameters changed since REC (while recording), or recorded by the last STOP.
    pub fn recorded_count(&self, engine: &Engine) -> usize {
        match (&self.recording, &self.recorded) {
            (Some(rec), _) => rec.changes(engine).0.count,
            (None, Some((c, _))) => c.count,
            _ => 0,
        }
    }

    /// What the last STOP recorded, limited to `focus` (REC FOCUS) when given.
    pub fn recorded_changes(&self, focus: Option<&Scope>) -> Option<RecordedChanges> {
        let (changes, counts) = self.recorded.as_ref()?;
        Some(match focus {
            Some(f) => changes.limited_to(f, counts),
            None => changes.clone(),
        })
    }

    /// CLEAR ALL during or after a recording: drops it.
    pub fn clear_recording(&mut self) {
        self.recording = None;
        self.recorded = None;
    }

    /// SAVE after STOP: a snippet of the recorded parameters (within `focus`).
    /// -1 if nothing was recorded.
    pub fn add_recorded_snippet(
        &mut self,
        engine: &Engine,
        bridge: &mut dyn PluginBridge,
        scene: i32,
        name: &str,
        focus: Option<&Scope>,
    ) -> i32 {
        let Some(changes) = self.recorded_changes(focus).filter(|c| !c.is_empty()) else { return -1 };
        if self.scene(scene).is_none() {
            return -1;
        }
        let state = changes.capture(engine, bridge);
        let Some(sc) = self.scene_mut(scene) else { return -1 };
        let mut sn = Snippet { name: name.into(), state: Some(state), recorded_parameters: changes.count, ..Snippet::default() };
        sn.set_scope(&changes.scope());
        sc.snippets.push(sn);
        let index = sc.snippets.len() as i32 - 1;
        self.recorded = None;
        index
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
        root.add(self.custom.to_xml());
        if !self.global_safe.is_empty() {
            root.add(self.global_safe.to_xml());
        }
        let mut partial = Element::new("PartialScope");
        self.partial_scope.write_attrs(&mut partial, "sections");
        root.add(partial);

        let scenes = root.add_new("Scenes");
        for scene in &self.scenes {
            let sx = scenes.add_new("Scene");
            sx.set("name", &scene.name).set("notes", &scene.notes);
            scene.recall_scope().write_attrs(sx, "scope");
            sx.set("fade", scene.fade_seconds)
                .set("tag", scene.tag)
                .set_bool("skip", scene.skip)
                .set_bool("link", scene.link_next);
            if !scene.midi.is_empty() {
                sx.set("midi", &scene.midi);
            }
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
                if sn.dcas != 0 || sn.mute_groups != 0 || sn.fx != 0 || sn.console {
                    nx.set("scopeDca", sn.dcas)
                        .set("scopeMgrp", sn.mute_groups)
                        .set("scopeFx", sn.fx)
                        .set_bool("scopeConsole", sn.console);
                }
                if sn.recorded_parameters > 0 {
                    nx.set("pars", sn.recorded_parameters);
                }
                if let Some(st) = &sn.state {
                    nx.add(st.clone());
                }
            }
        }
        root
    }

    /// Loads a show (version 3, or an older show or version 1 session file,
    /// converted to the WING layout). With `load_plugins` false (safe mode) the
    /// console is restored without instantiating plugins. Ok carries warnings
    /// (e.g. plugins that failed to load); Err means nothing was loaded.
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
            self.custom.reset();
            self.current_scene = -1;
            self.name = "Imported session".into();
            let mut converter = Converter::default();
            let console = converter.convert_console(xml);
            let mut warnings = converter.warnings;
            warnings.extend(console::load_console(engine, &console, bridge, load_plugins));
            return Ok(warnings);
        }

        if !xml.has_tag("LiveDAWShow") {
            return Err("Not a Sidecar show file".into());
        }
        if xml.int_or("version", 0) > SHOW_VERSION {
            return Err("This show was saved by a newer version of Sidecar".into());
        }
        let Some(console_xml) = xml.child("Console") else {
            return Err("The show file has no console section".into());
        };

        let mut converter = legacy::is_legacy(console_xml).then(Converter::default);
        let converted_console = converter.as_mut().map(|c| c.convert_console(console_xml));
        let console_xml = converted_console.as_ref().unwrap_or(console_xml);

        self.scenes.clear();
        self.name = xml.str_or("name", "Untitled show").to_owned();
        match xml.child("Layers") {
            Some(l) if converter.is_some() => self.layers.from_xml(&legacy::convert_layers(l)),
            Some(l) => self.layers.from_xml(l),
            None => self.layers.reset(),
        }
        match xml.child("CustomControls") {
            Some(c) => self.custom.from_xml(c),
            None => self.custom.reset(),
        }
        self.global_safe = xml.child("GlobalSafe").map(GlobalSafe::from_xml).unwrap_or_default();
        self.partial_scope =
            xml.child("PartialScope").map_or_else(Scope::default, |p| Scope::read_attrs(p, "sections", &Scope::default()));
        self.clear_recording();

        let mut warnings = console::load_console(engine, console_xml, bridge, load_plugins);

        let is_legacy = converter.is_some();
        let mut state_of = |e: &Element| -> Option<Element> {
            let st = e.child("State")?;
            Some(match converter.as_mut() {
                Some(c) if legacy::is_legacy(st) => c.convert_state(st),
                _ => st.clone(),
            })
        };
        let refs_of =
            |s: &str, legacy: bool| if legacy { refs_from_string(&legacy::convert_refs(s)) } else { refs_from_string(s) };

        if let Some(scenes) = xml.child("Scenes") {
            for sx in scenes.children_named("Scene") {
                let defaults = Scene::default();
                let recall = Scope::read_attrs(sx, "scope", &defaults.recall_scope());
                let mut scene = Scene {
                    name: sx.str_or("name", "").to_owned(),
                    notes: sx.str_or("notes", "").to_owned(),
                    scope: recall.sections,
                    scope_strips: recall.strips.map(|s| refs_of(&refs_to_string(&s), is_legacy)),
                    scope_dcas: recall.dcas,
                    scope_mute_groups: recall.mute_groups,
                    scope_fx: recall.fx,
                    scope_console: recall.console,
                    fade_seconds: sx.double_or("fade", 0.0).clamp(0.0, MAX_FADE_SECONDS),
                    tag: clamp_scene_tag(sx.int_or("tag", 0)),
                    skip: sx.bool_or("skip", false),
                    link_next: sx.bool_or("link", false),
                    midi: sx.str_or("midi", "").to_owned(),
                    state: state_of(sx),
                    snippets: Vec::new(),
                };
                if let Some(snippets) = sx.child("Snippets") {
                    for nx in snippets.children_named("Snippet") {
                        let globals = Scope::read_attrs(nx, "sections", &Scope::strips(0, Vec::new()));
                        scene.snippets.push(Snippet {
                            name: nx.str_or("name", "").to_owned(),
                            sections: nx.int_or("sections", 0) as u32,
                            strips: refs_of(nx.str_or("strips", ""), is_legacy),
                            dcas: globals.dcas,
                            mute_groups: globals.mute_groups,
                            fx: globals.fx,
                            console: globals.console,
                            fire_with_scene: nx.bool_or("fireWithScene", false),
                            recorded_parameters: usize::try_from(nx.int_or("pars", 0)).unwrap_or(0),
                            state: state_of(nx),
                        });
                    }
                }
                self.scenes.push(scene);
            }
        }

        if let Some(c) = converter {
            let mut all = c.warnings;
            all.append(&mut warnings);
            warnings = all;
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

    /// Clears scenes and resets the console to `num_channels` channels (the WING has 40).
    pub fn clear(&mut self, engine: &Engine, num_channels: usize) {
        self.cancel_fade();
        self.scenes.clear();
        self.current_scene = -1;
        self.name = "Untitled show".into();
        self.layers.reset();
        self.custom.reset();
        self.global_safe = GlobalSafe::default();
        self.partial_scope = Scope::default();
        self.clear_recording();
        engine.reset();
        engine.set_num_channels(num_channels);
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
