//! C ABI for sessions: shows, scenes, snippets, layers and show files.
//! Message thread only. Mirrored in `include/livedaw_engine.h`.
//!
//! Strings cross the boundary as UTF-8 (pointer + length) going in, and as
//! owned `LdString` handles coming out (free with `ld_string_free`).

use super::guard;
use livedaw_engine::{Engine, StripKind};
use livedaw_session::console::{CapturedInsert, InsertMode, PluginBridge, RestoreError};
use livedaw_session::layers::{FaderRef, Layer, LayerSet};
use livedaw_session::{ApplyResult, Show, StripRef};
use std::borrow::Cow;
use std::ffi::c_void;
use std::path::PathBuf;

// ---- strings -------------------------------------------------------------------------

pub struct LdString(String);

impl LdString {
    pub fn new(s: String) -> *mut LdString {
        Box::into_raw(Box::new(LdString(s)))
    }
}

/// # Safety
/// `p` must point to `len` readable bytes (or be null).
pub unsafe fn str_arg<'a>(p: *const u8, len: usize) -> Cow<'a, str> {
    if p.is_null() || len == 0 {
        return Cow::Borrowed("");
    }
    String::from_utf8_lossy(std::slice::from_raw_parts(p, len))
}

#[no_mangle]
pub unsafe extern "C" fn ld_string_data(s: *const LdString) -> *const u8 {
    s.as_ref().map_or(std::ptr::null(), |s| s.0.as_ptr())
}

#[no_mangle]
pub unsafe extern "C" fn ld_string_len(s: *const LdString) -> usize {
    s.as_ref().map_or(0, |s| s.0.len())
}

#[no_mangle]
pub unsafe extern "C" fn ld_string_free(s: *mut LdString) {
    if !s.is_null() {
        drop(Box::from_raw(s));
    }
}

/// A growable string the host writes into from a callback.
pub struct LdSink(String);

#[no_mangle]
pub unsafe extern "C" fn ld_sink_append(sink: *mut LdSink, utf8: *const u8, len: usize) {
    if let Some(sink) = sink.as_mut() {
        sink.0.push_str(&str_arg(utf8, len));
    }
}

// ---- plugin bridge ----------------------------------------------------------------------

/// Writes the plugin description (`<PLUGIN .../>` XML) and base64 state of FX
/// slot `slot`; returns false if the slot holds no plugin.
pub type CaptureFxFn = unsafe extern "C" fn(ctx: *mut c_void, slot: i32, description: *mut LdSink, state: *mut LdSink) -> bool;

/// mode 0 = restore state into the plugin already loaded, 1 = create the plugin.
/// Returns 0 on success, 1 if a different plugin is loaded (mode 0), 2 on failure
/// (with a message written to `error`).
pub type RestoreFxFn = unsafe extern "C" fn(
    ctx: *mut c_void,
    slot: i32,
    mode: i32,
    description: *const u8,
    description_len: usize,
    state: *const u8,
    state_len: usize,
    error: *mut LdSink,
) -> i32;

/// Empties FX slot `slot` (show load, before the saved plugins are created).
pub type ClearFxFn = unsafe extern "C" fn(ctx: *mut c_void, slot: i32);

#[repr(C)]
#[derive(Clone, Copy)]
pub struct LdPluginBridge {
    pub ctx: *mut c_void,
    pub capture: Option<CaptureFxFn>,
    pub restore: Option<RestoreFxFn>,
    pub clear: Option<ClearFxFn>,
}

impl PluginBridge for LdPluginBridge {
    fn capture_fx(&mut self, slot: usize) -> Option<CapturedInsert> {
        let f = self.capture?;
        let (mut desc, mut state) = (LdSink(String::new()), LdSink(String::new()));
        // SAFETY: host contract (see CaptureFxFn).
        let ok = unsafe { f(self.ctx, slot as i32, &mut desc, &mut state) };
        ok.then_some(CapturedInsert { description: desc.0, state: state.0 })
    }

    fn restore_fx(&mut self, slot: usize, mode: InsertMode, description: &str, state: &str) -> Result<(), RestoreError> {
        let Some(f) = self.restore else { return Err(RestoreError::Failed("plugin hosting is not available".into())) };
        let mut error = LdSink(String::new());
        let mode = if mode == InsertMode::Instantiate { 1 } else { 0 };
        // SAFETY: host contract (see RestoreFxFn).
        let status = unsafe {
            f(self.ctx, slot as i32, mode, description.as_ptr(), description.len(), state.as_ptr(), state.len(), &mut error)
        };
        match status {
            0 => Ok(()),
            1 => Err(RestoreError::Mismatch),
            _ => Err(RestoreError::Failed(if error.0.is_empty() { "unknown error".into() } else { error.0 })),
        }
    }

    fn clear_fx(&mut self, slot: usize) {
        if let Some(f) = self.clear {
            // SAFETY: host contract (see ClearFxFn).
            unsafe { f(self.ctx, slot as i32) };
        }
    }
}

// ---- shared types ---------------------------------------------------------------------

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct LdStripRef {
    pub kind: i32,
    pub id: i32,
}

impl From<StripRef> for LdStripRef {
    fn from(r: StripRef) -> Self {
        Self { kind: r.kind as i32, id: r.id }
    }
}

impl LdStripRef {
    fn to_ref(self) -> StripRef {
        StripRef::new(StripKind::from_i32(self.kind).unwrap_or(StripKind::Channel), self.id)
    }
}

unsafe fn refs_arg(p: *const LdStripRef, n: i32) -> Vec<StripRef> {
    if p.is_null() || n <= 0 {
        return Vec::new();
    }
    std::slice::from_raw_parts(p, n as usize).iter().map(|r| r.to_ref()).collect()
}

unsafe fn refs_out(refs: &[StripRef], out: *mut LdStripRef, capacity: i32) -> i32 {
    if !out.is_null() {
        for (i, r) in refs.iter().take(capacity.max(0) as usize).enumerate() {
            *out.add(i) = (*r).into();
        }
    }
    refs.len() as i32
}

/// A fader on the surface: kind 0..4 = strip kinds (see `LdStripRef`), 5 = DCA (id 0..15).
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct LdFaderRef {
    pub kind: i32,
    pub id: i32,
}

pub const FADER_DCA: i32 = 5;

impl From<FaderRef> for LdFaderRef {
    fn from(f: FaderRef) -> Self {
        match f {
            FaderRef::Strip(r) => Self { kind: r.kind as i32, id: r.id },
            FaderRef::Dca(i) => Self { kind: FADER_DCA, id: i },
        }
    }
}

impl LdFaderRef {
    fn to_ref(self) -> FaderRef {
        if self.kind == FADER_DCA {
            FaderRef::Dca(self.id)
        } else {
            FaderRef::Strip(LdStripRef { kind: self.kind, id: self.id }.to_ref())
        }
    }
}

unsafe fn faders_arg(p: *const LdFaderRef, n: i32) -> Vec<FaderRef> {
    if p.is_null() || n <= 0 {
        return Vec::new();
    }
    std::slice::from_raw_parts(p, n as usize).iter().map(|r| r.to_ref()).collect()
}

unsafe fn faders_out(faders: &[FaderRef], out: *mut LdFaderRef, capacity: i32) -> i32 {
    if !out.is_null() {
        for (i, f) in faders.iter().take(capacity.max(0) as usize).enumerate() {
            *out.add(i) = (*f).into();
        }
    }
    faders.len() as i32
}

#[repr(C)]
pub struct LdApplyResult {
    pub strips_applied: i32,
    pub strips_skipped_safe: i32,
    pub strips_missing: i32,
    /// Warnings joined by '\n', or null. Owned by the caller.
    pub warnings: *mut LdString,
}

unsafe fn write_result(out: *mut LdApplyResult, r: ApplyResult) {
    if let Some(out) = out.as_mut() {
        *out = LdApplyResult {
            strips_applied: r.strips_applied,
            strips_skipped_safe: r.strips_skipped_safe,
            strips_missing: r.strips_missing,
            warnings: if r.warnings.is_empty() { std::ptr::null_mut() } else { LdString::new(r.warnings.join("\n")) },
        };
    }
}

unsafe fn write_string(out: *mut *mut LdString, s: String) {
    if let Some(out) = out.as_mut() {
        *out = LdString::new(s);
    }
}

// ---- show -----------------------------------------------------------------------------

pub struct LdShow {
    show: Show,
    bridge: LdPluginBridge,
}

unsafe fn show<'a>(s: *mut LdShow) -> Option<&'a mut LdShow> {
    s.as_mut()
}

unsafe fn eng<'a>(e: *const Engine) -> Option<&'a Engine> {
    e.as_ref()
}

#[no_mangle]
pub unsafe extern "C" fn ld_show_create(bridge: *const LdPluginBridge) -> *mut LdShow {
    let bridge = bridge.as_ref().copied().unwrap_or(LdPluginBridge {
        ctx: std::ptr::null_mut(),
        capture: None,
        restore: None,
        clear: None,
    });
    guard(std::ptr::null_mut(), || Box::into_raw(Box::new(LdShow { show: Show::new(), bridge })))
}

#[no_mangle]
pub unsafe extern "C" fn ld_show_destroy(s: *mut LdShow) {
    if !s.is_null() {
        guard((), || drop(Box::from_raw(s)));
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_show_name(s: *mut LdShow) -> *mut LdString {
    LdString::new(show(s).map(|s| s.show.name.clone()).unwrap_or_default())
}

#[no_mangle]
pub unsafe extern "C" fn ld_show_set_name(s: *mut LdShow, utf8: *const u8, len: usize) {
    if let Some(s) = show(s) {
        s.show.name = str_arg(utf8, len).into_owned();
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_show_num_scenes(s: *mut LdShow) -> i32 {
    show(s).map_or(0, |s| s.show.num_scenes() as i32)
}

#[no_mangle]
pub unsafe extern "C" fn ld_show_current_scene(s: *mut LdShow) -> i32 {
    show(s).map_or(-1, |s| s.show.current_scene())
}

#[no_mangle]
pub unsafe extern "C" fn ld_show_add_scene(s: *mut LdShow, e: *const Engine, name: *const u8, len: usize, index: i32) -> i32 {
    let (Some(s), Some(e)) = (show(s), eng(e)) else { return -1 };
    guard(-1, || s.show.add_scene(e, &mut s.bridge, &str_arg(name, len), index))
}

#[no_mangle]
pub unsafe extern "C" fn ld_show_store_scene(s: *mut LdShow, e: *const Engine, index: i32) {
    if let (Some(s), Some(e)) = (show(s), eng(e)) {
        guard((), || s.show.store_scene(e, &mut s.bridge, index));
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_show_remove_scene(s: *mut LdShow, index: i32) {
    if let Some(s) = show(s) {
        guard((), || s.show.remove_scene(index));
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_show_move_scene(s: *mut LdShow, from: i32, to: i32) {
    if let Some(s) = show(s) {
        guard((), || s.show.move_scene(from, to));
    }
}

/// which: 0 = the scene at `index`, 1 = next (GO NEXT), 2 = previous (GO PREV),
/// 3 = the first scene tagged `index` (#1..#128). Skipped scenes are passed
/// over; a linked scene also recalls the one below it.
#[no_mangle]
pub unsafe extern "C" fn ld_show_recall_scene(s: *mut LdShow, e: *const Engine, which: i32, index: i32, out: *mut LdApplyResult) {
    let (Some(s), Some(e)) = (show(s), eng(e)) else { return write_result(out, ApplyResult::default()) };
    let r = guard(ApplyResult::default(), || match which {
        1 => s.show.recall_next(e, &mut s.bridge),
        2 => s.show.recall_previous(e, &mut s.bridge),
        3 => s.show.recall_by_tag(e, &mut s.bridge, index),
        _ => s.show.recall_scene(e, &mut s.bridge, index),
    });
    write_result(out, r);
}

/// The first (non-skipped) scene tagged `tag`, or -1.
#[no_mangle]
pub unsafe extern "C" fn ld_show_find_scene_by_tag(s: *mut LdShow, tag: i32) -> i32 {
    show(s).map_or(-1, |s| s.show.find_scene_by_tag(tag))
}

// Scene fields -----------------------------------------------------------------------

#[no_mangle]
pub unsafe extern "C" fn ld_scene_exists(s: *mut LdShow, scene: i32) -> bool {
    show(s).is_some_and(|s| s.show.scene(scene).is_some())
}

#[no_mangle]
pub unsafe extern "C" fn ld_scene_name(s: *mut LdShow, scene: i32) -> *mut LdString {
    LdString::new(show(s).and_then(|s| s.show.scene(scene)).map(|sc| sc.name.clone()).unwrap_or_default())
}

#[no_mangle]
pub unsafe extern "C" fn ld_scene_notes(s: *mut LdShow, scene: i32) -> *mut LdString {
    LdString::new(show(s).and_then(|s| s.show.scene(scene)).map(|sc| sc.notes.clone()).unwrap_or_default())
}

#[no_mangle]
pub unsafe extern "C" fn ld_scene_scope(s: *mut LdShow, scene: i32) -> u32 {
    show(s).and_then(|s| s.show.scene(scene)).map_or(0, |sc| sc.scope)
}

#[no_mangle]
pub unsafe extern "C" fn ld_scene_fade_seconds(s: *mut LdShow, scene: i32) -> f64 {
    show(s).and_then(|s| s.show.scene(scene)).map_or(0.0, |sc| sc.fade_seconds)
}

#[no_mangle]
pub unsafe extern "C" fn ld_scene_num_snippets(s: *mut LdShow, scene: i32) -> i32 {
    show(s).and_then(|s| s.show.scene(scene)).map_or(0, |sc| sc.snippets.len() as i32)
}

/// EDIT TAG: #1..#128, 0 = none.
#[no_mangle]
pub unsafe extern "C" fn ld_scene_tag(s: *mut LdShow, scene: i32) -> i32 {
    show(s).and_then(|s| s.show.scene(scene)).map_or(0, |sc| sc.tag)
}

#[no_mangle]
pub unsafe extern "C" fn ld_scene_skip(s: *mut LdShow, scene: i32) -> bool {
    show(s).and_then(|s| s.show.scene(scene)).is_some_and(|sc| sc.skip)
}

/// LINK: recalling the scene also recalls the one below it.
#[no_mangle]
pub unsafe extern "C" fn ld_scene_link(s: *mut LdShow, scene: i32) -> bool {
    show(s).and_then(|s| s.show.scene(scene)).is_some_and(|sc| sc.link_next)
}

/// Out-of-range tags clear the tag.
#[no_mangle]
pub unsafe extern "C" fn ld_scene_set_tag(s: *mut LdShow, scene: i32, tag: i32) {
    if let Some(sc) = show(s).and_then(|s| s.show.scene_mut(scene)) {
        sc.tag = livedaw_session::show::clamp_scene_tag(tag);
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_scene_set_skip(s: *mut LdShow, scene: i32, skip: bool) {
    if let Some(sc) = show(s).and_then(|s| s.show.scene_mut(scene)) {
        sc.skip = skip;
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_scene_set_link(s: *mut LdShow, scene: i32, link: bool) {
    if let Some(sc) = show(s).and_then(|s| s.show.scene_mut(scene)) {
        sc.link_next = link;
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_scene_set_name(s: *mut LdShow, scene: i32, utf8: *const u8, len: usize) {
    if let Some(sc) = show(s).and_then(|s| s.show.scene_mut(scene)) {
        sc.name = str_arg(utf8, len).into_owned();
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_scene_set_notes(s: *mut LdShow, scene: i32, utf8: *const u8, len: usize) {
    if let Some(sc) = show(s).and_then(|s| s.show.scene_mut(scene)) {
        sc.notes = str_arg(utf8, len).into_owned();
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_scene_set_scope(s: *mut LdShow, scene: i32, scope: u32) {
    if let Some(sc) = show(s).and_then(|s| s.show.scene_mut(scene)) {
        sc.scope = scope & livedaw_session::section::ALL;
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_scene_set_fade_seconds(s: *mut LdShow, scene: i32, seconds: f64) {
    if let Some(sc) = show(s).and_then(|s| s.show.scene_mut(scene)) {
        sc.fade_seconds = if seconds.is_finite() { seconds.clamp(0.0, livedaw_session::show::MAX_FADE_SECONDS) } else { 0.0 };
    }
}

// Snippets ---------------------------------------------------------------------------

#[no_mangle]
pub unsafe extern "C" fn ld_show_add_snippet(
    s: *mut LdShow,
    e: *const Engine,
    scene: i32,
    name: *const u8,
    name_len: usize,
    sections: u32,
    strips: *const LdStripRef,
    num_strips: i32,
) -> i32 {
    let (Some(s), Some(e)) = (show(s), eng(e)) else { return -1 };
    let strips = refs_arg(strips, num_strips);
    let name = str_arg(name, name_len);
    guard(-1, || s.show.add_snippet(e, &mut s.bridge, scene, &name, sections, strips))
}

#[no_mangle]
pub unsafe extern "C" fn ld_show_store_snippet(s: *mut LdShow, e: *const Engine, scene: i32, snippet: i32) {
    if let (Some(s), Some(e)) = (show(s), eng(e)) {
        guard((), || s.show.store_snippet(e, &mut s.bridge, scene, snippet));
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_show_remove_snippet(s: *mut LdShow, scene: i32, snippet: i32) {
    if let Some(s) = show(s) {
        guard((), || s.show.remove_snippet(scene, snippet));
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_show_recall_snippet(
    s: *mut LdShow,
    e: *const Engine,
    scene: i32,
    snippet: i32,
    out: *mut LdApplyResult,
) {
    let (Some(s), Some(e)) = (show(s), eng(e)) else { return write_result(out, ApplyResult::default()) };
    let r = guard(ApplyResult::default(), || s.show.recall_snippet(e, &mut s.bridge, scene, snippet));
    write_result(out, r);
}

#[no_mangle]
pub unsafe extern "C" fn ld_snippet_exists(s: *mut LdShow, scene: i32, snippet: i32) -> bool {
    show(s).is_some_and(|s| s.show.snippet(scene, snippet).is_some())
}

#[no_mangle]
pub unsafe extern "C" fn ld_snippet_name(s: *mut LdShow, scene: i32, snippet: i32) -> *mut LdString {
    LdString::new(show(s).and_then(|s| s.show.snippet(scene, snippet)).map(|n| n.name.clone()).unwrap_or_default())
}

#[no_mangle]
pub unsafe extern "C" fn ld_snippet_sections(s: *mut LdShow, scene: i32, snippet: i32) -> u32 {
    show(s).and_then(|s| s.show.snippet(scene, snippet)).map_or(0, |n| n.sections)
}

#[no_mangle]
pub unsafe extern "C" fn ld_snippet_fire_with_scene(s: *mut LdShow, scene: i32, snippet: i32) -> bool {
    show(s).and_then(|s| s.show.snippet(scene, snippet)).is_some_and(|n| n.fire_with_scene)
}

/// Copies up to `capacity` strip refs into `out`; returns the total count.
#[no_mangle]
pub unsafe extern "C" fn ld_snippet_strips(s: *mut LdShow, scene: i32, snippet: i32, out: *mut LdStripRef, capacity: i32) -> i32 {
    show(s).and_then(|s| s.show.snippet(scene, snippet)).map_or(0, |n| refs_out(&n.strips, out, capacity))
}

#[no_mangle]
pub unsafe extern "C" fn ld_snippet_set_name(s: *mut LdShow, scene: i32, snippet: i32, utf8: *const u8, len: usize) {
    if let Some(n) = show(s).and_then(|s| s.show.snippet_mut(scene, snippet)) {
        n.name = str_arg(utf8, len).into_owned();
    }
}

/// Changes what the snippet recalls (takes effect for sections it captured;
/// store the snippet again to capture newly added ones).
#[no_mangle]
pub unsafe extern "C" fn ld_snippet_set_sections(s: *mut LdShow, scene: i32, snippet: i32, sections: u32) {
    if let Some(n) = show(s).and_then(|s| s.show.snippet_mut(scene, snippet)) {
        n.sections = sections & livedaw_session::section::ALL;
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_snippet_set_fire_with_scene(s: *mut LdShow, scene: i32, snippet: i32, fire: bool) {
    if let Some(n) = show(s).and_then(|s| s.show.snippet_mut(scene, snippet)) {
        n.fire_with_scene = fire;
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_snippet_set_strips(s: *mut LdShow, scene: i32, snippet: i32, strips: *const LdStripRef, n: i32) {
    let refs = refs_arg(strips, n);
    if let Some(sn) = show(s).and_then(|s| s.show.snippet_mut(scene, snippet)) {
        sn.strips = refs;
    }
}

// Fades ---------------------------------------------------------------------------------

#[no_mangle]
pub unsafe extern "C" fn ld_show_is_fading(s: *mut LdShow) -> bool {
    show(s).is_some_and(|s| s.show.is_fading())
}

#[no_mangle]
pub unsafe extern "C" fn ld_show_cancel_fade(s: *mut LdShow) {
    if let Some(s) = show(s) {
        guard((), || s.show.cancel_fade());
    }
}

/// Advances running fades (call at ~60 Hz). Returns true while still fading.
#[no_mangle]
pub unsafe extern "C" fn ld_show_tick(s: *mut LdShow) -> bool {
    show(s).is_some_and(|s| guard(false, || s.show.tick()))
}

// Files -----------------------------------------------------------------------------------

#[no_mangle]
pub unsafe extern "C" fn ld_show_to_xml(s: *mut LdShow, e: *const Engine) -> *mut LdString {
    let (Some(s), Some(e)) = (show(s), eng(e)) else { return LdString::new(String::new()) };
    LdString::new(guard(String::new(), || s.show.to_xml(e, &mut s.bridge).to_document()))
}

/// Loads a show from XML text. Returns false (with `error`) if nothing was
/// loaded; on success `warnings` may list plugins that failed to load.
#[no_mangle]
pub unsafe extern "C" fn ld_show_load_xml(
    s: *mut LdShow,
    e: *const Engine,
    utf8: *const u8,
    len: usize,
    load_plugins: bool,
    message: *mut *mut LdString,
) -> bool {
    let (Some(s), Some(e)) = (show(s), eng(e)) else { return false };
    let text = str_arg(utf8, len);
    finish_load(
        guard(Err("internal error while loading".into()), || s.show.load_from_text(e, &mut s.bridge, &text, load_plugins)),
        message,
    )
}

#[no_mangle]
pub unsafe extern "C" fn ld_show_save_file(
    s: *mut LdShow,
    e: *const Engine,
    path: *const u8,
    len: usize,
    error: *mut *mut LdString,
) -> bool {
    let (Some(s), Some(e)) = (show(s), eng(e)) else { return false };
    let path = PathBuf::from(str_arg(path, len).into_owned());
    match guard(Err("internal error while saving".into()), || s.show.save_to_file(e, &mut s.bridge, &path)) {
        Ok(()) => true,
        Err(msg) => {
            write_string(error, msg);
            false
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_show_load_file(
    s: *mut LdShow,
    e: *const Engine,
    path: *const u8,
    len: usize,
    load_plugins: bool,
    message: *mut *mut LdString,
) -> bool {
    let (Some(s), Some(e)) = (show(s), eng(e)) else { return false };
    let path = PathBuf::from(str_arg(path, len).into_owned());
    finish_load(
        guard(Err("internal error while loading".into()), || s.show.load_from_file(e, &mut s.bridge, &path, load_plugins)),
        message,
    )
}

/// Ok: warnings (if any) in `message`, returns true. Err: error in `message`, false.
unsafe fn finish_load(r: Result<Vec<String>, String>, message: *mut *mut LdString) -> bool {
    match r {
        Ok(warnings) => {
            if !warnings.is_empty() {
                write_string(message, warnings.join("\n"));
            }
            true
        }
        Err(msg) => {
            write_string(message, msg);
            false
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_show_clear(s: *mut LdShow, e: *const Engine, num_channels: i32) {
    if let (Some(s), Some(e)) = (show(s), eng(e)) {
        guard((), || s.show.clear(e, num_channels.max(0) as usize));
    }
}

// ---- layers ------------------------------------------------------------------------------

/// The show's custom control assignments as a `<CustomControls>` element.
#[no_mangle]
pub unsafe extern "C" fn ld_show_custom_controls(s: *mut LdShow) -> *mut LdString {
    LdString::new(show(s).map(|s| s.show.custom.to_xml().to_compact()).unwrap_or_default())
}

/// Replaces the custom control assignments (false: not a `<CustomControls>` element).
#[no_mangle]
pub unsafe extern "C" fn ld_show_set_custom_controls(s: *mut LdShow, utf8: *const u8, len: usize) -> bool {
    let Some(s) = show(s) else { return false };
    match livedaw_session::xml::parse(&str_arg(utf8, len)) {
        Ok(xml) if xml.has_tag("CustomControls") => {
            s.show.custom.from_xml(&xml);
            true
        }
        _ => false,
    }
}

/// The show's own layer set (valid as long as the show).
#[no_mangle]
pub unsafe extern "C" fn ld_show_layers(s: *mut LdShow) -> *mut LayerSet {
    show(s).map_or(std::ptr::null_mut(), |s| &mut s.show.layers as *mut _)
}

/// A standalone layer set (free with `ld_layers_destroy`).
#[no_mangle]
pub extern "C" fn ld_layers_create() -> *mut LayerSet {
    Box::into_raw(Box::default())
}

#[no_mangle]
pub unsafe extern "C" fn ld_layers_destroy(l: *mut LayerSet) {
    if !l.is_null() {
        drop(Box::from_raw(l));
    }
}

unsafe fn layers<'a>(l: *mut LayerSet) -> Option<&'a mut LayerSet> {
    l.as_mut()
}

/// Section: 0 = left (12 faders on the WING), 1 = center (8), 2 = right (4).
/// Width 0 = Auto (as many faders as fit, see `ld_layers_set_fitted_width`).
#[no_mangle]
pub unsafe extern "C" fn ld_layers_width(l: *mut LayerSet, section: i32) -> i32 {
    layers(l).map_or(0, |l| l.width(section as usize))
}

#[no_mangle]
pub unsafe extern "C" fn ld_layers_set_width(l: *mut LayerSet, section: i32, width: i32) {
    if let Some(l) = layers(l) {
        l.set_width(section as usize, width);
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_layers_set_fitted_width(l: *mut LayerSet, section: i32, width: i32) {
    if let Some(l) = layers(l) {
        l.set_fitted_width(section as usize, width);
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_layers_effective_width(l: *mut LayerSet, section: i32) -> i32 {
    layers(l).map_or(1, |l| l.effective_width(section as usize))
}

#[no_mangle]
pub unsafe extern "C" fn ld_layers_selected(l: *mut LayerSet, section: i32) -> *mut LdString {
    LdString::new(layers(l).map(|l| l.selected(section as usize).to_owned()).unwrap_or_default())
}

#[no_mangle]
pub unsafe extern "C" fn ld_layers_set_selected(l: *mut LayerSet, section: i32, utf8: *const u8, len: usize) {
    if let Some(l) = layers(l) {
        l.select(section as usize, &str_arg(utf8, len));
    }
}

/// The section's default layer containing `f`, or an empty string.
#[no_mangle]
pub unsafe extern "C" fn ld_layers_find_default_containing(
    l: *mut LayerSet,
    e: *const Engine,
    section: i32,
    f: LdFaderRef,
) -> *mut LdString {
    let (Some(l), Some(e)) = (layers(l), eng(e)) else { return LdString::new(String::new()) };
    LdString::new(guard(None, || l.find_default_layer_containing(e, section as usize, &f.to_ref())).unwrap_or_default())
}

pub struct LdLayerList(Vec<Layer>);

/// The layers of a section. which: 0 = default layers, 1 = default + user
/// layers, 2 = user layers only, 3 = the single layer `id` (or the section's
/// first layer if it doesn't exist).
#[no_mangle]
pub unsafe extern "C" fn ld_layers_list(
    l: *mut LayerSet,
    e: *const Engine,
    section: i32,
    which: i32,
    id: *const u8,
    id_len: usize,
) -> *mut LdLayerList {
    let Some(l) = layers(l) else { return Box::into_raw(Box::new(LdLayerList(Vec::new()))) };
    let id = str_arg(id, id_len);
    let list = guard(Vec::new(), || match (which, eng(e)) {
        (2, _) => l.user_layers().to_vec(),
        (0, Some(e)) => l.default_layers(e, section as usize),
        (1, Some(e)) => l.all_layers(e, section as usize),
        (3, Some(e)) => vec![l.find_layer(e, section as usize, &id)],
        _ => Vec::new(),
    });
    Box::into_raw(Box::new(LdLayerList(list)))
}

#[no_mangle]
pub unsafe extern "C" fn ld_layer_list_free(list: *mut LdLayerList) {
    if !list.is_null() {
        drop(Box::from_raw(list));
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_layer_list_len(list: *const LdLayerList) -> i32 {
    list.as_ref().map_or(0, |l| l.0.len() as i32)
}

unsafe fn layer_at<'a>(list: *const LdLayerList, i: i32) -> Option<&'a Layer> {
    list.as_ref().and_then(|l| usize::try_from(i).ok().and_then(|i| l.0.get(i)))
}

#[no_mangle]
pub unsafe extern "C" fn ld_layer_list_id(list: *const LdLayerList, i: i32) -> *mut LdString {
    LdString::new(layer_at(list, i).map(|l| l.id.clone()).unwrap_or_default())
}

#[no_mangle]
pub unsafe extern "C" fn ld_layer_list_name(list: *const LdLayerList, i: i32) -> *mut LdString {
    LdString::new(layer_at(list, i).map(|l| l.name.clone()).unwrap_or_default())
}

#[no_mangle]
pub unsafe extern "C" fn ld_layer_list_is_user(list: *const LdLayerList, i: i32) -> bool {
    layer_at(list, i).is_some_and(|l| l.user)
}

#[no_mangle]
pub unsafe extern "C" fn ld_layer_list_faders(list: *const LdLayerList, i: i32, out: *mut LdFaderRef, capacity: i32) -> i32 {
    layer_at(list, i).map_or(0, |l| faders_out(&l.faders, out, capacity))
}

/// Returns the new layer's id.
#[no_mangle]
pub unsafe extern "C" fn ld_layers_add_user(
    l: *mut LayerSet,
    name: *const u8,
    name_len: usize,
    faders: *const LdFaderRef,
    n: i32,
) -> *mut LdString {
    let Some(l) = layers(l) else { return LdString::new(String::new()) };
    let faders = faders_arg(faders, n);
    LdString::new(l.add_user_layer(&str_arg(name, name_len), faders))
}

#[no_mangle]
pub unsafe extern "C" fn ld_layers_remove_user(l: *mut LayerSet, id: *const u8, len: usize) {
    if let Some(l) = layers(l) {
        l.remove_user_layer(&str_arg(id, len));
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_layers_rename_user(l: *mut LayerSet, id: *const u8, len: usize, name: *const u8, name_len: usize) {
    if let Some(l) = layers(l) {
        l.rename_user_layer(&str_arg(id, len), &str_arg(name, name_len));
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_layers_move_user(l: *mut LayerSet, id: *const u8, len: usize, delta: i32) {
    if let Some(l) = layers(l) {
        l.move_user_layer(&str_arg(id, len), delta);
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_layers_add_to_user(l: *mut LayerSet, id: *const u8, len: usize, r: LdFaderRef) {
    if let Some(l) = layers(l) {
        l.add_to_user_layer(&str_arg(id, len), r.to_ref());
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_layers_remove_from_user(l: *mut LayerSet, id: *const u8, len: usize, r: LdFaderRef) {
    if let Some(l) = layers(l) {
        l.remove_from_user_layer(&str_arg(id, len), &r.to_ref());
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_layers_move_within_user(l: *mut LayerSet, id: *const u8, len: usize, index: i32, delta: i32) {
    if let Some(l) = layers(l) {
        l.move_within_user_layer(&str_arg(id, len), index, delta);
    }
}

/// Replaces the faders of a section's default layer key (VIEW > CONFIG).
#[no_mangle]
pub unsafe extern "C" fn ld_layers_set_override(
    l: *mut LayerSet,
    section: i32,
    id: *const u8,
    len: usize,
    faders: *const LdFaderRef,
    n: i32,
) {
    if let (Some(l), Ok(section)) = (layers(l), usize::try_from(section)) {
        l.set_default_override(section, &str_arg(id, len), faders_arg(faders, n));
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_layers_clear_override(l: *mut LayerSet, section: i32, id: *const u8, len: usize) {
    if let (Some(l), Ok(section)) = (layers(l), usize::try_from(section)) {
        l.clear_default_override(section, &str_arg(id, len));
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_layers_has_override(l: *mut LayerSet, section: i32, id: *const u8, len: usize) -> bool {
    match (layers(l), usize::try_from(section)) {
        (Some(l), Ok(section)) => l.has_default_override(section, &str_arg(id, len)),
        _ => false,
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_layers_to_xml(l: *mut LayerSet) -> *mut LdString {
    LdString::new(layers(l).map(|l| l.to_xml().to_compact()).unwrap_or_default())
}

#[no_mangle]
pub unsafe extern "C" fn ld_layers_from_xml(l: *mut LayerSet, utf8: *const u8, len: usize) -> bool {
    let Some(l) = layers(l) else { return false };
    match livedaw_session::xml::parse(&str_arg(utf8, len)) {
        Ok(x) => {
            l.from_xml(&x);
            true
        }
        Err(_) => false,
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_layers_reset(l: *mut LayerSet) {
    if let Some(l) = layers(l) {
        l.reset();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    unsafe fn take(s: *mut LdString) -> String {
        let out = String::from_utf8_lossy(std::slice::from_raw_parts(ld_string_data(s), ld_string_len(s))).into_owned();
        ld_string_free(s);
        out
    }

    #[test]
    fn show_through_the_c_abi() {
        unsafe {
            let e = crate::ld_engine_create(0);
            crate::ld_engine_set_num_channels(e, 2);
            let s = ld_show_create(std::ptr::null());
            let idx = ld_show_add_scene(s, e, b"Intro".as_ptr(), 5, -1);
            assert_eq!(idx, 0);
            assert_eq!(take(ld_scene_name(s, 0)), "Intro");
            ld_scene_set_fade_seconds(s, 0, 1000.0);
            assert_eq!(ld_scene_fade_seconds(s, 0), 60.0);
            ld_scene_set_tag(s, 0, 12);
            ld_scene_set_link(s, 0, true);
            assert_eq!((ld_scene_tag(s, 0), ld_scene_skip(s, 0), ld_scene_link(s, 0)), (12, false, true));
            assert_eq!(ld_show_find_scene_by_tag(s, 12), 0);
            ld_scene_set_tag(s, 0, 129);
            assert_eq!(ld_scene_tag(s, 0), 0);
            ld_scene_set_skip(s, 0, true);
            assert!(ld_scene_skip(s, 0));
            ld_scene_set_skip(s, 0, false);
            ld_scene_set_link(s, 0, false);
            ld_scene_set_tag(s, 0, 12);

            let refs = [LdStripRef { kind: 0, id: 1 }];
            let faders = [LdFaderRef { kind: 0, id: 1 }, LdFaderRef { kind: FADER_DCA, id: 3 }];
            assert_eq!(ld_show_add_snippet(s, e, 0, b"S".as_ptr(), 1, 1 << 8, refs.as_ptr(), 1), 0);
            let mut out = [LdStripRef::default(); 4];
            assert_eq!(ld_snippet_strips(s, 0, 0, out.as_mut_ptr(), 4), 1);
            assert_eq!(out[0].id, 1);

            let mut r =
                LdApplyResult { strips_applied: 0, strips_skipped_safe: 0, strips_missing: 0, warnings: std::ptr::null_mut() };
            ld_show_recall_scene(s, e, 0, 0, &mut r);
            assert_eq!(r.strips_applied, 2 + 8 + 16 + 4 + 8);
            assert!(r.warnings.is_null());
            ld_show_recall_scene(s, e, 3, 12, &mut r); // by tag
            assert_eq!(r.strips_applied, 2 + 8 + 16 + 4 + 8);
            ld_show_recall_scene(s, e, 3, 13, &mut r);
            assert_eq!(r.strips_applied, 0);

            let xml = take(ld_show_to_xml(s, e));
            let mut msg = std::ptr::null_mut();
            assert!(ld_show_load_xml(s, e, xml.as_ptr(), xml.len(), true, &mut msg));
            assert!(msg.is_null());
            assert!(!ld_show_load_xml(s, e, b"<x/>".as_ptr(), 4, true, &mut msg));
            assert_eq!(take(msg), "Not a Live DAW show file");

            let l = ld_show_layers(s);
            let id = take(ld_layers_add_user(l, b"U".as_ptr(), 1, faders.as_ptr(), 2));
            let list = ld_layers_list(l, e, 0, 1, std::ptr::null(), 0);
            let n = ld_layer_list_len(list);
            assert_eq!(take(ld_layer_list_id(list, n - 1)), id);
            assert!(ld_layer_list_is_user(list, n - 1));
            let mut f = [LdFaderRef::default(); 4];
            assert_eq!(ld_layer_list_faders(list, n - 1, f.as_mut_ptr(), 4), 2);
            assert_eq!((f[1].kind, f[1].id), (FADER_DCA, 3));
            assert_eq!(take(ld_layer_list_name(list, 0)), "CH 1-2 · AUX 1-8");
            ld_layer_list_free(list);
            assert_eq!(take(ld_layers_selected(l, 2)), "out-1");
            assert_eq!(ld_layers_effective_width(l, 1), 8);

            let strip = crate::ld_engine_strip_at(e, 0, 0);
            crate::ld_strip_set_name(strip, "Bass".as_ptr(), 4);
            assert_eq!(take(crate::ld_strip_name(strip)), "Bass");

            ld_show_destroy(s);
            crate::ld_engine_destroy(e);
        }
    }
}
