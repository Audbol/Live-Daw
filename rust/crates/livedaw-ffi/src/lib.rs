//! C ABI for the Sidecar engine. The C++ mirror is `include/livedaw_engine.h`.
//!
//! Conventions:
//! * every function is panic-safe: a panic is caught and a neutral value returned;
//! * strip pointers stay valid until the strip is removed (or the engine reset)
//!   *and* `ld_engine_collect` has run - the host drops its references first;
//! * functions marked "message thread" must not be called from the audio thread.

#![allow(clippy::missing_safety_doc)]

pub mod analysis;
pub mod codec;
pub mod console;
pub mod models;
pub mod monitor;
pub mod recorder;
pub mod session;
pub mod usb;
pub mod web;
pub mod wing;

use livedaw_engine::monitor::{MonitorControls, MonitorDspParams};
use livedaw_engine::params::{
    DcaParams, DynSlotParams, EqBlockParams, FilterParams, FxParams, InputParams, InsertParams, MainSendParams, MatrixSendParams,
    TapEqParams,
};
use livedaw_engine::params::{MonitorPathParams, MonitorSourceParams};
use livedaw_engine::sources::SourceParams;
use livedaw_engine::talk::{TalkParams, TalkPathParams};
use livedaw_engine::{pdc_tap, Engine, EngineControls, InsertCallbacks, SendParams, Strip, StripKind, StripMeters, StripParams};
use std::mem::{offset_of, size_of};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::Arc;

// ABI 14: LdFaderRef kind 8 = a blank layer slot.
// ABI 15: ld_layers_remove_from_user takes the slot (a user layer can hold a
// fader twice); ld_layers_add_to_user returns whether it had room.
// ABI 16: the console model (ld_console_model_info, ld_layers_set_model, ld_wing_set_model).
pub const LD_ABI_VERSION: i32 = 16;

fn guard<T>(default: T, f: impl FnOnce() -> T) -> T {
    catch_unwind(AssertUnwindSafe(f)).unwrap_or(default)
}

unsafe fn engine<'a>(e: *const Engine) -> Option<&'a Engine> {
    e.as_ref()
}

unsafe fn strip<'a>(s: *const Strip) -> Option<&'a Strip> {
    s.as_ref()
}

fn ptr(s: Option<Arc<Strip>>) -> *const Strip {
    // The engine's own Arc keeps the strip alive; see the module docs.
    s.map_or(std::ptr::null(), |a| Arc::as_ptr(&a))
}

// ---- layout verification -----------------------------------------------------

/// Sizes and offsets of the shared structs, compared with the C++ side at startup.
#[repr(C)]
#[derive(Default)]
pub struct LdLayoutInfo {
    pub abi_version: i32,
    pub strip_params: i32,
    pub input: i32,
    pub filter: i32,
    pub dyn_slot: i32,
    pub eq_block: i32,
    pub insert: i32,
    pub main_send: i32,
    pub matrix_send: i32,
    pub meters: i32,
    pub send: i32,
    pub dca: i32,
    pub fx: i32,
    pub controls: i32,
    pub off_filter: i32,
    pub off_gate: i32,
    pub off_eq: i32,
    pub off_dynamics: i32,
    pub off_post_insert: i32,
    pub off_fader: i32,
    pub off_stereo: i32,
    pub off_mains: i32,
    pub off_matrix: i32,
    pub off_dca_mask: i32,
    pub off_direct_level: i32,
    pub off_colour: i32,
    pub off_recall_safe: i32,
    pub off_dyn_values: i32,
    pub off_dyn_xo_depth: i32,
    pub off_eq_values: i32,
    pub off_send_pan: i32,
    pub off_meters_automix: i32,
    pub off_controls_dcas: i32,
    pub off_controls_fx: i32,
    pub off_fx_values: i32,
    // ABI 5
    pub off_send_mode: i32,
    pub off_matrix_send_level: i32,
    pub off_meters_trim_peak: i32,
    pub off_meters_pfl_peak: i32,
    pub off_controls_bus_solo_afl: i32,
    pub off_controls_automix_enable: i32,
    pub off_controls_dim_db: i32,
    pub off_controls_main_link: i32,
    // ABI 6
    pub off_alt_input_left: i32,
    pub off_alt_selected: i32,
    pub off_controls_input_select_global: i32,
    pub off_controls_global_alt: i32,
    pub rec_clip: i32,
    pub rec_playback_track: i32,
    pub rec_record_track: i32,
    pub rec_take: i32,
    // ABI 7: TAP WIDTH / TAP EQ, the analysis controls (RTA, scope, LISTEN)
    pub off_tap_width: i32,
    pub off_tap_eq: i32,
    pub tap_eq: i32,
    pub analysis: i32,
    pub off_analysis_eq: i32,
    pub off_analysis_filter_tap: i32,
    // ABI 7: console level (solo modes, talkback, monitor paths, sources)
    pub talk: i32,
    pub talk_path: i32,
    pub monitor_source: i32,
    pub monitor_path: i32,
    pub source: i32,
    pub off_controls_solo_mode: i32,
    pub off_controls_talk: i32,
    pub off_controls_speakers: i32,
    pub off_controls_phones: i32,
    pub off_talk_paths: i32,
    pub off_monitor_path_peak: i32,
    pub off_source_peak: i32,
    // ABI 8: monitor path processing, SOURCE SOLO, source tags
    pub monitor_dsp: i32,
    pub monitor_controls: i32,
    pub off_controls_monitor: i32,
    pub off_monitor_dsp_eq: i32,
    pub off_monitor_dsp_pan: i32,
    pub off_monitor_controls_source_solo: i32,
    pub off_source_mute_groups: i32,
    // ABI 11: matrix send PAN LINK, DCA icon and light
    pub off_matrix_send_pan_reversed: i32,
    pub off_dca_icon: i32,
    // ABI 12: LINK CUSTOMIZATION TO SOURCE
    pub off_cust_link: i32,
    // ABI 13: DCA SOLO BUS
    pub off_dca_mon: i32,
    // ABI 16: the console model (ld_console_model_info, ld_layers_set_model, ld_wing_set_model)
    pub console_model_info: i32,
}

#[no_mangle]
pub unsafe extern "C" fn ld_layout_info(out: *mut LdLayoutInfo) {
    let Some(out) = out.as_mut() else { return };
    *out = LdLayoutInfo {
        abi_version: LD_ABI_VERSION,
        strip_params: size_of::<StripParams>() as i32,
        input: size_of::<InputParams>() as i32,
        filter: size_of::<FilterParams>() as i32,
        dyn_slot: size_of::<DynSlotParams>() as i32,
        eq_block: size_of::<EqBlockParams>() as i32,
        insert: size_of::<InsertParams>() as i32,
        main_send: size_of::<MainSendParams>() as i32,
        matrix_send: size_of::<MatrixSendParams>() as i32,
        meters: size_of::<StripMeters>() as i32,
        send: size_of::<SendParams>() as i32,
        dca: size_of::<DcaParams>() as i32,
        fx: size_of::<FxParams>() as i32,
        controls: size_of::<EngineControls>() as i32,
        off_filter: offset_of!(StripParams, filter) as i32,
        off_gate: offset_of!(StripParams, gate) as i32,
        off_eq: offset_of!(StripParams, eq) as i32,
        off_dynamics: offset_of!(StripParams, dynamics) as i32,
        off_post_insert: offset_of!(StripParams, post_insert) as i32,
        off_fader: offset_of!(StripParams, fader_db) as i32,
        off_stereo: offset_of!(StripParams, stereo) as i32,
        off_mains: offset_of!(StripParams, mains) as i32,
        off_matrix: offset_of!(StripParams, matrix) as i32,
        off_dca_mask: offset_of!(StripParams, dca_mask) as i32,
        off_direct_level: offset_of!(StripParams, direct_level_db) as i32,
        off_colour: offset_of!(StripParams, colour) as i32,
        off_recall_safe: offset_of!(StripParams, recall_safe) as i32,
        off_dyn_values: offset_of!(DynSlotParams, values) as i32,
        off_dyn_xo_depth: offset_of!(DynSlotParams, xo_depth) as i32,
        off_eq_values: offset_of!(EqBlockParams, values) as i32,
        off_send_pan: offset_of!(SendParams, pan) as i32,
        off_meters_automix: offset_of!(StripMeters, automix_db) as i32,
        off_controls_dcas: offset_of!(EngineControls, dcas) as i32,
        off_controls_fx: offset_of!(EngineControls, fx) as i32,
        off_fx_values: offset_of!(FxParams, values) as i32,
        off_send_mode: offset_of!(SendParams, mode) as i32,
        off_matrix_send_level: offset_of!(MatrixSendParams, level_db) as i32,
        off_meters_trim_peak: offset_of!(StripMeters, trim_peak) as i32,
        off_meters_pfl_peak: offset_of!(StripMeters, pfl_peak) as i32,
        off_controls_bus_solo_afl: offset_of!(EngineControls, bus_solo_afl) as i32,
        off_controls_automix_enable: offset_of!(EngineControls, automix_enable) as i32,
        off_controls_dim_db: offset_of!(EngineControls, monitor_dim_db) as i32,
        off_controls_main_link: offset_of!(EngineControls, main_link) as i32,
        off_alt_input_left: offset_of!(StripParams, alt_input_left) as i32,
        off_alt_selected: offset_of!(StripParams, alt_selected) as i32,
        off_controls_input_select_global: offset_of!(EngineControls, input_select_global) as i32,
        off_controls_global_alt: offset_of!(EngineControls, global_alt) as i32,
        rec_clip: size_of::<recorder::LdRecClip>() as i32,
        rec_playback_track: size_of::<recorder::LdRecPlaybackTrack>() as i32,
        rec_record_track: size_of::<recorder::LdRecRecordTrack>() as i32,
        rec_take: size_of::<recorder::LdRecTake>() as i32,
        off_tap_width: offset_of!(StripParams, tap_width) as i32,
        off_tap_eq: offset_of!(StripParams, tap_eq) as i32,
        tap_eq: size_of::<TapEqParams>() as i32,
        analysis: size_of::<livedaw_engine::AnalysisControls>() as i32,
        off_analysis_eq: offset_of!(livedaw_engine::AnalysisControls, eq) as i32,
        off_analysis_filter_tap: offset_of!(livedaw_engine::AnalysisControls, filter_tap) as i32,
        talk: size_of::<TalkParams>() as i32,
        talk_path: size_of::<TalkPathParams>() as i32,
        monitor_source: size_of::<MonitorSourceParams>() as i32,
        monitor_path: size_of::<MonitorPathParams>() as i32,
        source: size_of::<SourceParams>() as i32,
        off_controls_solo_mode: offset_of!(EngineControls, solo_mode) as i32,
        off_controls_talk: offset_of!(EngineControls, talk) as i32,
        off_controls_speakers: offset_of!(EngineControls, speakers) as i32,
        off_controls_phones: offset_of!(EngineControls, phones) as i32,
        off_talk_paths: offset_of!(TalkParams, paths) as i32,
        off_monitor_path_peak: offset_of!(MonitorPathParams, peak) as i32,
        off_source_peak: offset_of!(SourceParams, peak) as i32,
        monitor_dsp: size_of::<MonitorDspParams>() as i32,
        monitor_controls: size_of::<MonitorControls>() as i32,
        off_controls_monitor: offset_of!(EngineControls, monitor) as i32,
        off_monitor_dsp_eq: offset_of!(MonitorDspParams, eq) as i32,
        off_monitor_dsp_pan: offset_of!(MonitorDspParams, pan) as i32,
        off_monitor_controls_source_solo: offset_of!(MonitorControls, source_solo) as i32,
        off_source_mute_groups: offset_of!(SourceParams, mute_groups) as i32,
        off_matrix_send_pan_reversed: offset_of!(MatrixSendParams, pan_reversed) as i32,
        off_dca_icon: offset_of!(DcaParams, icon) as i32,
        off_cust_link: offset_of!(StripParams, cust_link) as i32,
        off_dca_mon: offset_of!(DcaParams, mon) as i32,
        console_model_info: size_of::<session::LdConsoleModelInfo>() as i32,
    };
}

// ---- engine lifetime ----------------------------------------------------------

/// Creates an engine. `num_workers` < 0 picks a default for this machine.
#[no_mangle]
pub extern "C" fn ld_engine_create(num_workers: i32) -> *mut Engine {
    guard(std::ptr::null_mut(), || {
        let n = if num_workers < 0 { Engine::default_num_workers() } else { num_workers as usize };
        Box::into_raw(Box::new(Engine::new(n)))
    })
}

/// Destroys the engine. Worker threads must have returned from `ld_engine_run_worker`.
#[no_mangle]
pub unsafe extern "C" fn ld_engine_destroy(e: *mut Engine) {
    if !e.is_null() {
        guard((), || drop(Box::from_raw(e)));
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_engine_num_workers(e: *const Engine) -> i32 {
    engine(e).map_or(0, |e| e.pool().num_workers() as i32)
}

/// Worker thread body (call from a host realtime thread); returns after shutdown.
#[no_mangle]
pub unsafe extern "C" fn ld_engine_run_worker(e: *const Engine, index: i32) {
    if let Some(e) = engine(e) {
        guard((), || e.pool().run_worker(index.max(0) as usize));
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_engine_shutdown_workers(e: *const Engine) {
    if let Some(e) = engine(e) {
        e.pool().shutdown();
    }
}

// ---- device -------------------------------------------------------------------

/// Device-stopped only. Maps give, per hardware channel, its index in the
/// process() buffers (-1 = inactive).
#[no_mangle]
pub unsafe extern "C" fn ld_engine_prepare(
    e: *const Engine,
    sample_rate: f64,
    block_size: i32,
    in_map: *const i32,
    num_hw_inputs: i32,
    out_map: *const i32,
    num_hw_outputs: i32,
) {
    let Some(e) = engine(e) else { return };
    let ins = if in_map.is_null() { &[][..] } else { std::slice::from_raw_parts(in_map, num_hw_inputs.max(0) as usize) };
    let outs = if out_map.is_null() { &[][..] } else { std::slice::from_raw_parts(out_map, num_hw_outputs.max(0) as usize) };
    guard((), || e.prepare(sample_rate, block_size.max(1) as usize, ins, outs));
}

#[no_mangle]
pub unsafe extern "C" fn ld_engine_set_device_running(e: *const Engine, running: bool) {
    if let Some(e) = engine(e) {
        e.set_device_running(running);
    }
}

/// Audio thread. `ins`/`outs` are the active device channels.
#[no_mangle]
pub unsafe extern "C" fn ld_engine_process(
    e: *const Engine,
    ins: *const *const f32,
    num_ins: i32,
    outs: *const *mut f32,
    num_outs: i32,
    num_samples: i32,
) {
    let Some(e) = engine(e) else { return };
    let ins = if ins.is_null() { &[][..] } else { std::slice::from_raw_parts(ins, num_ins.max(0) as usize) };
    let outs = if outs.is_null() { &[][..] } else { std::slice::from_raw_parts(outs, num_outs.max(0) as usize) };
    e.process(ins, outs, num_samples.max(0) as usize); // panic-safe internally
}

#[no_mangle]
pub unsafe extern "C" fn ld_engine_sample_rate(e: *const Engine) -> f64 {
    engine(e).map_or(48000.0, |e| e.sample_rate())
}

#[no_mangle]
pub unsafe extern "C" fn ld_engine_block_size(e: *const Engine) -> i32 {
    engine(e).map_or(512, |e| e.block_size() as i32)
}

#[no_mangle]
pub unsafe extern "C" fn ld_engine_controls(e: *const Engine) -> *const EngineControls {
    engine(e).map_or(std::ptr::null(), |e| &e.controls as *const _)
}

/// Message thread, once at start-up: STARTUP MAIN MUTE mutes every main if the
/// preference is on. Returns whether it did.
#[no_mangle]
pub unsafe extern "C" fn ld_engine_apply_startup_main_mute(e: *const Engine) -> bool {
    engine(e).is_some_and(|e| guard(false, || e.apply_startup_main_mute()))
}

// ---- garbage -----------------------------------------------------------------

/// Message thread: frees removed strips/processors the audio thread can no longer see.
#[no_mangle]
pub unsafe extern "C" fn ld_engine_collect(e: *const Engine) {
    if let Some(e) = engine(e) {
        guard((), || e.collect());
    }
}

/// Frees all pending garbage now (the audio thread must be stopped).
#[no_mangle]
pub unsafe extern "C" fn ld_engine_collect_all_now(e: *const Engine) {
    if let Some(e) = engine(e) {
        guard((), || e.garbage().collect_all_now());
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_engine_pending_garbage(e: *const Engine) -> i32 {
    engine(e).map_or(0, |e| e.garbage().num_pending() as i32)
}

// ---- structure (message thread) ---------------------------------------------------

#[no_mangle]
pub unsafe extern "C" fn ld_engine_num_strips(e: *const Engine, kind: i32) -> i32 {
    let (Some(e), Some(kind)) = (engine(e), StripKind::from_i32(kind)) else { return 0 };
    e.num_strips(kind) as i32
}

#[no_mangle]
pub unsafe extern "C" fn ld_engine_strip_at(e: *const Engine, kind: i32, index: i32) -> *const Strip {
    let (Some(e), Some(kind)) = (engine(e), StripKind::from_i32(kind)) else { return std::ptr::null() };
    if index < 0 {
        return std::ptr::null();
    }
    ptr(e.strip_at(kind, index as usize))
}

#[no_mangle]
pub unsafe extern "C" fn ld_engine_add_channel(e: *const Engine, requested_id: i32) -> *const Strip {
    engine(e).map_or(std::ptr::null(), |e| guard(std::ptr::null(), || ptr(Some(e.add_channel(requested_id)))))
}

#[no_mangle]
pub unsafe extern "C" fn ld_engine_set_num_channels(e: *const Engine, count: i32) {
    if let Some(e) = engine(e) {
        guard((), || e.set_num_channels(count.max(0) as usize));
    }
}

/// Adds a bus beyond the console's 16 (null if no slot is free).
#[no_mangle]
pub unsafe extern "C" fn ld_engine_add_bus(e: *const Engine, requested_slot: i32) -> *const Strip {
    engine(e).map_or(std::ptr::null(), |e| guard(std::ptr::null(), || ptr(e.add_bus(requested_slot))))
}

#[no_mangle]
pub unsafe extern "C" fn ld_engine_bus_in_slot(e: *const Engine, slot: i32) -> *const Strip {
    engine(e).map_or(std::ptr::null(), |e| ptr(e.bus_in_slot(slot)))
}

/// Removes a channel or an added bus (the console's 16 buses stay).
#[no_mangle]
pub unsafe extern "C" fn ld_engine_remove_strip(e: *const Engine, s: *const Strip) -> bool {
    engine(e).is_some_and(|e| guard(false, || e.remove_strip(s)))
}

#[no_mangle]
pub unsafe extern "C" fn ld_engine_reset(e: *const Engine) {
    if let Some(e) = engine(e) {
        guard((), || e.reset());
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_engine_total_dsp_faults(e: *const Engine) -> i32 {
    engine(e).map_or(0, |e| e.total_dsp_faults())
}

#[no_mangle]
pub unsafe extern "C" fn ld_engine_num_faulted_fx(e: *const Engine) -> i32 {
    engine(e).map_or(0, |e| e.num_faulted_fx() as i32)
}

/// group: 0 = DCA, 1 = mute group.
#[no_mangle]
pub unsafe extern "C" fn ld_engine_group_name(e: *const Engine, group: i32, index: i32) -> *mut session::LdString {
    let name =
        engine(e).map(|e| if group == 0 { e.dca_name(index.max(0) as usize) } else { e.mute_group_name(index.max(0) as usize) });
    session::LdString::new(name.unwrap_or_default())
}

#[no_mangle]
pub unsafe extern "C" fn ld_engine_set_group_name(e: *const Engine, group: i32, index: i32, utf8: *const u8, len: usize) {
    let Some(e) = engine(e) else { return };
    let name = session::str_arg(utf8, len);
    if group == 0 {
        e.set_dca_name(index.max(0) as usize, &name);
    } else {
        e.set_mute_group_name(index.max(0) as usize, &name);
    }
}

// ---- FX slots ---------------------------------------------------------------------

/// Message thread: installs a processor in FX slot `slot` (or clears it when
/// `callbacks` is null). The engine calls `release(ctx)` on the message thread
/// once it can no longer call `process`.
#[no_mangle]
pub unsafe extern "C" fn ld_fx_set(e: *const Engine, slot: i32, callbacks: *const InsertCallbacks) {
    let Some(e) = engine(e) else { return };
    if slot < 0 || slot as usize >= e.num_fx() {
        return;
    }
    let cb = callbacks.as_ref().copied();
    guard((), || e.set_fx_plugin(slot as usize, cb));
}

/// Message thread: as `ld_fx_set` with a processor holding the same plugins as
/// the slot's current one (a plugin chain rearranged): a faulted (auto-bypassed)
/// slot stays faulted.
#[no_mangle]
pub unsafe extern "C" fn ld_fx_replace(e: *const Engine, slot: i32, callbacks: *const InsertCallbacks) {
    let Some(e) = engine(e) else { return };
    if slot < 0 || slot as usize >= e.num_fx() {
        return;
    }
    let Some(cb) = callbacks.as_ref().copied() else { return };
    guard((), || e.replace_fx_plugin(slot as usize, cb));
}

/// Message thread: selects the device of a slot (index into the FX table,
/// model table 4), creating it now; `defaults` resets its settings. Returns
/// false if the device isn't allowed there (premium devices need slots 1-8).
#[no_mangle]
pub unsafe extern "C" fn ld_fx_set_model(e: *const Engine, slot: i32, model: i32, defaults: bool) -> bool {
    let Some(e) = engine(e) else { return false };
    if slot < 0 || model < 0 {
        return false;
    }
    guard(false, || e.set_fx_model(slot as usize, model as usize, defaults))
}

/// The meter of a slot's built-in device: gain reduction in dB (<= 0).
#[no_mangle]
pub unsafe extern "C" fn ld_fx_meter(e: *const Engine, slot: i32) -> f32 {
    engine(e).and_then(|e| e.fx(slot.max(0) as usize).filter(|_| slot >= 0)).map_or(0.0, |r| r.meter())
}

/// A dynamic band's live gain of a slot's built-in device (TRIPLE DEQ bands
/// 0-2), in dB, signed: cut < 0, boost > 0. 0 for other devices.
#[no_mangle]
pub unsafe extern "C" fn ld_fx_band_gain(e: *const Engine, slot: i32, band: i32) -> f32 {
    let Ok(band) = usize::try_from(band) else { return 0.0 };
    engine(e).and_then(|e| e.fx(slot.max(0) as usize).filter(|_| slot >= 0)).map_or(0.0, |r| r.band_gain(band))
}

/// Message thread: the host context of slot `slot` (null if empty).
#[no_mangle]
pub unsafe extern "C" fn ld_fx_ctx(e: *const Engine, slot: i32) -> *mut std::ffi::c_void {
    engine(e).and_then(|e| e.fx(slot.max(0) as usize).filter(|_| slot >= 0)).map_or(std::ptr::null_mut(), |r| r.ctx())
}

/// what: 0 = a plugin is loaded, 1 = faulted, 2 = double-assigned, 3 = a
/// built-in device is running.
#[no_mangle]
pub unsafe extern "C" fn ld_fx_state(e: *const Engine, slot: i32, what: i32) -> bool {
    let Some(r) = engine(e).and_then(|e| e.fx(slot.max(0) as usize).filter(|_| slot >= 0)) else { return false };
    match what {
        0 => r.is_loaded(),
        1 => r.is_faulted(),
        2 => r.is_double_assigned(),
        _ => r.has_device(),
    }
}

/// Clears the fault (auto-bypass) and double-assignment flags of a slot.
#[no_mangle]
pub unsafe extern "C" fn ld_fx_clear_flags(e: *const Engine, slot: i32) {
    if let Some(r) = engine(e).and_then(|e| e.fx(slot.max(0) as usize).filter(|_| slot >= 0)) {
        r.clear_fault();
        r.clear_double_assigned();
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_fx_load(e: *const Engine, slot: i32) -> f32 {
    engine(e).and_then(|e| e.fx(slot.max(0) as usize).filter(|_| slot >= 0)).map_or(0.0, |r| r.slot_load())
}

/// Number of strip inserts currently pointing at slot `slot`.
#[no_mangle]
pub unsafe extern "C" fn ld_fx_num_users(e: *const Engine, slot: i32) -> i32 {
    engine(e).map_or(0, |e| if slot >= 0 { e.fx_users(slot as usize).len() as i32 } else { 0 })
}

// ---- plugin delay compensation (ABI 10) -------------------------------------------

/// Any thread: the latency (samples) the plugin in slot `slot` reports
/// (`AudioProcessor::getLatencySamples`). Reset when a plugin is installed.
#[no_mangle]
pub unsafe extern "C" fn ld_fx_set_latency(e: *const Engine, slot: i32, samples: i32) {
    if let Some(r) = engine(e).and_then(|e| e.fx(slot.max(0) as usize).filter(|_| slot >= 0)) {
        r.set_latency(samples.max(0) as u32);
    }
}

/// The latency reported for slot `slot`'s plugin (0 without one).
#[no_mangle]
pub unsafe extern "C" fn ld_fx_latency(e: *const Engine, slot: i32) -> i32 {
    engine(e).and_then(|e| e.fx(slot.max(0) as usize).filter(|_| slot >= 0)).map_or(0, |r| r.reported_latency() as i32)
}

/// DELAY COMPENSATION on / off (default on); takes effect with the next plan.
#[no_mangle]
pub unsafe extern "C" fn ld_engine_set_pdc(e: *const Engine, on: bool) {
    if let Some(e) = engine(e) {
        e.pdc().enabled.store(on, std::sync::atomic::Ordering::Relaxed);
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_engine_pdc(e: *const Engine) -> bool {
    engine(e).is_some_and(|e| e.pdc().enabled.load(std::sync::atomic::Ordering::Relaxed))
}

/// Message thread: works out the compensation plan now (`ld_engine_collect`
/// also does).
#[no_mangle]
pub unsafe extern "C" fn ld_engine_update_pdc(e: *const Engine) {
    if let Some(e) = engine(e) {
        guard((), || e.update_pdc());
    }
}

/// The plan's longest path latency (samples): what the compensation aligns to.
#[no_mangle]
pub unsafe extern "C" fn ld_engine_pdc_total(e: *const Engine) -> i32 {
    engine(e).map_or(0, |e| e.pdc().total.load(std::sync::atomic::Ordering::Relaxed) as i32)
}

/// A path needs more compensation than `kMaxPdc` samples (it stays late by the rest).
#[no_mangle]
pub unsafe extern "C" fn ld_engine_pdc_limited(e: *const Engine) -> bool {
    engine(e).is_some_and(|e| e.pdc().limited.load(std::sync::atomic::Ordering::Relaxed))
}

/// The plugin latency of a strip's output (its inserts plus what it was aligned to).
#[no_mangle]
pub unsafe extern "C" fn ld_strip_latency(s: *const Strip) -> i32 {
    strip(s).map_or(0, |s| s.pdc.latency(pdc_tap::OUT) as i32)
}

// ---- strips ---------------------------------------------------------------------

#[no_mangle]
pub unsafe extern "C" fn ld_strip_kind(s: *const Strip) -> i32 {
    strip(s).map_or(0, |s| s.kind() as i32)
}

#[no_mangle]
pub unsafe extern "C" fn ld_strip_id(s: *const Strip) -> i32 {
    strip(s).map_or(-1, |s| s.id())
}

#[no_mangle]
pub unsafe extern "C" fn ld_strip_params(s: *const Strip) -> *const StripParams {
    strip(s).map_or(std::ptr::null(), |s| &s.params as *const _)
}

#[no_mangle]
pub unsafe extern "C" fn ld_strip_meters(s: *const Strip) -> *const StripMeters {
    strip(s).map_or(std::ptr::null(), |s| &s.meters as *const _)
}

/// The live gain of band `band` (0-1) of the GATE (`gate`) or DYNAMICS slot's
/// dynamic EQ (DEQ, DEQ2), in dB, signed: cut < 0, boost > 0. 0 for other models.
#[no_mangle]
pub unsafe extern "C" fn ld_strip_dyn_band_gain(s: *const Strip, gate: bool, band: i32) -> f32 {
    let Ok(band) = usize::try_from(band) else { return 0.0 };
    strip(s).map_or(0.0, |s| s.band_meters.get(gate, band))
}

#[no_mangle]
pub unsafe extern "C" fn ld_strip_num_channels(s: *const Strip) -> i32 {
    strip(s).map_or(1, |s| s.num_channels() as i32)
}

#[no_mangle]
pub unsafe extern "C" fn ld_strip_is_idle(s: *const Strip) -> bool {
    strip(s).is_some_and(|s| s.is_idle())
}

/// Message thread: the send to bus `slot`, allocated if needed (null if out of range).
#[no_mangle]
pub unsafe extern "C" fn ld_strip_send(s: *const Strip, slot: i32) -> *const SendParams {
    let Some(s) = strip(s) else { return std::ptr::null() };
    if slot < 0 {
        return std::ptr::null();
    }
    guard(std::ptr::null(), || s.sends.get_or_create(slot as usize).map_or(std::ptr::null(), |p| p as *const _))
}

/// Any thread: the send to bus `slot` if it was ever configured, else null.
#[no_mangle]
pub unsafe extern "C" fn ld_strip_find_send(s: *const Strip, slot: i32) -> *const SendParams {
    let Some(s) = strip(s) else { return std::ptr::null() };
    if slot < 0 {
        return std::ptr::null();
    }
    s.sends.find(slot as usize).map_or(std::ptr::null(), |p| p as *const _)
}

/// Message thread: the strip's name (free with `ld_string_free`).
#[no_mangle]
pub unsafe extern "C" fn ld_strip_name(s: *const Strip) -> *mut session::LdString {
    session::LdString::new(strip(s).map(|s| s.name()).unwrap_or_default())
}

#[no_mangle]
pub unsafe extern "C" fn ld_strip_set_name(s: *const Strip, utf8: *const u8, len: usize) {
    if let Some(s) = strip(s) {
        guard((), || s.set_name(&session::str_arg(utf8, len)));
    }
}

/// Message thread: the strip's free tags, comma-separated (free with `ld_string_free`).
#[no_mangle]
pub unsafe extern "C" fn ld_strip_tags(s: *const Strip) -> *mut session::LdString {
    session::LdString::new(strip(s).map(|s| s.tags()).unwrap_or_default())
}

#[no_mangle]
pub unsafe extern "C" fn ld_strip_set_tags(s: *const Strip, utf8: *const u8, len: usize) {
    if let Some(s) = strip(s) {
        guard((), || s.set_tags(&session::str_arg(utf8, len)));
    }
}

/// Blocks with a model: 0 = gate slot, 1 = EQ, 2 = dynamics slot, 3 = filter tool.
#[no_mangle]
pub unsafe extern "C" fn ld_strip_set_model(s: *const Strip, block: i32, model: i32) {
    let Some(s) = strip(s) else { return };
    let m = model.max(0) as usize;
    guard((), || match block {
        0 => s.params.gate.set_model(m),
        1 => s.params.eq.set_model(m, s.kind().is_output()),
        2 => s.params.dynamics.set_model(m),
        _ => s.params.filter.set_tool_model(m),
    });
}

/// Writes the response (dB) of a strip's EQ (block 1), FILTER (block 3), TAP EQ
/// (block 4), GATE / DYNAMICS key filter (5 / 6) or DYNAMICS crossover (7) at
/// `count` frequencies - the same filters the engine runs.
#[no_mangle]
pub unsafe extern "C" fn ld_strip_response(
    s: *const Strip,
    block: i32,
    freqs: *const f64,
    out_db: *mut f64,
    count: i32,
    sample_rate: f64,
) {
    let Some(s) = strip(s) else { return };
    if freqs.is_null() || out_db.is_null() || count <= 0 {
        return;
    }
    let freqs = std::slice::from_raw_parts(freqs, count as usize);
    let out = std::slice::from_raw_parts_mut(out_db, count as usize);
    guard((), || models::strip_response(s, block, freqs, out, sample_rate));
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn layout_is_reported() {
        let mut info = LdLayoutInfo::default();
        unsafe { ld_layout_info(&mut info) };
        assert_eq!(info.abi_version, LD_ABI_VERSION);
        assert!(info.strip_params > 0 && info.send == 24);
    }

    #[test]
    fn engine_round_trip_through_the_c_abi() {
        unsafe {
            let e = ld_engine_create(0);
            ld_engine_set_num_channels(e, 4);
            assert_eq!(ld_engine_num_strips(e, 0), 4);
            assert_eq!(ld_engine_num_strips(e, StripKind::Bus as i32), 16);
            let bus = ld_engine_add_bus(e, -1);
            assert!(!bus.is_null());
            let ch = ld_engine_strip_at(e, 0, 0);
            ld_strip_send(ch, ld_strip_id(bus));
            assert!(!ld_strip_find_send(ch, ld_strip_id(bus)).is_null());
            assert!(ld_engine_remove_strip(e, bus));
            ld_strip_set_model(ch, 2, 3);
            assert_eq!((*ld_strip_params(ch)).dynamics.model_index(), 3);
            let freqs = [100.0, 1000.0];
            let mut out = [1.0; 2];
            ld_strip_response(ch, 1, freqs.as_ptr(), out.as_mut_ptr(), 2, 48000.0);
            assert!(out[0].abs() < 0.01 && out[1].abs() < 0.01);
            // The key filters and the crossover: flat until a filter type is chosen.
            for block in 5..=7 {
                ld_strip_response(ch, block, freqs.as_ptr(), out.as_mut_ptr(), 2, 48000.0);
                assert!(out[0].abs() < 1e-6 && out[1].abs() < 1e-6, "block {block}");
            }
            let p = &*ld_strip_params(ch);
            p.gate.sc_filter.store(1, std::sync::atomic::Ordering::Relaxed); // LOWPASS at 1 kHz
            ld_strip_response(ch, 5, freqs.as_ptr(), out.as_mut_ptr(), 2, 48000.0);
            assert!(out[0].abs() < 0.1 && (out[1] + 3.0).abs() < 0.2, "{out:?}");
            p.dynamics.xo_kind.store(2, std::sync::atomic::Ordering::Relaxed); // HIGH at 1 kHz
            ld_strip_response(ch, 7, freqs.as_ptr(), out.as_mut_ptr(), 2, 48000.0);
            assert!(out[0] < -30.0 && (out[1] + 3.0).abs() < 0.2, "{out:?}");
            ld_engine_collect(e);
            ld_engine_destroy(e);
        }
    }

    unsafe extern "C" fn pass(_: *mut std::ffi::c_void, _: *const *mut f32, _: i32, _: i32) -> i32 {
        0
    }

    #[test]
    fn delay_compensation_through_the_c_abi() {
        unsafe {
            let e = ld_engine_create(0);
            ld_engine_set_num_channels(e, 2);
            assert!(ld_engine_pdc(e));
            let cb = InsertCallbacks { process: pass, release: None, ctx: std::ptr::null_mut() };
            ld_fx_set(e, 2, &cb);
            ld_fx_set_latency(e, 2, 256);
            assert_eq!(ld_fx_latency(e, 2), 256);
            let ch = ld_engine_strip_at(e, 0, 0);
            let p = &*ld_strip_params(ch);
            p.pre_insert.on.store(true, std::sync::atomic::Ordering::Relaxed);
            p.pre_insert.fx.store(2, std::sync::atomic::Ordering::Relaxed);
            ld_engine_update_pdc(e);
            assert_eq!(ld_strip_latency(ch), 256);
            assert_eq!(ld_strip_latency(ld_engine_strip_at(e, StripKind::Main as i32, 0)), 256);
            assert_eq!(ld_engine_pdc_total(e), 256);
            assert!(!ld_engine_pdc_limited(e));
            ld_engine_set_pdc(e, false);
            ld_engine_collect(e);
            assert_eq!(ld_engine_pdc_total(e), 0);
            ld_fx_set(e, 2, std::ptr::null());
            assert_eq!(ld_fx_latency(e, 2), 0);
            ld_engine_destroy(e);
        }
    }
}
