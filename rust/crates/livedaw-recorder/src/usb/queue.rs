//! What the USB player plays: a queue of clips, either the WAV files of a
//! folder (the browser) or a playlist (manual 8.2, fig. 79).
//!
//! A playlist entry can be skipped (SKIP: ignored until SKIP is switched off
//! again) or stop the playlist after it (STOP); REPEAT plays the first clip
//! again after the last. The files of a folder play one at a time: the
//! player stops after each and cues the next.
//!
//! Playlists are saved as extended M3U files: paths relative to the
//! playlist's folder where possible, and `#LIVEDAW:` lines for REPEAT and an
//! entry's SKIP / STOP flags (other players ignore them).

use std::path::{Path, PathBuf};

/// The playlist file extension.
pub const PLAYLIST_EXTENSION: &str = "m3u";

/// One clip of the queue. `id` stays the same while the clip is in the queue
/// (moved or not), so the player can follow it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct QueueEntry {
    pub id: u64,
    pub path: PathBuf,
    pub skip: bool,
    pub stop: bool,
}

/// What follows the end of a clip.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum After {
    /// Play this entry next (gapless).
    Play(u64),
    /// Stop, cueing this entry (None: nothing left to cue).
    Stop(Option<u64>),
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub enum QueueKind {
    /// The audio files of a folder (or nothing yet).
    #[default]
    Folder,
    /// A playlist: its file once saved or opened.
    Playlist { file: Option<PathBuf> },
}

#[derive(Clone, Debug, Default)]
pub struct Queue {
    entries: Vec<QueueEntry>,
    kind: QueueKind,
    folder: Option<PathBuf>,
    repeat: bool,
    dirty: bool,
    next_id: u64,
    version: u64,
}

/// True for the file types the player plays.
pub fn is_audio_file(path: &Path) -> bool {
    path.extension().and_then(|e| e.to_str()).is_some_and(|e| e.eq_ignore_ascii_case("wav"))
}

/// True for playlist files.
pub fn is_playlist_file(path: &Path) -> bool {
    path.extension().and_then(|e| e.to_str()).is_some_and(|e| e.eq_ignore_ascii_case(PLAYLIST_EXTENSION))
}

/// The audio files of `folder` (not its subfolders), sorted by name.
pub fn audio_files_in(folder: &Path) -> Vec<PathBuf> {
    let mut files: Vec<PathBuf> = std::fs::read_dir(folder)
        .map(|d| d.filter_map(|e| e.ok()).map(|e| e.path()).filter(|p| p.is_file() && is_audio_file(p)).collect())
        .unwrap_or_default();
    files.sort_by_key(|p| p.file_name().map(|n| n.to_string_lossy().to_lowercase()));
    files
}

impl Queue {
    pub fn entries(&self) -> &[QueueEntry] {
        &self.entries
    }

    pub fn len(&self) -> usize {
        self.entries.len()
    }

    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }

    pub fn kind(&self) -> &QueueKind {
        &self.kind
    }

    pub fn is_playlist(&self) -> bool {
        matches!(self.kind, QueueKind::Playlist { .. })
    }

    /// The playlist's file, if it has been saved or opened.
    pub fn playlist_file(&self) -> Option<&Path> {
        match &self.kind {
            QueueKind::Playlist { file } => file.as_deref(),
            QueueKind::Folder => None,
        }
    }

    /// The folder whose files the queue holds (folder queues).
    pub fn folder(&self) -> Option<&Path> {
        self.folder.as_deref()
    }

    pub fn repeat(&self) -> bool {
        self.repeat
    }

    /// The playlist changed since it was opened or saved.
    pub fn is_dirty(&self) -> bool {
        self.dirty
    }

    /// Bumped on every change (the views poll it).
    pub fn version(&self) -> u64 {
        self.version
    }

    pub fn index_of(&self, id: u64) -> Option<usize> {
        self.entries.iter().position(|e| e.id == id)
    }

    pub fn entry(&self, id: u64) -> Option<&QueueEntry> {
        self.entries.iter().find(|e| e.id == id)
    }

    fn changed(&mut self, playlist_edit: bool) {
        self.version += 1;
        if playlist_edit && self.is_playlist() {
            self.dirty = true;
        }
    }

    fn make_entry(&mut self, path: PathBuf) -> QueueEntry {
        self.next_id += 1;
        QueueEntry { id: self.next_id, path, skip: false, stop: false }
    }

    // ---- folder and playlist --------------------------------------------------

    /// The queue becomes the audio files of `folder` (the browser plays them).
    pub fn set_folder(&mut self, folder: &Path) {
        let files = audio_files_in(folder);
        self.entries = files.into_iter().map(|p| self.make_entry(p)).collect();
        self.kind = QueueKind::Folder;
        self.folder = Some(folder.to_owned());
        self.dirty = false;
        self.changed(false);
    }

    /// The folder CLOSE PLAYLIST returns to (the browser's, while a playlist is open).
    pub fn set_return_folder(&mut self, folder: &Path) {
        self.folder = Some(folder.to_owned());
    }

    /// Re-reads the folder, keeping the ids of the files still there.
    pub fn refresh_folder(&mut self) {
        let Some(folder) = self.folder.clone().filter(|_| !self.is_playlist()) else { return };
        let files = audio_files_in(&folder);
        let old = std::mem::take(&mut self.entries);
        self.entries = files
            .into_iter()
            .map(|p| match old.iter().find(|e| e.path == p) {
                Some(e) => e.clone(),
                None => self.make_entry(p),
            })
            .collect();
        if self.entries != old {
            self.changed(false);
        }
    }

    /// CREATE PLAYLIST: an empty, unsaved playlist.
    pub fn new_playlist(&mut self) {
        self.entries.clear();
        self.kind = QueueKind::Playlist { file: None };
        self.repeat = false;
        self.dirty = false;
        self.changed(false);
    }

    /// CLOSE PLAYLIST: back to the folder's files (or an empty queue).
    pub fn close_playlist(&mut self) {
        match self.folder.clone() {
            Some(f) => self.set_folder(&f),
            None => {
                self.entries.clear();
                self.kind = QueueKind::Folder;
                self.dirty = false;
                self.changed(false);
            }
        }
    }

    /// OPEN PLAYLIST.
    pub fn open_playlist(&mut self, file: &Path) -> Result<(), String> {
        let text = std::fs::read_to_string(file).map_err(|e| format!("Can't open {}: {e}", file.display()))?;
        let (paths, repeat) = parse_m3u(&text, file.parent().unwrap_or(Path::new("")));
        self.entries = paths
            .into_iter()
            .map(|(p, skip, stop)| {
                let mut e = self.make_entry(p);
                e.skip = skip;
                e.stop = stop;
                e
            })
            .collect();
        self.kind = QueueKind::Playlist { file: Some(file.to_owned()) };
        self.repeat = repeat;
        self.dirty = false;
        self.changed(false);
        Ok(())
    }

    /// SAVE PLAYLIST (as `file`; the queue becomes that playlist).
    pub fn save_playlist(&mut self, file: &Path) -> Result<(), String> {
        let text = write_m3u(&self.entries, self.repeat, file.parent().unwrap_or(Path::new("")));
        if let Some(dir) = file.parent().filter(|d| !d.as_os_str().is_empty()) {
            std::fs::create_dir_all(dir).map_err(|e| format!("Can't create {}: {e}", dir.display()))?;
        }
        let tmp = file.with_extension("m3u.tmp");
        std::fs::write(&tmp, text).and_then(|_| std::fs::rename(&tmp, file)).map_err(|e| {
            let _ = std::fs::remove_file(&tmp);
            format!("Can't save {}: {e}", file.display())
        })?;
        self.kind = QueueKind::Playlist { file: Some(file.to_owned()) };
        self.dirty = false;
        self.changed(false);
        Ok(())
    }

    // ---- editing ----------------------------------------------------------------

    /// ADD: appends a clip (a folder queue becomes a new playlist first).
    /// Returns the new entry's id.
    pub fn add(&mut self, path: &Path) -> u64 {
        if !self.is_playlist() {
            self.new_playlist();
        }
        let e = self.make_entry(path.to_owned());
        let id = e.id;
        self.entries.push(e);
        self.changed(true);
        id
    }

    /// ADD ALL: appends the audio files of `folder`. Returns how many.
    pub fn add_all(&mut self, folder: &Path) -> usize {
        let files = audio_files_in(folder);
        for f in &files {
            self.add(f);
        }
        files.len()
    }

    /// REMOVE.
    pub fn remove(&mut self, index: usize) -> bool {
        if index >= self.entries.len() {
            return false;
        }
        self.entries.remove(index);
        self.changed(true);
        true
    }

    /// UP (-1) / DOWN (+1). Returns the entry's new index.
    pub fn move_entry(&mut self, index: usize, delta: i32) -> Option<usize> {
        let to = index.checked_add_signed(delta as isize)?;
        if index >= self.entries.len() || to >= self.entries.len() {
            return None;
        }
        let e = self.entries.remove(index);
        self.entries.insert(to, e);
        self.changed(true);
        Some(to)
    }

    pub fn set_skip(&mut self, index: usize, skip: bool) {
        if let Some(e) = self.entries.get_mut(index).filter(|e| e.skip != skip) {
            e.skip = skip;
            self.changed(true);
        }
    }

    pub fn set_stop(&mut self, index: usize, stop: bool) {
        if let Some(e) = self.entries.get_mut(index).filter(|e| e.stop != stop) {
            e.stop = stop;
            self.changed(true);
        }
    }

    /// STOP held: every clip stops the playlist after it (or none does).
    pub fn set_stop_all(&mut self, stop: bool) {
        if self.entries.iter().any(|e| e.stop != stop) {
            self.entries.iter_mut().for_each(|e| e.stop = stop);
            self.changed(true);
        }
    }

    pub fn set_repeat(&mut self, repeat: bool) {
        if self.repeat != repeat {
            self.repeat = repeat;
            self.changed(true);
        }
    }

    // ---- what plays ---------------------------------------------------------------

    /// The first clip that isn't skipped.
    pub fn first(&self) -> Option<u64> {
        self.entries.iter().find(|e| !e.skip).map(|e| e.id)
    }

    /// The next clip after index `from` that isn't skipped (wrapping around
    /// to the start when `wrap`, down to `from` itself).
    fn following(&self, from: usize, wrap: bool) -> Option<usize> {
        let n = self.entries.len();
        let wrapped = if wrap { 0..(from + 1).min(n) } else { 0..0 };
        (from + 1..n).chain(wrapped).find(|&i| !self.entries[i].skip)
    }

    /// The previous clip before index `from` that isn't skipped (wrapping
    /// around to the end when `wrap`, down to `from` itself).
    fn preceding(&self, from: usize, wrap: bool) -> Option<usize> {
        let n = self.entries.len();
        let wrapped = if wrap { from.min(n)..n } else { 0..0 };
        (0..from.min(n)).rev().chain(wrapped.rev()).find(|&i| !self.entries[i].skip)
    }

    /// What follows clip `id` when it ends: the next clip, or a stop (after a
    /// STOP clip, at the end without REPEAT, after each file of a folder)
    /// cueing the clip PLAY would start.
    pub fn after(&self, id: u64) -> After {
        let Some(i) = self.index_of(id) else { return After::Stop(self.first()) };
        let wrap = self.repeat && self.is_playlist();
        let next = self.following(i, wrap).map(|j| self.entries[j].id);
        if !self.is_playlist() || self.entries[i].stop {
            return After::Stop(next.or_else(|| self.first()));
        }
        match next {
            Some(n) => After::Play(n),
            None => After::Stop(self.first()),
        }
    }

    /// The `>` key: the next clip (None at the end without REPEAT).
    pub fn next_of(&self, id: u64) -> Option<u64> {
        let Some(i) = self.index_of(id) else { return self.first() };
        self.following(i, self.repeat).map(|j| self.entries[j].id)
    }

    /// The `<` key: the previous clip (None at the start without REPEAT).
    pub fn prev_of(&self, id: u64) -> Option<u64> {
        let Some(i) = self.index_of(id) else { return self.first() };
        self.preceding(i, self.repeat).map(|j| self.entries[j].id)
    }
}

// ---- M3U -----------------------------------------------------------------------------

const TAG: &str = "#LIVEDAW:";

/// Parses an (extended) M3U playlist: entries with their SKIP / STOP flags,
/// and REPEAT. Relative paths are relative to `base`.
pub fn parse_m3u(text: &str, base: &Path) -> (Vec<(PathBuf, bool, bool)>, bool) {
    let mut entries = Vec::new();
    let mut repeat = false;
    let (mut skip, mut stop) = (false, false);
    for line in text.lines().map(str::trim) {
        if let Some(tag) = line.strip_prefix(TAG) {
            match tag.trim().to_ascii_uppercase().as_str() {
                "REPEAT" => repeat = true,
                "SKIP" => skip = true,
                "STOP" => stop = true,
                _ => {}
            }
            continue;
        }
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let p = PathBuf::from(line.replace('\\', std::path::MAIN_SEPARATOR_STR));
        let path = if p.is_absolute() { p } else { base.join(p) };
        entries.push((path, skip, stop));
        skip = false;
        stop = false;
    }
    (entries, repeat)
}

/// Writes an extended M3U playlist (see [`parse_m3u`]).
pub fn write_m3u(entries: &[QueueEntry], repeat: bool, base: &Path) -> String {
    let mut out = String::from("#EXTM3U\n");
    if repeat {
        out.push_str(TAG);
        out.push_str("REPEAT\n");
    }
    for e in entries {
        if e.skip {
            out.push_str(TAG);
            out.push_str("SKIP\n");
        }
        if e.stop {
            out.push_str(TAG);
            out.push_str("STOP\n");
        }
        if let Some(name) = e.path.file_stem() {
            out.push_str("#EXTINF:-1,");
            out.push_str(&name.to_string_lossy());
            out.push('\n');
        }
        let rel = e.path.strip_prefix(base).unwrap_or(&e.path);
        out.push_str(&rel.to_string_lossy());
        out.push('\n');
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp_dir(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("livedaw-usbq-{}-{}", name, std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn playlist(names: &[&str]) -> Queue {
        let mut q = Queue::default();
        q.new_playlist();
        for n in names {
            q.add(Path::new(n));
        }
        q
    }

    #[test]
    fn playlist_order_skip_stop_and_repeat() {
        let mut q = playlist(&["a.wav", "b.wav", "c.wav"]);
        let ids: Vec<u64> = q.entries().iter().map(|e| e.id).collect();
        assert!(q.is_playlist() && q.is_dirty());
        assert_eq!(q.after(ids[0]), After::Play(ids[1]));
        assert_eq!(q.after(ids[2]), After::Stop(Some(ids[0])), "the end without REPEAT cues the first clip");

        q.set_skip(1, true);
        assert_eq!(q.after(ids[0]), After::Play(ids[2]), "SKIP is passed over");
        assert_eq!(q.next_of(ids[0]), Some(ids[2]));
        assert_eq!(q.prev_of(ids[2]), Some(ids[0]));
        assert_eq!(q.next_of(ids[2]), None);

        q.set_repeat(true);
        assert_eq!(q.after(ids[2]), After::Play(ids[0]), "REPEAT plays the first clip again");
        assert_eq!(q.next_of(ids[2]), Some(ids[0]));
        assert_eq!(q.prev_of(ids[0]), Some(ids[2]));

        q.set_stop(0, true);
        assert_eq!(q.after(ids[0]), After::Stop(Some(ids[2])), "STOP stops after the clip and cues the next");
        q.set_stop(0, false);
        q.set_skip(2, true);
        assert_eq!(q.after(ids[0]), After::Play(ids[0]), "REPEAT with a single clip loops it");
        q.set_skip(2, false);
        q.set_stop_all(false);
        q.set_skip(0, true);
        q.set_skip(2, true);
        assert_eq!(q.first(), None);
        assert_eq!(q.after(ids[2]), After::Stop(None));
    }

    #[test]
    fn editing_keeps_ids() {
        let mut q = playlist(&["a.wav", "b.wav", "c.wav"]);
        let ids: Vec<u64> = q.entries().iter().map(|e| e.id).collect();
        assert_eq!(q.move_entry(0, 1), Some(1));
        assert_eq!(q.entries().iter().map(|e| e.id).collect::<Vec<_>>(), vec![ids[1], ids[0], ids[2]]);
        assert_eq!(q.move_entry(0, -1), None);
        assert_eq!(q.move_entry(2, 1), None);
        assert!(q.remove(1));
        assert!(!q.remove(5));
        assert_eq!(q.index_of(ids[2]), Some(1));
        assert_eq!(q.after(ids[0]), After::Stop(q.first()), "a removed clip stops and cues the first");
        let v = q.version();
        q.set_repeat(true);
        assert!(q.version() > v);
    }

    #[test]
    fn folder_queue_stops_after_each_file() {
        let dir = temp_dir("folder");
        for n in ["b.wav", "A.WAV", "c.txt", "d.wav"] {
            std::fs::write(dir.join(n), b"x").unwrap();
        }
        std::fs::create_dir_all(dir.join("sub.wav")).unwrap(); // a folder, not a file
        let mut q = Queue::default();
        q.set_folder(&dir);
        let names: Vec<String> = q.entries().iter().map(|e| e.path.file_name().unwrap().to_string_lossy().into_owned()).collect();
        assert_eq!(names, ["A.WAV", "b.wav", "d.wav"]);
        let ids: Vec<u64> = q.entries().iter().map(|e| e.id).collect();
        assert_eq!(q.after(ids[0]), After::Stop(Some(ids[1])));
        assert_eq!(q.after(ids[2]), After::Stop(Some(ids[0])));
        assert_eq!(q.next_of(ids[0]), Some(ids[1]));

        std::fs::write(dir.join("aa.wav"), b"x").unwrap();
        q.refresh_folder();
        assert_eq!(q.len(), 4);
        assert_eq!(q.entries()[0].id, ids[0], "existing files keep their ids");

        // ADD turns the folder into a new playlist.
        q.add(&dir.join("d.wav"));
        assert!(q.is_playlist() && q.len() == 1 && q.playlist_file().is_none());
        assert_eq!(q.add_all(&dir), 4);
        q.close_playlist();
        assert!(!q.is_playlist());
        assert_eq!(q.len(), 4);
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn m3u_round_trip() {
        let dir = temp_dir("m3u");
        let mut q = playlist(&[]);
        q.add(&dir.join("songs").join("one.wav"));
        q.add(Path::new("/elsewhere/two.wav"));
        q.add(&dir.join("three.wav"));
        q.set_skip(1, true);
        q.set_stop(2, true);
        q.set_repeat(true);
        let file = dir.join("lists").join("SET 1.m3u");
        q.save_playlist(&file).unwrap();
        assert!(!q.is_dirty());
        let text = std::fs::read_to_string(&file).unwrap();
        assert!(text.starts_with("#EXTM3U\n#LIVEDAW:REPEAT\n"));
        assert!(text.contains("#EXTINF:-1,two\n"));

        let mut loaded = Queue::default();
        loaded.open_playlist(&file).unwrap();
        assert!(loaded.repeat() && !loaded.is_dirty());
        assert_eq!(loaded.playlist_file(), Some(file.as_path()));
        let got: Vec<(PathBuf, bool, bool)> = loaded.entries().iter().map(|e| (e.path.clone(), e.skip, e.stop)).collect();
        assert_eq!(
            got,
            vec![
                (dir.join("songs").join("one.wav"), false, false),
                (PathBuf::from("/elsewhere/two.wav"), true, false),
                (dir.join("three.wav"), false, true)
            ]
        );
        // Relative entries resolve against the playlist's folder.
        let (entries, repeat) = parse_m3u("# comment\n\nsub/x.wav\n#LIVEDAW:stop\n/abs/y.wav\n", Path::new("/base"));
        assert!(!repeat);
        assert_eq!(entries, vec![(PathBuf::from("/base/sub/x.wav"), false, false), (PathBuf::from("/abs/y.wav"), false, true)]);
        assert!(Queue::default().open_playlist(&dir.join("missing.m3u")).is_err());
        let _ = std::fs::remove_dir_all(dir);
    }
}
