//! C ABI for sessions: shows, scenes, snippets, layers and show files.
//! Message thread only. Mirrored in `include/livedaw_engine.h`.
//!
//! Strings cross the boundary as UTF-8 (pointer + length) going in, and as
//! owned `LdString` handles coming out (free with `ld_string_free`).

use super::guard;
use livedaw_engine::{Engine, StripKind};
use livedaw_session::console::{CapturedInsert, InsertMode, PluginBridge, RestoreError};
use livedaw_session::layers::{FaderRef, Layer, LayerSet, SpillKind};
use livedaw_session::library::{self, FileType};
use livedaw_session::presets::{self, PresetType};
use livedaw_session::{ApplyResult, ConsoleModel, GlobalSafe, Scope, Show, StripRef};
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

/// A fader on the surface: kind 0..4 = a strip (see `LdStripRef`), or with
/// `to_kind` 2..4 the send level from that strip to the bus, main or matrix
/// `to_id`; 5 = DCA (id 0..15); 6 = FX slot `id`'s parameter `to_id`;
/// 7 = MIDI CC `to_id` on MIDI channel `id`; 8 = a blank slot (ABI 14).
/// `to_kind` is -1 when unused.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct LdFaderRef {
    pub kind: i32,
    pub id: i32,
    pub to_kind: i32,
    pub to_id: i32,
}

impl Default for LdFaderRef {
    fn default() -> Self {
        Self { kind: 0, id: 0, to_kind: -1, to_id: 0 }
    }
}

pub const FADER_DCA: i32 = 5;
pub const FADER_FX_PARAM: i32 = 6;
pub const FADER_MIDI_CC: i32 = 7;
pub const FADER_EMPTY: i32 = 8;

impl From<FaderRef> for LdFaderRef {
    fn from(f: FaderRef) -> Self {
        match f {
            FaderRef::Empty => Self { kind: FADER_EMPTY, ..Self::default() },
            FaderRef::Strip(r) => Self { kind: r.kind as i32, id: r.id, ..Self::default() },
            FaderRef::Dca(i) => Self { kind: FADER_DCA, id: i, ..Self::default() },
            FaderRef::Send { from, to } => Self { kind: from.kind as i32, id: from.id, to_kind: to.kind as i32, to_id: to.id },
            FaderRef::FxParam { slot, param } => Self { kind: FADER_FX_PARAM, id: slot, to_kind: -1, to_id: param },
            FaderRef::MidiCc { channel, cc } => Self { kind: FADER_MIDI_CC, id: channel, to_kind: -1, to_id: cc },
        }
    }
}

impl LdFaderRef {
    fn to_ref(self) -> FaderRef {
        match self.kind {
            FADER_EMPTY => FaderRef::Empty,
            FADER_DCA => FaderRef::Dca(self.id),
            FADER_FX_PARAM => FaderRef::FxParam { slot: self.id, param: self.to_id },
            FADER_MIDI_CC => FaderRef::midi_cc(self.id, self.to_id),
            _ => {
                let from = LdStripRef { kind: self.kind, id: self.id }.to_ref();
                match StripKind::from_i32(self.to_kind) {
                    Some(k @ (StripKind::Bus | StripKind::Main | StripKind::Matrix)) => {
                        FaderRef::Send { from, to: StripRef::new(k, self.to_id) }
                    }
                    _ => FaderRef::Strip(from),
                }
            }
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
    pub(crate) show: Show,
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

/// SEND MIDI of the scenes the last ld_show_recall_scene recalled, one per
/// line, in order (empty if none); taking it clears it.
#[no_mangle]
pub unsafe extern "C" fn ld_show_take_midi(s: *mut LdShow) -> *mut LdString {
    LdString::new(show(s).map(|s| s.show.take_pending_midi().join("\n")).unwrap_or_default())
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

/// SEND MIDI: the MIDI data the scene sends when recalled.
#[no_mangle]
pub unsafe extern "C" fn ld_scene_midi(s: *mut LdShow, scene: i32) -> *mut LdString {
    LdString::new(show(s).and_then(|s| s.show.scene(scene)).map(|sc| sc.midi.clone()).unwrap_or_default())
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
pub unsafe extern "C" fn ld_scene_set_midi(s: *mut LdShow, scene: i32, utf8: *const u8, len: usize) {
    if let Some(sc) = show(s).and_then(|s| s.show.scene_mut(scene)) {
        sc.midi = str_arg(utf8, len).trim().to_owned();
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

// ---- scopes, global safes, partial load / update, snippet REC ------------------------------

/// A recall scope (see `livedaw_session::scope::Scope`). Going in, `strips`
/// points at `num_strips` refs (ignored with `all_strips`); coming out, the
/// strips go to a separate buffer and `strips` is null.
#[repr(C)]
pub struct LdScope {
    pub sections: u32,
    pub dcas: u32,
    pub mute_groups: u32,
    pub fx: u32,
    pub console: bool,
    pub all_strips: bool,
    pub strips: *const LdStripRef,
    pub num_strips: i32,
}

unsafe fn scope_arg(p: *const LdScope) -> Option<Scope> {
    let p = p.as_ref()?;
    Some(
        Scope {
            sections: p.sections,
            strips: if p.all_strips { None } else { Some(refs_arg(p.strips, p.num_strips)) },
            dcas: p.dcas,
            mute_groups: p.mute_groups,
            fx: p.fx,
            console: p.console,
        }
        .sanitised(),
    )
}

/// Writes `scope` to `out` and up to `capacity` strips to `strips`; returns the strip count.
unsafe fn scope_out(scope: &Scope, out: *mut LdScope, strips: *mut LdStripRef, capacity: i32) -> i32 {
    if let Some(o) = out.as_mut() {
        *o = LdScope {
            sections: scope.sections,
            dcas: scope.dcas,
            mute_groups: scope.mute_groups,
            fx: scope.fx,
            console: scope.console,
            all_strips: scope.strips.is_none(),
            strips: std::ptr::null(),
            num_strips: scope.strips.as_ref().map_or(0, |s| s.len() as i32),
        };
    }
    scope.strips.as_ref().map_or(0, |s| refs_out(s, strips, capacity))
}

/// A scene's recall scope; returns the number of strips (0 with all_strips).
#[no_mangle]
pub unsafe extern "C" fn ld_scene_get_scope(
    s: *mut LdShow,
    scene: i32,
    out: *mut LdScope,
    strips: *mut LdStripRef,
    capacity: i32,
) -> i32 {
    match show(s).and_then(|s| s.show.scene(scene)) {
        Some(sc) => scope_out(&sc.recall_scope(), out, strips, capacity),
        None => scope_out(&Scope::default(), out, strips, capacity),
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_scene_set_recall_scope(s: *mut LdShow, scene: i32, scope: *const LdScope) {
    if let (Some(sc), Some(scope)) = (show(s).and_then(|s| s.show.scene_mut(scene)), scope_arg(scope)) {
        sc.set_recall_scope(&scope);
    }
}

/// PARTIAL UPDATE: stores only `scope` into the scene.
#[no_mangle]
pub unsafe extern "C" fn ld_show_store_scene_partial(s: *mut LdShow, e: *const Engine, index: i32, scope: *const LdScope) {
    if let (Some(s), Some(e), Some(scope)) = (show(s), eng(e), scope_arg(scope)) {
        guard((), || s.show.store_scene_partial(e, &mut s.bridge, index, &scope));
    }
}

/// PARTIAL LOAD (`scope`; `ignore_safes` = NO GLOB) or RESTORE (`scope` null:
/// the whole scene, safes included).
#[no_mangle]
pub unsafe extern "C" fn ld_show_recall_scene_with(
    s: *mut LdShow,
    e: *const Engine,
    index: i32,
    scope: *const LdScope,
    ignore_safes: bool,
    out: *mut LdApplyResult,
) {
    let (Some(s), Some(e)) = (show(s), eng(e)) else { return write_result(out, ApplyResult::default()) };
    let scope = scope_arg(scope);
    let r = guard(ApplyResult::default(), || s.show.recall_scene_with(e, &mut s.bridge, index, scope.as_ref(), ignore_safes));
    write_result(out, r);
}

/// The scope PARTIAL LOAD / UPDATE last used.
#[no_mangle]
pub unsafe extern "C" fn ld_show_partial_scope(s: *mut LdShow, out: *mut LdScope, strips: *mut LdStripRef, capacity: i32) -> i32 {
    match show(s) {
        Some(s) => scope_out(&s.show.partial_scope, out, strips, capacity),
        None => scope_out(&Scope::default(), out, strips, capacity),
    }
}

/// The GLOBAL safes beyond strips (sections, DCAs, mute groups, FX, console; no strips).
#[no_mangle]
pub unsafe extern "C" fn ld_show_global_safe(s: *mut LdShow, out: *mut LdScope) {
    let g = show(s).map(|s| s.show.global_safe.clone()).unwrap_or_default();
    let scope = Scope {
        sections: g.sections,
        strips: Some(Vec::new()),
        dcas: g.dcas,
        mute_groups: g.mute_groups,
        fx: g.fx,
        console: g.console,
    };
    scope_out(&scope, out, std::ptr::null_mut(), 0);
}

#[no_mangle]
pub unsafe extern "C" fn ld_show_set_global_safe(s: *mut LdShow, safe: *const LdScope) {
    if let (Some(s), Some(g)) = (show(s), scope_arg(safe)) {
        s.show.global_safe =
            GlobalSafe { sections: g.sections, dcas: g.dcas, mute_groups: g.mute_groups, fx: g.fx, console: g.console }
                .sanitised();
    }
}

/// A snippet's scope (strips, sections, DCAs, mute groups, FX, console);
/// returns the number of strips.
#[no_mangle]
pub unsafe extern "C" fn ld_snippet_get_scope(
    s: *mut LdShow,
    scene: i32,
    snippet: i32,
    out: *mut LdScope,
    strips: *mut LdStripRef,
    capacity: i32,
) -> i32 {
    match show(s).and_then(|s| s.show.snippet(scene, snippet)) {
        Some(sn) => scope_out(&sn.scope(), out, strips, capacity),
        None => scope_out(&Scope::strips(0, Vec::new()), out, strips, capacity),
    }
}

/// The parameter count shown for a snippet (what REC recorded, else the values it holds).
#[no_mangle]
pub unsafe extern "C" fn ld_snippet_parameter_count(s: *mut LdShow, scene: i32, snippet: i32) -> i32 {
    show(s).and_then(|s| s.show.snippet(scene, snippet)).map_or(0, |sn| sn.parameter_count() as i32)
}

#[no_mangle]
pub unsafe extern "C" fn ld_show_add_snippet_scoped(
    s: *mut LdShow,
    e: *const Engine,
    scene: i32,
    name: *const u8,
    name_len: usize,
    scope: *const LdScope,
) -> i32 {
    let (Some(s), Some(e), Some(scope)) = (show(s), eng(e), scope_arg(scope)) else { return -1 };
    let name = str_arg(name, name_len);
    guard(-1, || s.show.add_snippet_scoped(e, &mut s.bridge, scene, &name, &scope))
}

/// ADD ITEMS (`add`) or REMOVE ITEMS: no strips in `items` means every strip of the snippet.
#[no_mangle]
pub unsafe extern "C" fn ld_show_snippet_items(
    s: *mut LdShow,
    e: *const Engine,
    scene: i32,
    snippet: i32,
    add: bool,
    items: *const LdScope,
) {
    let (Some(s), Some(e), Some(items)) = (show(s), eng(e), scope_arg(items)) else { return };
    guard((), || {
        if add {
            s.show.add_snippet_items(e, &mut s.bridge, scene, snippet, &items);
        } else {
            s.show.remove_snippet_items(scene, snippet, &items);
        }
    });
}

/// PARTIAL LOAD of a snippet (`limit` null: all of it).
#[no_mangle]
pub unsafe extern "C" fn ld_show_recall_snippet_with(
    s: *mut LdShow,
    e: *const Engine,
    scene: i32,
    snippet: i32,
    limit: *const LdScope,
    out: *mut LdApplyResult,
) {
    let (Some(s), Some(e)) = (show(s), eng(e)) else { return write_result(out, ApplyResult::default()) };
    let limit = scope_arg(limit);
    let r = guard(ApplyResult::default(), || s.show.recall_snippet_with(e, &mut s.bridge, scene, snippet, limit.as_ref()));
    write_result(out, r);
}

/// Snippet REC. op: 0 REC, 1 STOP (returns the number of changed parameters),
/// 2 CLEAR (drop the recording), 3 the count so far (while recording) or of
/// the last STOP, 4 recording? (1/0), 5 a stopped recording waiting for SAVE? (1/0).
#[no_mangle]
pub unsafe extern "C" fn ld_show_snippet_rec(s: *mut LdShow, e: *const Engine, op: i32) -> i32 {
    let (Some(s), Some(e)) = (show(s), eng(e)) else { return 0 };
    guard(0, || match op {
        0 => {
            s.show.start_recording(e);
            0
        }
        1 => s.show.stop_recording(e) as i32,
        2 => {
            s.show.clear_recording();
            0
        }
        3 => s.show.recorded_count(e) as i32,
        4 => i32::from(s.show.is_recording()),
        5 => i32::from(s.show.recorded_changes(None).is_some_and(|c| !c.is_empty())),
        _ => 0,
    })
}

/// SAVE after STOP: a snippet of the recorded parameters within `focus` (null:
/// all of them). -1 if nothing was recorded.
#[no_mangle]
pub unsafe extern "C" fn ld_show_add_recorded_snippet(
    s: *mut LdShow,
    e: *const Engine,
    scene: i32,
    name: *const u8,
    name_len: usize,
    focus: *const LdScope,
) -> i32 {
    let (Some(s), Some(e)) = (show(s), eng(e)) else { return -1 };
    let name = str_arg(name, name_len);
    let focus = scope_arg(focus);
    guard(-1, || s.show.add_recorded_snippet(e, &mut s.bridge, scene, &name, focus.as_ref()))
}

// Fades ---------------------------------------------------------------------------------

#[no_mangle]
pub unsafe extern "C" fn ld_show_is_fading(s: *mut LdShow) -> bool {
    show(s).is_some_and(|s| s.show.is_fading())
}

#[no_mangle]
pub unsafe extern "C" fn ld_show_cancel_fade(s: *mut LdShow, e: *const Engine) {
    if let (Some(s), Some(e)) = (show(s), eng(e)) {
        guard((), || s.show.cancel_fade(e));
    }
}

/// Advances running fades (call at ~60 Hz). Returns true while still fading.
#[no_mangle]
pub unsafe extern "C" fn ld_show_tick(s: *mut LdShow, e: *const Engine) -> bool {
    let (Some(s), Some(e)) = (show(s), eng(e)) else { return false };
    guard(false, || s.show.tick(e))
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

// ---- LIBRARY presets (FX, CHAN) -------------------------------------------------------------

/// A preset file's header. `kind`: 0 = not a preset, 1 = FX, 2 = channel.
/// `name` and `detail` (the FX device, or the strip kind) are owned by the caller.
#[repr(C)]
pub struct LdPresetInfo {
    pub kind: i32,
    pub scope: u32,
    pub strip_kind: i32,
    pub name: *mut LdString,
    pub detail: *mut LdString,
}

/// Reads a preset file's header; false (kind 0, null strings) if it isn't one.
#[no_mangle]
pub unsafe extern "C" fn ld_preset_info(path: *const u8, len: usize, out: *mut LdPresetInfo) -> bool {
    let path = PathBuf::from(str_arg(path, len).into_owned());
    let info = guard(None, || presets::load(&path).ok().and_then(|p| presets::info(&p)));
    if let Some(o) = out.as_mut() {
        *o = match &info {
            Some(i) => LdPresetInfo {
                kind: if i.kind == PresetType::Fx { 1 } else { 2 },
                scope: i.scope,
                strip_kind: i.strip_kind.map_or(-1, |k| k as i32),
                name: LdString::new(i.name.clone()),
                detail: LdString::new(i.detail.clone()),
            },
            None => LdPresetInfo { kind: 0, scope: 0, strip_kind: -1, name: std::ptr::null_mut(), detail: std::ptr::null_mut() },
        };
    }
    info.is_some()
}

unsafe fn finish(r: Result<Vec<String>, String>, message: *mut *mut LdString) -> bool {
    finish_load(r, message)
}

/// SAVE on the FX tab: FX slot `slot` to `path`. False with `message` on failure.
#[no_mangle]
pub unsafe extern "C" fn ld_show_save_fx_preset(
    s: *mut LdShow,
    e: *const Engine,
    slot: i32,
    path: *const u8,
    path_len: usize,
    name: *const u8,
    name_len: usize,
    message: *mut *mut LdString,
) -> bool {
    let (Some(s), Some(e)) = (show(s), eng(e)) else { return false };
    let path = PathBuf::from(str_arg(path, path_len).into_owned());
    let name = str_arg(name, name_len).into_owned();
    let r = guard(Err("internal error".into()), || {
        let preset =
            presets::capture_fx(e, &mut s.bridge, usize::try_from(slot).unwrap_or(usize::MAX), &name).ok_or("No such FX slot")?;
        presets::save(&path, &preset).map(|()| Vec::new())
    });
    finish(r, message)
}

/// LOAD on the FX tab: the preset at `path` into FX slot `slot` (warnings in `message`).
#[no_mangle]
pub unsafe extern "C" fn ld_show_load_fx_preset(
    s: *mut LdShow,
    e: *const Engine,
    slot: i32,
    path: *const u8,
    path_len: usize,
    message: *mut *mut LdString,
) -> bool {
    let (Some(s), Some(e)) = (show(s), eng(e)) else { return false };
    let path = PathBuf::from(str_arg(path, path_len).into_owned());
    let r = guard(Err("internal error".into()), || {
        let preset = presets::load(&path)?;
        presets::apply_fx(e, &mut s.bridge, &preset, usize::try_from(slot).unwrap_or(usize::MAX))
    });
    finish(r, message)
}

/// SAVE on the CHAN tab: `strip` to `path`, loading `scope` (sections).
#[no_mangle]
pub unsafe extern "C" fn ld_show_save_channel_preset(
    s: *mut LdShow,
    e: *const Engine,
    strip: LdStripRef,
    scope: u32,
    path: *const u8,
    path_len: usize,
    name: *const u8,
    name_len: usize,
    message: *mut *mut LdString,
) -> bool {
    let (Some(s), Some(e)) = (show(s), eng(e)) else { return false };
    let path = PathBuf::from(str_arg(path, path_len).into_owned());
    let name = str_arg(name, name_len).into_owned();
    let r = guard(Err("internal error".into()), || {
        let preset = presets::capture_channel(e, &mut s.bridge, &strip.to_ref(), scope, &name).ok_or("No such strip")?;
        presets::save(&path, &preset).map(|()| Vec::new())
    });
    finish(r, message)
}

/// LOAD on the CHAN tab: the preset at `path` onto `strip` with its own scope
/// (`use_scope` false) or `scope`.
#[no_mangle]
pub unsafe extern "C" fn ld_show_load_channel_preset(
    s: *mut LdShow,
    e: *const Engine,
    strip: LdStripRef,
    path: *const u8,
    path_len: usize,
    use_scope: bool,
    scope: u32,
    message: *mut *mut LdString,
) -> bool {
    let (Some(s), Some(e)) = (show(s), eng(e)) else { return false };
    let path = PathBuf::from(str_arg(path, path_len).into_owned());
    let r = guard(Err("internal error".into()), || {
        let preset = presets::load(&path)?;
        presets::apply_channel(e, &mut s.bridge, &preset, &strip.to_ref(), use_scope.then_some(scope)).map(|r| r.warnings)
    });
    finish(r, message)
}

/// UPDATE SCOPE: rewrites a channel preset's scope.
#[no_mangle]
pub unsafe extern "C" fn ld_preset_set_scope(path: *const u8, len: usize, scope: u32) -> bool {
    let path = PathBuf::from(str_arg(path, len).into_owned());
    guard(false, || {
        let Ok(mut preset) = presets::load(&path) else { return false };
        if presets::info(&preset).is_none_or(|i| i.kind != PresetType::Channel) {
            return false;
        }
        presets::set_channel_scope(&mut preset, scope);
        presets::save(&path, &preset).is_ok()
    })
}

// ---- LIBRARY scene and snippet files (SNAP, SNIP) -------------------------------------------

/// A scene or snippet file's header. `kind`: 0 = not one, 1 = scene, 2 = snippet;
/// `count`: a scene's snippets, a snippet's parameters. `name` is owned by the caller.
#[repr(C)]
pub struct LdLibraryInfo {
    pub kind: i32,
    pub count: i32,
    pub name: *mut LdString,
}

/// Reads a scene or snippet file's header; false (kind 0, null name) if it isn't one.
#[no_mangle]
pub unsafe extern "C" fn ld_library_info(path: *const u8, len: usize, out: *mut LdLibraryInfo) -> bool {
    let path = PathBuf::from(str_arg(path, len).into_owned());
    let info = guard(None, || library::load(&path).ok().and_then(|f| library::info(&f)));
    if let Some(o) = out.as_mut() {
        *o = match &info {
            Some(i) => LdLibraryInfo {
                kind: if i.kind == FileType::Scene { 1 } else { 2 },
                count: i32::try_from(i.count).unwrap_or(i32::MAX),
                name: LdString::new(i.name.clone()),
            },
            None => LdLibraryInfo { kind: 0, count: 0, name: std::ptr::null_mut() },
        };
    }
    info.is_some()
}

/// Saves scene `scene` (snippet < 0, with its snippets) or snippet `snippet`
/// of it to `path`, named after the file. False with `message` on failure.
#[no_mangle]
pub unsafe extern "C" fn ld_show_export_library(
    s: *mut LdShow,
    scene: i32,
    snippet: i32,
    path: *const u8,
    path_len: usize,
    message: *mut *mut LdString,
) -> bool {
    let Some(s) = show(s) else { return false };
    let path = PathBuf::from(str_arg(path, path_len).into_owned());
    let r = guard(Err("internal error".into()), || library::export(&s.show, scene, snippet, &path).map(|()| Vec::new()));
    finish(r, message)
}

/// Adds the scene file at `path` to the show at `index` (-1: appended), or
/// the snippet file at `path` to scene `index`. Returns the new scene's or
/// snippet's index, or -1 with `message`.
#[no_mangle]
pub unsafe extern "C" fn ld_show_import_library(
    s: *mut LdShow,
    path: *const u8,
    path_len: usize,
    index: i32,
    message: *mut *mut LdString,
) -> i32 {
    let Some(s) = show(s) else { return -1 };
    let path = PathBuf::from(str_arg(path, path_len).into_owned());
    let r = guard(Err("internal error".into()), || {
        let file = library::load(&path)?;
        match library::info(&file).map(|i| i.kind) {
            Some(FileType::Scene) => library::import_scene(&mut s.show, &file, index),
            _ => library::import_snippet(&mut s.show, &file, index),
        }
    });
    match r {
        Ok(i) => i,
        Err(msg) => {
            write_string(message, msg);
            -1
        }
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

/// The console model the surface shows (0 full size, 1 COMPACT; see
/// `livedaw_session::model`): the COMPACT's section is the left one, 12
/// faders with nine layer keys. An app setting, kept by resets and loads.
#[no_mangle]
pub unsafe extern "C" fn ld_layers_set_model(l: *mut LayerSet, model: i32) {
    if let Some(l) = layers(l) {
        l.set_model(ConsoleModel::from_i32(model));
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_layers_model(l: *mut LayerSet) -> i32 {
    layers(l).map_or(0, |l| l.model().as_i32())
}

/// A console model's surface and local I/O (`livedaw_session::ModelIo`).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct LdConsoleModelInfo {
    pub faders: i32,
    pub layer_keys: i32,
    pub local_preamps: i32,
    pub local_outputs: i32,
    pub aux_inputs: i32,
    pub aux_outputs: i32,
    pub gpios: i32,
}

/// Fills `out` for `model` (unknown models: the full-size console's).
#[no_mangle]
pub unsafe extern "C" fn ld_console_model_info(model: i32, out: *mut LdConsoleModelInfo) {
    let Some(out) = out.as_mut() else { return };
    let io = ConsoleModel::from_i32(model).io();
    *out = LdConsoleModelInfo {
        faders: io.faders,
        layer_keys: io.layer_keys,
        local_preamps: io.local_preamps,
        local_outputs: io.local_outputs,
        aux_inputs: io.aux_inputs,
        aux_outputs: io.aux_outputs,
        gpios: io.gpios,
    };
}

#[no_mangle]
pub unsafe extern "C" fn ld_layers_effective_width(l: *mut LayerSet, section: i32) -> i32 {
    layers(l).map_or(1, |l| l.effective_width(section as usize))
}

/// The layer a section shows ("spill" while it spills).
#[no_mangle]
pub unsafe extern "C" fn ld_layers_selected(l: *mut LayerSet, section: i32) -> *mut LdString {
    LdString::new(layers(l).map(|l| l.shown(section as usize).to_owned()).unwrap_or_default())
}

/// The first fader a section shows (its scroll position).
#[no_mangle]
pub unsafe extern "C" fn ld_layers_offset(l: *mut LayerSet, section: i32) -> i32 {
    layers(l).map_or(0, |l| l.offset(section as usize))
}

#[no_mangle]
pub unsafe extern "C" fn ld_layers_set_offset(l: *mut LayerSet, section: i32, offset: i32) {
    if let Some(l) = layers(l) {
        l.set_offset(section as usize, offset);
    }
}

/// A spill: kind 0 = DCA `index`, 1 = the sources sending to bus slot `index`,
/// 2 = FX slot `index`'s parameters, 3 = automix group `index` (0 X, 1 Y),
/// 4 = the sends of `strip`, 5 = the strips feeding main `index` (ABI 17).
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct LdSpill {
    pub kind: i32,
    pub index: i32,
    pub strip: LdStripRef,
}

impl LdSpill {
    fn to_kind(self) -> Option<SpillKind> {
        Some(match self.kind {
            0 => SpillKind::Dca(self.index),
            1 => SpillKind::Bus(self.index),
            2 => SpillKind::Fx(self.index),
            3 => SpillKind::Automix(self.index),
            4 => SpillKind::Sends(self.strip.to_ref()),
            5 => SpillKind::Main(self.index),
            _ => return None,
        })
    }

    fn of(kind: SpillKind) -> Self {
        match kind {
            SpillKind::Dca(i) => Self { kind: 0, index: i, ..Self::default() },
            SpillKind::Bus(i) => Self { kind: 1, index: i, ..Self::default() },
            SpillKind::Fx(i) => Self { kind: 2, index: i, ..Self::default() },
            SpillKind::Automix(i) => Self { kind: 3, index: i, ..Self::default() },
            SpillKind::Sends(r) => Self { kind: 4, index: 0, strip: r.into() },
            SpillKind::Main(i) => Self { kind: 5, index: i, ..Self::default() },
        }
    }
}

/// The section a spill from `origin` goes to (-1: no section).
#[no_mangle]
pub extern "C" fn ld_layers_spill_target(origin: i32) -> i32 {
    livedaw_session::layers::spill_target(usize::try_from(origin).ok()) as i32
}

#[no_mangle]
pub unsafe extern "C" fn ld_layers_start_spill(l: *mut LayerSet, section: i32, spill: LdSpill) {
    if let (Some(l), Some(kind), Ok(section)) = (layers(l), spill.to_kind(), usize::try_from(section)) {
        l.start_spill(section, kind);
    }
}

/// Ends a section's spill (its layer and scroll position come back); false if none.
#[no_mangle]
pub unsafe extern "C" fn ld_layers_end_spill(l: *mut LayerSet, section: i32) -> bool {
    match (layers(l), usize::try_from(section)) {
        (Some(l), Ok(section)) => l.end_spill(section),
        _ => false,
    }
}

/// What a section spills; false if it doesn't.
#[no_mangle]
pub unsafe extern "C" fn ld_layers_spill(l: *mut LayerSet, section: i32, out: *mut LdSpill) -> bool {
    let Some(kind) = layers(l).and_then(|l| l.spill(usize::try_from(section).ok()?)) else { return false };
    if let Some(o) = out.as_mut() {
        *o = LdSpill::of(kind);
    }
    true
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

/// Adds `r` to a user layer's first blank slot or after its last (another
/// copy when the layer already holds it); false when the layer is full.
#[no_mangle]
pub unsafe extern "C" fn ld_layers_add_to_user(l: *mut LayerSet, id: *const u8, len: usize, r: LdFaderRef) -> bool {
    layers(l).is_some_and(|l| l.add_to_user_layer(&str_arg(id, len), r.to_ref()))
}

/// Blanks slot `index` of a user layer if it holds `r`; `index` -1: the
/// first slot holding `r`. Other copies of `r` stay (ABI 15).
#[no_mangle]
pub unsafe extern "C" fn ld_layers_remove_from_user(l: *mut LayerSet, id: *const u8, len: usize, r: LdFaderRef, index: i32) {
    if let Some(l) = layers(l) {
        l.remove_from_user_layer(&str_arg(id, len), &r.to_ref(), usize::try_from(index).ok());
    }
}

/// Inserts `n` faders (blanks too) before slot `index` of a user layer; the
/// slots from there on move right. False (nothing changed) when a filled
/// slot would go past `ld_layers_max_slots()`.
#[no_mangle]
pub unsafe extern "C" fn ld_layers_insert_user_slots(
    l: *mut LayerSet,
    id: *const u8,
    len: usize,
    index: i32,
    faders: *const LdFaderRef,
    n: i32,
) -> bool {
    match (layers(l), usize::try_from(index)) {
        (Some(l), Ok(index)) => l.insert_user_slots(&str_arg(id, len), index, &faders_arg(faders, n)),
        _ => false,
    }
}

/// The most slots a layer can have.
#[no_mangle]
pub extern "C" fn ld_layers_max_slots() -> i32 {
    livedaw_session::layers::MAX_SLOTS as i32
}

/// Replaces a user layer's faders, blanks (`FADER_EMPTY`) included.
#[no_mangle]
pub unsafe extern "C" fn ld_layers_set_user_faders(
    l: *mut LayerSet,
    id: *const u8,
    len: usize,
    faders: *const LdFaderRef,
    n: i32,
) {
    if let Some(l) = layers(l) {
        l.set_user_layer_faders(&str_arg(id, len), faders_arg(faders, n));
    }
}

/// Puts `r` in slot `index` of a user layer (replacing what was there).
#[no_mangle]
pub unsafe extern "C" fn ld_layers_set_user_slot(l: *mut LayerSet, id: *const u8, len: usize, index: i32, r: LdFaderRef) {
    if let (Some(l), Ok(index)) = (layers(l), usize::try_from(index)) {
        l.set_user_slot(&str_arg(id, len), index, r.to_ref());
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_layers_clear_user_slot(l: *mut LayerSet, id: *const u8, len: usize, index: i32) {
    if let (Some(l), Ok(index)) = (layers(l), usize::try_from(index)) {
        l.clear_user_slot(&str_arg(id, len), index);
    }
}

/// Swaps two slots of a user layer (a move when one is blank).
#[no_mangle]
pub unsafe extern "C" fn ld_layers_swap_user_slots(l: *mut LayerSet, id: *const u8, len: usize, a: i32, b: i32) {
    if let (Some(l), Ok(a), Ok(b)) = (layers(l), usize::try_from(a), usize::try_from(b)) {
        l.swap_user_slots(&str_arg(id, len), a, b);
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
    fn console_model_through_the_c_abi() {
        unsafe {
            let mut info = LdConsoleModelInfo::default();
            ld_console_model_info(1, &mut info);
            assert_eq!((info.local_preamps, info.gpios, info.aux_inputs, info.faders, info.layer_keys), (24, 2, 0, 13, 9));
            ld_console_model_info(0, &mut info);
            assert_eq!((info.local_preamps, info.gpios, info.aux_inputs, info.faders, info.layer_keys), (8, 4, 8, 24, 7));
            ld_console_model_info(1, std::ptr::null_mut());

            let l = ld_layers_create();
            assert_eq!(ld_layers_model(l), 0);
            ld_layers_set_width(l, 0, 0);
            ld_layers_set_model(l, 1);
            assert_eq!((ld_layers_model(l), ld_layers_effective_width(l, 0)), (1, 12));
            ld_layers_reset(l);
            assert_eq!(ld_layers_model(l), 1);
            ld_layers_set_model(l, 7); // unknown: the full-size console
            assert_eq!(ld_layers_model(l), 0);
            ld_layers_destroy(l);
            assert_eq!(ld_layers_model(std::ptr::null_mut()), 0);
        }
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
            let send = LdFaderRef { kind: 0, id: 1, to_kind: 2, to_id: 4 };
            let faders = [
                LdFaderRef { kind: 0, id: 1, ..Default::default() },
                LdFaderRef { kind: FADER_DCA, id: 3, ..Default::default() },
                send,
                LdFaderRef { kind: FADER_MIDI_CC, id: 2, to_kind: -1, to_id: 11 },
            ];
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
            // SEND MIDI
            ld_scene_set_midi(s, 0, b" B0,07,64 ".as_ptr(), 10);
            assert_eq!(take(ld_scene_midi(s, 0)), "B0,07,64");
            ld_show_recall_scene(s, e, 0, 0, &mut r);
            assert_eq!(take(ld_show_take_midi(s)), "B0,07,64");
            assert_eq!(take(ld_show_take_midi(s)), "");

            let xml = take(ld_show_to_xml(s, e));
            let mut msg = std::ptr::null_mut();
            assert!(ld_show_load_xml(s, e, xml.as_ptr(), xml.len(), true, &mut msg));
            assert!(msg.is_null());
            assert!(!ld_show_load_xml(s, e, b"<x/>".as_ptr(), 4, true, &mut msg));
            assert_eq!(take(msg), "Not a Sidecar show file");

            let l = ld_show_layers(s);
            let id = take(ld_layers_add_user(l, b"U".as_ptr(), 1, faders.as_ptr(), 4));
            let list = ld_layers_list(l, e, 0, 1, std::ptr::null(), 0);
            let n = ld_layer_list_len(list);
            assert_eq!(take(ld_layer_list_id(list, n - 1)), id);
            assert!(ld_layer_list_is_user(list, n - 1));
            let mut f = [LdFaderRef::default(); 4];
            assert_eq!(ld_layer_list_faders(list, n - 1, f.as_mut_ptr(), 4), 4);
            assert_eq!((f[3].kind, f[3].id, f[3].to_id), (FADER_MIDI_CC, 2, 11));
            assert_eq!((f[1].kind, f[1].id), (FADER_DCA, 3));
            assert_eq!((f[0].to_kind, f[2].kind, f[2].id, f[2].to_kind, f[2].to_id), (-1, 0, 1, 2, 4));
            assert_eq!(take(ld_layer_list_name(list, 0)), "CH 1-2 · AUX 1-8");
            ld_layer_list_free(list);

            // Blank slots: set, swap, clear; nothing shifts.
            ld_layers_set_user_slot(l, id.as_ptr(), id.len(), 6, LdFaderRef { kind: FADER_DCA, id: 9, ..Default::default() });
            ld_layers_clear_user_slot(l, id.as_ptr(), id.len(), 1);
            ld_layers_swap_user_slots(l, id.as_ptr(), id.len(), 0, 4);
            let list = ld_layers_list(l, e, 0, 2, std::ptr::null(), 0);
            let mut f = [LdFaderRef::default(); 8];
            assert_eq!(ld_layer_list_faders(list, 0, f.as_mut_ptr(), 8), 7);
            let kinds: Vec<i32> = f[..7].iter().map(|r| r.kind).collect();
            assert_eq!(kinds, [FADER_EMPTY, FADER_EMPTY, 0, FADER_MIDI_CC, 0, FADER_EMPTY, FADER_DCA]);
            ld_layer_list_free(list);
            let blanks = [
                LdFaderRef { kind: FADER_EMPTY, ..Default::default() },
                f[6],
                LdFaderRef { kind: FADER_EMPTY, ..Default::default() },
            ];
            ld_layers_set_user_faders(l, id.as_ptr(), id.len(), blanks.as_ptr(), 3);
            let list = ld_layers_list(l, e, 0, 2, std::ptr::null(), 0);
            assert_eq!(ld_layer_list_faders(list, 0, f.as_mut_ptr(), 8), 2, "trailing blanks dropped");
            assert_eq!((f[0].kind, f[1].kind, f[1].id), (FADER_EMPTY, FADER_DCA, 9));
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
