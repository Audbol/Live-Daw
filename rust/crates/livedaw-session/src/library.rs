//! LIBRARY scene and snippet files: the WING saves single snapshots and
//! snippets to a USB stick and loads them back into a show (SNAP and SNIP
//! tabs, manual 5.6). Here they go to a folder.
//!
//! * Scene file (`.lsnap`): a scene with its settings (scope, fade, notes,
//!   tag, SKIP, LINK), its console state and its snippets.
//! * Snippet file (`.lsnip`): a snippet with its scope and the values it holds.
//!
//! The files are Sidecar's own XML, written atomically like show files, not
//! the WING's formats (a WING `.snap` is imported on the SHOW tab). Importing
//! adds to the show and never touches the console: the scene or snippet holds
//! strip references, and a recall skips strips the console doesn't have.

use crate::presets::{self, PresetType};
use crate::refs::StripRef;
use crate::show::{read_scene, read_snippet, scene_to_xml, snippet_to_xml, write_atomically, SceneItem, Show};
use crate::xml::{self, Element};
use crate::{refs::refs_from_string, Scene, Snippet};
use std::fs;
use std::path::Path;

pub const LIBRARY_VERSION: i32 = 1;
pub const SCENE_EXTENSION: &str = "lsnap";
pub const SNIPPET_EXTENSION: &str = "lsnip";

const SCENE_ROOT: &str = "LiveDAWScene";
const SNIPPET_ROOT: &str = "LiveDAWSnippet";

/// What a library file holds.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FileType {
    Scene,
    Snippet,
}

/// A library file's header: its type, the scene's or snippet's name and a
/// count (a scene's snippets, a snippet's parameters).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FileInfo {
    pub kind: FileType,
    pub name: String,
    pub count: usize,
}

/// Reads a library file's header.
pub fn info(file: &Element) -> Option<FileInfo> {
    if file.int_or("version", 0) > LIBRARY_VERSION {
        return None;
    }
    if file.has_tag(SCENE_ROOT) {
        let scene = file.child("Scene")?;
        let count = scene.child("Snippets").map_or(0, |s| s.children_named("Snippet").count());
        return Some(FileInfo { kind: FileType::Scene, name: scene.str_or("name", "").to_owned(), count });
    }
    if file.has_tag(SNIPPET_ROOT) {
        let snippet = parse_snippet(file.child("Snippet")?);
        return Some(FileInfo { kind: FileType::Snippet, name: snippet.name.clone(), count: snippet.parameter_count() });
    }
    None
}

fn state_of(e: &Element) -> Option<Element> {
    e.child("State").cloned()
}

fn parse_scene(e: &Element) -> Scene {
    read_scene(e, &mut state_of, &|s: &str| refs_from_string(s))
}

fn parse_snippet(e: &Element) -> Snippet {
    read_snippet(e, &mut state_of, &|s: &str| refs_from_string(s))
}

fn scene_root(scene: &Scene) -> Element {
    let mut root = Element::new(SCENE_ROOT);
    root.set("version", LIBRARY_VERSION);
    root.add(scene_to_xml(scene));
    root
}

fn snippet_root(snippet: &Snippet) -> Element {
    let mut root = Element::new(SNIPPET_ROOT);
    root.set("version", LIBRARY_VERSION);
    root.add(snippet_to_xml(snippet));
    root
}

/// Scene `index` of the show as a scene file (with its snippets).
pub fn scene_file(show: &Show, index: i32) -> Option<Element> {
    show.scene(index).map(scene_root)
}

/// Snippet `snippet` of scene `scene` as a snippet file.
pub fn snippet_file(show: &Show, scene: i32, snippet: i32) -> Option<Element> {
    show.snippet(scene, snippet).map(snippet_root)
}

/// EXPORT: saves scene `scene` (with its snippets; `snippet` < 0) or snippet
/// `snippet` of it at `path`, named after the file, as the WING names a
/// scene or snippet saved to USB by the name it was saved under.
pub fn export(show: &Show, scene: i32, snippet: i32, path: &Path) -> Result<(), String> {
    let name = path.file_stem().map(|s| s.to_string_lossy().trim().to_owned()).filter(|n| !n.is_empty());
    let file = if snippet < 0 {
        let mut sc = show.scene(scene).ok_or("No such scene")?.clone();
        if let Some(name) = name {
            sc.name = name;
        }
        scene_root(&sc)
    } else {
        let mut sn = show.snippet(scene, snippet).ok_or("No such snippet")?.clone();
        if let Some(name) = name {
            sn.name = name;
        }
        snippet_root(&sn)
    };
    save(path, &file)
}

/// Adds the scene of a scene file to the show at `index` (appended if out of
/// range). Returns its index.
pub fn import_scene(show: &mut Show, file: &Element, index: i32) -> Result<i32, String> {
    match info(file) {
        Some(i) if i.kind == FileType::Scene => {}
        Some(_) => return Err("Not a scene file".into()),
        None => return Err("Not a Sidecar scene file".into()),
    }
    let scene = parse_scene(file.child("Scene").ok_or("The file holds no scene")?);
    Ok(show.insert_scene(scene, index))
}

/// Adds the snippet of a snippet file to scene `scene`. Returns its index.
pub fn import_snippet(show: &mut Show, file: &Element, scene: i32) -> Result<i32, String> {
    match info(file) {
        Some(i) if i.kind == FileType::Snippet => {}
        Some(_) => return Err("Not a snippet file".into()),
        None => return Err("Not a Sidecar snippet file".into()),
    }
    if show.scene(scene).is_none() {
        return Err("Select the scene the snippet goes into".into());
    }
    let snippet = parse_snippet(file.child("Snippet").ok_or("The file holds no snippet")?);
    Ok(show.insert_snippet(scene, snippet))
}

/// The audio files a clip scene can play (as the CLIP tab lists them).
pub const CLIP_EXTENSIONS: &[&str] = &["wav", "wave", "flac", "aif", "aiff", "aifc", "mp3"];

/// ADD ITEM TO SHOW (manual 5.6 p. 52): a snippet file, an audio clip, an FX
/// preset (loading into FX slot `fx_slot`, 0-based) or a channel preset
/// (loading onto `strip`) as a show item, with its name (the file's). A
/// scene file is imported as a scene instead ([`import_scene`]); a show
/// can't be added.
pub fn item_from_file(path: &Path, fx_slot: usize, strip: StripRef) -> Result<(SceneItem, String), String> {
    let name = path.file_stem().map(|s| s.to_string_lossy().into_owned()).unwrap_or_default();
    let extension = path.extension().map(|e| e.to_string_lossy().to_lowercase()).unwrap_or_default();
    let item = match extension.as_str() {
        SNIPPET_EXTENSION => {
            let file = load(path)?;
            match info(&file) {
                Some(i) if i.kind == FileType::Snippet => {}
                _ => return Err("Not a snippet file".into()),
            }
            SceneItem::Snippet(parse_snippet(file.child("Snippet").ok_or("The file holds no snippet")?))
        }
        presets::FX_EXTENSION | presets::CHANNEL_EXTENSION => {
            let preset = presets::load(path)?;
            match presets::info(&preset).map(|i| i.kind) {
                Some(PresetType::Fx) if fx_slot < livedaw_engine::params::NUM_FX => SceneItem::Fx { preset, slot: fx_slot },
                Some(PresetType::Fx) => return Err(format!("FX {} doesn't exist", fx_slot + 1)),
                Some(PresetType::Channel) => SceneItem::Channel { preset, strip },
                None => return Err("Not a Sidecar preset".into()),
            }
        }
        e if CLIP_EXTENSIONS.contains(&e) => {
            if !path.is_file() {
                return Err(format!("{} doesn't exist", path.display()));
            }
            SceneItem::Clip(path.to_path_buf())
        }
        SCENE_EXTENSION => return Err("A scene file is imported as a scene".into()),
        _ => return Err("This file can't be added to a show".into()),
    };
    Ok((item, name))
}

/// A scene or snippet file's scene or snippet, for loading it onto the
/// console without adding it to the show (the STARTUP folder).
pub fn read_file(file: &Element) -> Result<Result<Scene, Snippet>, String> {
    match info(file).map(|i| i.kind) {
        Some(FileType::Scene) => Ok(Ok(parse_scene(file.child("Scene").ok_or("The file holds no scene")?))),
        Some(FileType::Snippet) => Ok(Err(parse_snippet(file.child("Snippet").ok_or("The file holds no snippet")?))),
        None => Err("Not a Sidecar scene or snippet file".into()),
    }
}

// ---- files -------------------------------------------------------------------------------

pub fn save(path: &Path, file: &Element) -> Result<(), String> {
    if let Some(dir) = path.parent().filter(|d| !d.as_os_str().is_empty()) {
        fs::create_dir_all(dir).map_err(|e| format!("Could not create {}: {e}", dir.display()))?;
    }
    write_atomically(path, &file.to_document())
}

pub fn load(path: &Path) -> Result<Element, String> {
    let text = fs::read_to_string(path).map_err(|e| format!("Could not read {}: {e}", path.display()))?;
    let xml = xml::parse(&text).map_err(|e| format!("Not a valid library file ({e})"))?;
    info(&xml).ok_or_else(|| "Not a Sidecar scene or snippet file".to_owned())?;
    Ok(xml)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn headers() {
        let mut show = Show::new();
        let scene = Scene { name: "Verse".into(), snippets: vec![Snippet::default(), Snippet::default()], ..Scene::default() };
        show.insert_scene(scene, -1);
        let file = scene_file(&show, 0).unwrap();
        assert_eq!(info(&file), Some(FileInfo { kind: FileType::Scene, name: "Verse".into(), count: 2 }));
        let file = snippet_file(&show, 0, 1).unwrap();
        assert_eq!(info(&file).map(|i| i.kind), Some(FileType::Snippet));
        assert!(scene_file(&show, 1).is_none() && snippet_file(&show, 0, 2).is_none());

        // Newer files, other files and the wrong kind are refused.
        let mut newer = scene_file(&show, 0).unwrap();
        newer.set("version", LIBRARY_VERSION + 1);
        assert!(info(&newer).is_none());
        assert!(info(&Element::new("LiveDAWShow")).is_none());
        let (scene, snippet) = (scene_file(&show, 0).unwrap(), snippet_file(&show, 0, 0).unwrap());
        assert!(import_snippet(&mut show, &scene, 0).is_err());
        assert!(import_scene(&mut show, &snippet, 0).is_err());
    }

    #[test]
    fn export_names_after_the_file() {
        let mut show = Show::new();
        let scene = Scene {
            name: "Verse".into(),
            snippets: vec![Snippet { name: "Solo".into(), ..Snippet::default() }],
            ..Scene::default()
        };
        show.insert_scene(scene, -1);
        let dir = std::env::temp_dir().join(format!("livedaw-library-{}", std::process::id()));
        let (scene_path, snippet_path) = (dir.join("Verse 2.lsnap"), dir.join("Solo B.lsnip"));
        export(&show, 0, -1, &scene_path).unwrap();
        export(&show, 0, 0, &snippet_path).unwrap();
        assert_eq!(info(&load(&scene_path).unwrap()).map(|i| i.name), Some("Verse 2".into()));
        assert_eq!(info(&load(&snippet_path).unwrap()).map(|i| i.name), Some("Solo B".into()));
        assert!(export(&show, 1, -1, &scene_path).is_err() && export(&show, 0, 1, &snippet_path).is_err());
        // The show keeps its names.
        assert_eq!(show.scene(0).map(|s| s.name.as_str()), Some("Verse"));
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn insert_keeps_the_current_scene() {
        let mut show = Show::new();
        for name in ["A", "B"] {
            show.insert_scene(Scene { name: name.into(), ..Scene::default() }, -1);
        }
        let file = scene_file(&show, 1).unwrap();
        assert_eq!(import_scene(&mut show, &file, 0), Ok(0));
        assert_eq!(import_scene(&mut show, &file, 99), Ok(3));
        let names: Vec<_> = show.scenes().iter().map(|s| s.name.as_str()).collect();
        assert_eq!(names, ["B", "A", "B", "B"]);
        assert_eq!(show.current_scene(), -1);
    }
}
