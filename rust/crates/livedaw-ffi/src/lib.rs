//! C ABI for the Live DAW engine. The C++ mirror is `include/livedaw_engine.h`.
//!
//! Conventions:
//! * every function is panic-safe: a panic is caught and a neutral value returned;
//! * strip pointers stay valid until the strip is removed (or the engine reset)
//!   *and* `ld_engine_collect` has run - the host drops its references first;
//! * functions marked "message thread" must not be called from the audio thread.

#![allow(clippy::missing_safety_doc)]

use livedaw_dsp::{BandType, EqBand, EqSettings, ParametricEq};
use livedaw_engine::params::{CompParams, EqParams, GateParams};
use livedaw_engine::{Engine, EngineControls, InsertCallbacks, SendParams, Strip, StripKind, StripMeters, StripParams};
use std::mem::{offset_of, size_of};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::Arc;

pub const LD_ABI_VERSION: i32 = 1;

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

/// Sizes and offsets of the shared structs, checked by the C++ side at startup.
#[repr(C)]
#[derive(Default)]
pub struct LdLayoutInfo {
    pub abi_version: i32,
    pub strip_params_size: i32,
    pub gate_offset: i32,
    pub eq_offset: i32,
    pub comp_offset: i32,
    pub fader_offset: i32,
    pub stereo_offset: i32,
    pub output_right_offset: i32,
    pub recall_safe_offset: i32,
    pub gate_size: i32,
    pub eq_size: i32,
    pub comp_size: i32,
    pub meters_size: i32,
    pub meters_dsp_faults_offset: i32,
    pub send_size: i32,
    pub send_pre_fader_offset: i32,
    pub controls_size: i32,
    pub controls_monitor_peak_offset: i32,
    pub controls_engine_faults_offset: i32,
}

#[no_mangle]
pub unsafe extern "C" fn ld_layout_info(out: *mut LdLayoutInfo) {
    if let Some(out) = out.as_mut() {
        *out = LdLayoutInfo {
            abi_version: LD_ABI_VERSION,
            strip_params_size: size_of::<StripParams>() as i32,
            gate_offset: offset_of!(StripParams, gate) as i32,
            eq_offset: offset_of!(StripParams, eq) as i32,
            comp_offset: offset_of!(StripParams, comp) as i32,
            fader_offset: offset_of!(StripParams, fader_db) as i32,
            stereo_offset: offset_of!(StripParams, stereo) as i32,
            output_right_offset: offset_of!(StripParams, output_right) as i32,
            recall_safe_offset: offset_of!(StripParams, recall_safe) as i32,
            gate_size: size_of::<GateParams>() as i32,
            eq_size: size_of::<EqParams>() as i32,
            comp_size: size_of::<CompParams>() as i32,
            meters_size: size_of::<StripMeters>() as i32,
            meters_dsp_faults_offset: offset_of!(StripMeters, dsp_faults) as i32,
            send_size: size_of::<SendParams>() as i32,
            send_pre_fader_offset: offset_of!(SendParams, pre_fader) as i32,
            controls_size: size_of::<EngineControls>() as i32,
            controls_monitor_peak_offset: offset_of!(EngineControls, monitor_peak) as i32,
            controls_engine_faults_offset: offset_of!(EngineControls, engine_faults) as i32,
        };
    }
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

// ---- garbage -----------------------------------------------------------------

/// Message thread: frees removed strips/plugins the audio thread can no longer see.
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
pub unsafe extern "C" fn ld_engine_add_input(e: *const Engine, requested_id: i32) -> *const Strip {
    engine(e).map_or(std::ptr::null(), |e| guard(std::ptr::null(), || ptr(Some(e.add_input(requested_id)))))
}

#[no_mangle]
pub unsafe extern "C" fn ld_engine_set_num_inputs(e: *const Engine, count: i32) {
    if let Some(e) = engine(e) {
        guard((), || e.set_num_inputs(count.max(0) as usize));
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_engine_num_inputs(e: *const Engine) -> i32 {
    engine(e).map_or(0, |e| e.num_inputs() as i32)
}

#[no_mangle]
pub unsafe extern "C" fn ld_engine_input_at(e: *const Engine, index: i32) -> *const Strip {
    engine(e).map_or(std::ptr::null(), |e| ptr(e.input_at(index.max(0) as usize)))
}

/// kind: 1 = aux, 2 = group. Returns null if no slot is free.
#[no_mangle]
pub unsafe extern "C" fn ld_engine_add_bus(e: *const Engine, kind: i32, requested_slot: i32) -> *const Strip {
    let (Some(e), Some(kind)) = (engine(e), StripKind::from_i32(kind)) else { return std::ptr::null() };
    guard(std::ptr::null(), || ptr(e.add_bus(kind, requested_slot)))
}

#[no_mangle]
pub unsafe extern "C" fn ld_engine_num_buses(e: *const Engine) -> i32 {
    engine(e).map_or(0, |e| e.num_buses() as i32)
}

#[no_mangle]
pub unsafe extern "C" fn ld_engine_bus_at(e: *const Engine, index: i32) -> *const Strip {
    engine(e).map_or(std::ptr::null(), |e| ptr(e.bus_at(index.max(0) as usize)))
}

#[no_mangle]
pub unsafe extern "C" fn ld_engine_bus_in_slot(e: *const Engine, slot: i32) -> *const Strip {
    engine(e).map_or(std::ptr::null(), |e| ptr(e.bus_in_slot(slot)))
}

#[no_mangle]
pub unsafe extern "C" fn ld_engine_master(e: *const Engine) -> *const Strip {
    engine(e).map_or(std::ptr::null(), |e| ptr(Some(e.master())))
}

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
pub unsafe extern "C" fn ld_engine_num_faulted_plugins(e: *const Engine) -> i32 {
    engine(e).map_or(0, |e| e.num_faulted_plugins() as i32)
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

// ---- inserts ----------------------------------------------------------------------

/// Message thread: installs a plugin (or clears the slot when `callbacks` is null).
/// The engine calls `release(ctx)` on the message thread once it can no longer
/// call `process` - including when the strip itself is freed.
#[no_mangle]
pub unsafe extern "C" fn ld_strip_set_insert(e: *const Engine, s: *const Strip, slot: i32, callbacks: *const InsertCallbacks) {
    let (Some(e), Some(s)) = (engine(e), strip(s)) else { return };
    if slot < 0 {
        return;
    }
    let cb = callbacks.as_ref().copied();
    guard((), || s.inserts.set(slot as usize, cb, e.garbage()));
}

#[no_mangle]
pub unsafe extern "C" fn ld_strip_insert_is_bypassed(s: *const Strip, slot: i32) -> bool {
    strip(s).is_some_and(|s| slot >= 0 && s.inserts.is_bypassed(slot as usize))
}

#[no_mangle]
pub unsafe extern "C" fn ld_strip_insert_set_bypassed(s: *const Strip, slot: i32, bypassed: bool) {
    if let (Some(s), true) = (strip(s), slot >= 0) {
        s.inserts.set_bypassed(slot as usize, bypassed);
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_strip_insert_is_faulted(s: *const Strip, slot: i32) -> bool {
    strip(s).is_some_and(|s| slot >= 0 && s.inserts.is_faulted(slot as usize))
}

#[no_mangle]
pub unsafe extern "C" fn ld_strip_insert_clear_fault(s: *const Strip, slot: i32) {
    if let (Some(s), true) = (strip(s), slot >= 0) {
        s.inserts.clear_fault(slot as usize);
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_strip_insert_load(s: *const Strip, slot: i32) -> f32 {
    strip(s).map_or(0.0, |s| if slot >= 0 { s.inserts.slot_load(slot as usize) } else { 0.0 })
}

// ---- EQ curve (for the UI) ---------------------------------------------------------

#[repr(C)]
#[derive(Clone, Copy)]
pub struct LdEqBand {
    pub band_type: i32,
    pub frequency: f32,
    pub gain_db: f32,
    pub q: f32,
    pub enabled: i32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct LdEqSettings {
    pub enabled: i32,
    pub bands: [LdEqBand; 4],
}

/// Combined magnitude response in dB of an EQ at `freq` - the same maths the engine runs.
#[no_mangle]
pub unsafe extern "C" fn ld_eq_response_db(settings: *const LdEqSettings, freq: f64, sample_rate: f64) -> f64 {
    let Some(s) = settings.as_ref() else { return 0.0 };
    let band = |b: &LdEqBand| EqBand {
        band_type: BandType::from_i32(b.band_type),
        frequency: b.frequency,
        gain_db: b.gain_db,
        q: b.q,
        enabled: b.enabled != 0,
    };
    let eq = EqSettings {
        enabled: s.enabled != 0,
        bands: [band(&s.bands[0]), band(&s.bands[1]), band(&s.bands[2]), band(&s.bands[3])],
    };
    ParametricEq::response_db(&eq, freq, sample_rate)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn layout_is_reported() {
        let mut info = LdLayoutInfo::default();
        unsafe { ld_layout_info(&mut info) };
        assert_eq!(info.abi_version, LD_ABI_VERSION);
        assert!(info.strip_params_size > 0 && info.send_size == 16);
    }

    #[test]
    fn engine_round_trip_through_the_c_abi() {
        unsafe {
            let e = ld_engine_create(0);
            ld_engine_set_num_inputs(e, 4);
            assert_eq!(ld_engine_num_inputs(e), 4);
            let bus = ld_engine_add_bus(e, StripKind::Aux as i32, -1);
            assert!(!bus.is_null());
            let ch = ld_engine_input_at(e, 0);
            assert!(!ld_strip_find_send(ch, ld_strip_id(bus)).is_null());
            assert!(ld_engine_remove_strip(e, bus));
            ld_engine_collect(e);
            ld_engine_destroy(e);
        }
    }
}
