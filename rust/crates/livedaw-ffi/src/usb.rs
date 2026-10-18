//! `ld_usb_*`: the USB recorder / player (message thread; the audio thread
//! side runs inside `ld_engine_process`). C++ view: `livedaw::UsbRecorder`.
//!
//! Only scalars, strings and out-pointers cross here: no shared struct
//! layout (the ABI version doesn't change).

use crate::session::{str_arg, LdString};
use crate::{engine, guard};
use livedaw_engine::recorder::usb::{wav_file_info, RecFormat, RecTap, UsbDeck};
use livedaw_engine::Engine;
use std::path::{Path, PathBuf};
use std::time::Duration;

unsafe fn deck<'a>(e: *const Engine) -> Option<&'a UsbDeck> {
    engine(e).map(|e| e.usb())
}

unsafe fn path_arg(p: *const u8, len: usize) -> PathBuf {
    PathBuf::from(str_arg(p, len).into_owned())
}

unsafe fn set_error(error: *mut *mut LdString, msg: String) {
    if let Some(out) = error.as_mut() {
        *out = LdString::new(msg);
    }
}

unsafe fn result(r: Result<(), String>, error: *mut *mut LdString) -> bool {
    match r {
        Ok(()) => true,
        Err(msg) => {
            set_error(error, msg);
            false
        }
    }
}

fn path_string(p: Option<&Path>) -> *mut LdString {
    LdString::new(p.map(|p| p.to_string_lossy().into_owned()).unwrap_or_default())
}

// ---- recorder ------------------------------------------------------------------------

/// REC: starts recording to `path` (created now; channels 2 or 4, bits 16 or
/// 24), or resumes a paused recording. False sets `*error` (may be null).
#[no_mangle]
pub unsafe extern "C" fn ld_usb_rec_start(
    e: *const Engine,
    path: *const u8,
    len: usize,
    channels: i32,
    bits: i32,
    error: *mut *mut LdString,
) -> bool {
    let Some(d) = deck(e) else { return false };
    let file = path_arg(path, len);
    let format = RecFormat { channels: channels.max(0) as usize, bits: bits.clamp(0, 32) as u16 };
    result(guard(Err("internal error".into()), || d.record(&file, format)), error)
}

#[no_mangle]
pub unsafe extern "C" fn ld_usb_rec_pause(e: *const Engine) {
    if let Some(d) = deck(e) {
        guard((), || d.pause_recording());
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_usb_rec_stop(e: *const Engine) {
    if let Some(d) = deck(e) {
        guard((), || d.stop_recording());
    }
}

/// NEW FILE: the recording continues in `path` from the next sample.
#[no_mangle]
pub unsafe extern "C" fn ld_usb_rec_new_file(e: *const Engine, path: *const u8, len: usize, error: *mut *mut LdString) -> bool {
    let Some(d) = deck(e) else { return false };
    let file = path_arg(path, len);
    result(guard(Err("internal error".into()), || d.new_file(&file)), error)
}

/// 0 STOP, 1 REC, 2 PAUSE, 3 ERROR.
#[no_mangle]
pub unsafe extern "C" fn ld_usb_rec_state(e: *const Engine) -> i32 {
    deck(e).map_or(0, |d| guard(Default::default(), || d.rec_state()) as i32)
}

/// Frames recorded into the current file.
#[no_mangle]
pub unsafe extern "C" fn ld_usb_rec_time(e: *const Engine) -> i64 {
    deck(e).map_or(0, |d| d.rec_time())
}

/// Frames recorded since REC.
#[no_mangle]
pub unsafe extern "C" fn ld_usb_rec_total(e: *const Engine) -> i64 {
    deck(e).map_or(0, |d| d.rec_total())
}

/// The file being (or last) recorded; empty if none.
#[no_mangle]
pub unsafe extern "C" fn ld_usb_rec_file(e: *const Engine) -> *mut LdString {
    path_string(deck(e).and_then(|d| d.rec_file()).as_deref())
}

/// Linear peak of RECORDER port `port` since the last call.
#[no_mangle]
pub unsafe extern "C" fn ld_usb_rec_level(e: *const Engine, port: i32) -> f32 {
    deck(e).map_or(0.0, |d| d.rec_level(port.max(0) as usize))
}

/// What RECORDER port `port` (0-3) records: -1 off, 0.. an input index (a
/// source), `kUsbTapStripBase + kind * 0x100000 + (number - 1) * 2 + channel` a strip.
#[no_mangle]
pub unsafe extern "C" fn ld_usb_rec_tap(e: *const Engine, port: i32) -> i32 {
    deck(e).map_or(-1, |d| d.tap(port.max(0) as usize).code())
}

#[no_mangle]
pub unsafe extern "C" fn ld_usb_set_rec_tap(e: *const Engine, port: i32, code: i32) {
    if let Some(d) = deck(e) {
        d.set_tap(port.max(0) as usize, RecTap::from_code(code));
    }
}

// ---- player ----------------------------------------------------------------------------

#[no_mangle]
pub unsafe extern "C" fn ld_usb_play(e: *const Engine) {
    if let Some(d) = deck(e) {
        guard((), || d.play());
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_usb_pause(e: *const Engine) {
    if let Some(d) = deck(e) {
        guard((), || d.pause());
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_usb_stop(e: *const Engine) {
    if let Some(d) = deck(e) {
        guard((), || d.stop());
    }
}

/// Cues (or plays) queue entry `entry` (0: the first) from frame `pos`.
#[no_mangle]
pub unsafe extern "C" fn ld_usb_load(e: *const Engine, entry: u64, pos: i64, play: bool) {
    if let Some(d) = deck(e) {
        guard((), || d.load(entry, pos, play));
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_usb_seek(e: *const Engine, pos: i64) {
    if let Some(d) = deck(e) {
        guard((), || d.seek(pos));
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_usb_next(e: *const Engine) -> bool {
    deck(e).is_some_and(|d| guard(false, || d.next()))
}

#[no_mangle]
pub unsafe extern "C" fn ld_usb_prev(e: *const Engine) -> bool {
    deck(e).is_some_and(|d| guard(false, || d.prev()))
}

/// 0 STOP, 1 PLAY, 2 PAUSE, 3 ERROR.
#[no_mangle]
pub unsafe extern "C" fn ld_usb_play_state(e: *const Engine) -> i32 {
    deck(e).map_or(0, |d| guard(Default::default(), || d.player_state()) as i32)
}

/// The queue entry playing or cued (0: none).
#[no_mangle]
pub unsafe extern "C" fn ld_usb_play_entry(e: *const Engine) -> u64 {
    deck(e).map_or(0, |d| guard(0, || d.current_entry()))
}

#[no_mangle]
pub unsafe extern "C" fn ld_usb_play_position(e: *const Engine) -> i64 {
    deck(e).map_or(0, |d| guard(0, || d.position()))
}

#[no_mangle]
pub unsafe extern "C" fn ld_usb_play_length(e: *const Engine) -> i64 {
    deck(e).map_or(0, |d| d.length())
}

#[no_mangle]
pub unsafe extern "C" fn ld_usb_play_level(e: *const Engine, ch: i32) -> f32 {
    deck(e).map_or(0.0, |d| d.play_level(ch.max(0) as usize))
}

/// What the player knows about entry `entry` once it opened it (any pointer
/// may be null). False if it doesn't know it (yet).
#[no_mangle]
pub unsafe extern "C" fn ld_usb_play_info(
    e: *const Engine,
    entry: u64,
    channels: *mut i32,
    rate: *mut i32,
    bits: *mut i32,
    length: *mut i64,
    rate_mismatch: *mut bool,
) -> bool {
    let Some(info) = deck(e).and_then(|d| guard(None, || d.file_info(entry))) else { return false };
    if let Some(x) = channels.as_mut() {
        *x = info.channels as i32;
    }
    if let Some(x) = rate.as_mut() {
        *x = info.sample_rate as i32;
    }
    if let Some(x) = bits.as_mut() {
        *x = i32::from(info.bits);
    }
    if let Some(x) = length.as_mut() {
        *x = info.length;
    }
    if let Some(x) = rate_mismatch.as_mut() {
        *x = info.rate_mismatch;
    }
    true
}

/// A WAV file's header (any pointer may be null); false if it can't be read.
#[no_mangle]
pub unsafe extern "C" fn ld_usb_wav_info(
    path: *const u8,
    len: usize,
    channels: *mut i32,
    rate: *mut i32,
    bits: *mut i32,
    frames: *mut i64,
) -> bool {
    let file = path_arg(path, len);
    let Some(info) = guard(None, || wav_file_info(&file)) else { return false };
    if let Some(x) = channels.as_mut() {
        *x = info.channels as i32;
    }
    if let Some(x) = rate.as_mut() {
        *x = info.sample_rate as i32;
    }
    if let Some(x) = bits.as_mut() {
        *x = i32::from(info.format.bits());
    }
    if let Some(x) = frames.as_mut() {
        *x = info.frames as i64;
    }
    true
}

#[no_mangle]
pub unsafe extern "C" fn ld_usb_dropouts(e: *const Engine) -> i32 {
    deck(e).map_or(0, |d| d.dropouts() as i32)
}

/// Waits until the disk thread caught up (offline use, faster than real time).
#[no_mangle]
pub unsafe extern "C" fn ld_usb_wait_for_disk(e: *const Engine, timeout_ms: i32) -> bool {
    deck(e).is_some_and(|d| d.wait_for_disk(Duration::from_millis(timeout_ms.max(0) as u64)))
}

// ---- the queue (folder or playlist) --------------------------------------------------------

#[no_mangle]
pub unsafe extern "C" fn ld_usb_queue_len(e: *const Engine) -> i32 {
    deck(e).map_or(0, |d| d.queue().len() as i32)
}

/// Bumped on every change of the queue.
#[no_mangle]
pub unsafe extern "C" fn ld_usb_queue_version(e: *const Engine) -> u64 {
    deck(e).map_or(0, |d| d.queue().version())
}

/// Entry `index`: its id and flags (pointers may be null); false past the end.
#[no_mangle]
pub unsafe extern "C" fn ld_usb_queue_entry(
    e: *const Engine,
    index: i32,
    id: *mut u64,
    skip: *mut bool,
    stop: *mut bool,
) -> bool {
    let Some(d) = deck(e) else { return false };
    let q = d.queue();
    let Some(entry) = usize::try_from(index).ok().and_then(|i| q.entries().get(i)) else { return false };
    if let Some(x) = id.as_mut() {
        *x = entry.id;
    }
    if let Some(x) = skip.as_mut() {
        *x = entry.skip;
    }
    if let Some(x) = stop.as_mut() {
        *x = entry.stop;
    }
    true
}

#[no_mangle]
pub unsafe extern "C" fn ld_usb_queue_path(e: *const Engine, index: i32) -> *mut LdString {
    let Some(d) = deck(e) else { return path_string(None) };
    let q = d.queue();
    path_string(usize::try_from(index).ok().and_then(|i| q.entries().get(i)).map(|e| e.path.as_path()))
}

/// The index of entry `id`, -1 if it isn't in the queue.
#[no_mangle]
pub unsafe extern "C" fn ld_usb_queue_index_of(e: *const Engine, id: u64) -> i32 {
    deck(e).and_then(|d| d.queue().index_of(id)).map_or(-1, |i| i as i32)
}

/// The queue becomes the audio files of `folder` (unless a playlist is
/// open: then only the folder to return to changes).
#[no_mangle]
pub unsafe extern "C" fn ld_usb_queue_set_folder(e: *const Engine, folder: *const u8, len: usize) {
    let Some(d) = deck(e) else { return };
    let folder = path_arg(folder, len);
    guard((), || {
        let mut q = d.queue();
        if !q.is_playlist() {
            q.set_folder(&folder);
        } else {
            q.set_return_folder(&folder);
        }
    });
}

/// Re-reads the folder of a folder queue (files added, renamed or deleted).
#[no_mangle]
pub unsafe extern "C" fn ld_usb_queue_refresh(e: *const Engine) {
    if let Some(d) = deck(e) {
        guard((), || d.queue().refresh_folder());
    }
}

/// 0 a folder's files, 1 a playlist.
#[no_mangle]
pub unsafe extern "C" fn ld_usb_queue_kind(e: *const Engine) -> i32 {
    deck(e).map_or(0, |d| i32::from(d.queue().is_playlist()))
}

/// The playlist's file (empty while unsaved, or for a folder).
#[no_mangle]
pub unsafe extern "C" fn ld_usb_queue_playlist_file(e: *const Engine) -> *mut LdString {
    let Some(d) = deck(e) else { return path_string(None) };
    let q = d.queue();
    path_string(q.playlist_file())
}

#[no_mangle]
pub unsafe extern "C" fn ld_usb_queue_dirty(e: *const Engine) -> bool {
    deck(e).is_some_and(|d| d.queue().is_dirty())
}

#[no_mangle]
pub unsafe extern "C" fn ld_usb_playlist_new(e: *const Engine) {
    if let Some(d) = deck(e) {
        guard((), || d.queue().new_playlist());
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_usb_playlist_close(e: *const Engine) {
    if let Some(d) = deck(e) {
        guard((), || d.queue().close_playlist());
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_usb_playlist_open(e: *const Engine, path: *const u8, len: usize, error: *mut *mut LdString) -> bool {
    let Some(d) = deck(e) else { return false };
    let file = path_arg(path, len);
    result(guard(Err("internal error".into()), || d.queue().open_playlist(&file)), error)
}

#[no_mangle]
pub unsafe extern "C" fn ld_usb_playlist_save(e: *const Engine, path: *const u8, len: usize, error: *mut *mut LdString) -> bool {
    let Some(d) = deck(e) else { return false };
    let file = path_arg(path, len);
    result(guard(Err("internal error".into()), || d.queue().save_playlist(&file)), error)
}

/// ADD: appends a clip (a folder queue becomes a new playlist). Returns its id.
#[no_mangle]
pub unsafe extern "C" fn ld_usb_queue_add(e: *const Engine, path: *const u8, len: usize) -> u64 {
    let Some(d) = deck(e) else { return 0 };
    let file = path_arg(path, len);
    guard(0, || d.queue().add(&file))
}

/// ADD ALL: appends the audio files of `folder`. Returns how many.
#[no_mangle]
pub unsafe extern "C" fn ld_usb_queue_add_all(e: *const Engine, folder: *const u8, len: usize) -> i32 {
    let Some(d) = deck(e) else { return 0 };
    let folder = path_arg(folder, len);
    guard(0, || d.queue().add_all(&folder) as i32)
}

#[no_mangle]
pub unsafe extern "C" fn ld_usb_queue_remove(e: *const Engine, index: i32) -> bool {
    deck(e).is_some_and(|d| usize::try_from(index).is_ok_and(|i| d.queue().remove(i)))
}

/// UP (-1) / DOWN (+1); returns the entry's new index or -1.
#[no_mangle]
pub unsafe extern "C" fn ld_usb_queue_move(e: *const Engine, index: i32, delta: i32) -> i32 {
    let Some(d) = deck(e) else { return -1 };
    usize::try_from(index).ok().and_then(|i| d.queue().move_entry(i, delta)).map_or(-1, |i| i as i32)
}

#[no_mangle]
pub unsafe extern "C" fn ld_usb_queue_set_skip(e: *const Engine, index: i32, skip: bool) {
    if let (Some(d), Ok(i)) = (deck(e), usize::try_from(index)) {
        d.queue().set_skip(i, skip);
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_usb_queue_set_stop(e: *const Engine, index: i32, stop: bool) {
    if let (Some(d), Ok(i)) = (deck(e), usize::try_from(index)) {
        d.queue().set_stop(i, stop);
    }
}

/// STOP held: every entry stops the playlist after it (or none).
#[no_mangle]
pub unsafe extern "C" fn ld_usb_queue_set_stop_all(e: *const Engine, stop: bool) {
    if let Some(d) = deck(e) {
        d.queue().set_stop_all(stop);
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_usb_queue_set_repeat(e: *const Engine, repeat: bool) {
    if let Some(d) = deck(e) {
        d.queue().set_repeat(repeat);
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_usb_queue_repeat(e: *const Engine) -> bool {
    deck(e).is_some_and(|d| d.queue().repeat())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{ld_engine_create, ld_engine_destroy};

    #[test]
    fn the_queue_and_taps_through_the_abi() {
        unsafe {
            let e = ld_engine_create(0);
            let dir = std::env::temp_dir().join(format!("livedaw-ffi-usb-{}", std::process::id()));
            let _ = std::fs::remove_dir_all(&dir);
            std::fs::create_dir_all(&dir).unwrap();
            let a = dir.join("a.wav").to_string_lossy().into_owned();
            let id = ld_usb_queue_add(e, a.as_ptr(), a.len());
            assert!(id != 0);
            assert_eq!(ld_usb_queue_kind(e), 1);
            assert_eq!(ld_usb_queue_len(e), 1);
            let (mut got, mut skip, mut stop) = (0u64, true, true);
            assert!(ld_usb_queue_entry(e, 0, &mut got, &mut skip, &mut stop));
            assert_eq!((got, skip, stop), (id, false, false));
            assert!(!ld_usb_queue_entry(e, 1, std::ptr::null_mut(), std::ptr::null_mut(), std::ptr::null_mut()));
            ld_usb_queue_set_skip(e, 0, true);
            assert!(ld_usb_queue_entry(e, 0, std::ptr::null_mut(), &mut skip, std::ptr::null_mut()) && skip);
            assert_eq!(ld_usb_queue_index_of(e, id), 0);
            assert_eq!(ld_usb_queue_move(e, 0, 1), -1);

            ld_usb_set_rec_tap(e, 3, 0x20001);
            assert_eq!(ld_usb_rec_tap(e, 3), 0x20001);
            assert_eq!(ld_usb_rec_state(e), 0);
            let bad = "/nonexistent-dir/\0/x.wav";
            let mut error: *mut LdString = std::ptr::null_mut();
            assert!(!ld_usb_rec_start(e, bad.as_ptr(), bad.len(), 2, 16, &mut error));
            assert!(!error.is_null());
            crate::session::ld_string_free(error);
            let _ = std::fs::remove_dir_all(dir);
            ld_engine_destroy(e);
        }
    }
}
