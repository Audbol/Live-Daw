//! C ABI of the RTA, the LR vector scope and LISTEN (`ld_analysis_*`,
//! `ld_rta_*`, `ld_scope_*`). The controls are a shared block of atomics like
//! `EngineControls`; bands and scope samples are copied out on request.

use crate::{engine, guard};
use livedaw_engine::analyser::{rta_band_hz, RTA_BANDS};
use livedaw_engine::{AnalysisControls, Engine};
use std::sync::atomic::Ordering::Relaxed;

/// The analysis controls (RTA settings, analysed strip, scope, LISTEN).
#[no_mangle]
pub unsafe extern "C" fn ld_engine_analysis(e: *const Engine) -> *const AnalysisControls {
    engine(e).map_or(std::ptr::null(), |e| e.analysis().controls() as *const _)
}

/// Number of RTA bands (one per semitone, E0 .. D#10).
#[no_mangle]
pub extern "C" fn ld_rta_num_bands() -> i32 {
    RTA_BANDS as i32
}

/// Centre frequency of RTA band `band` in Hz (0 if out of range).
#[no_mangle]
pub extern "C" fn ld_rta_band_hz(band: i32) -> f32 {
    usize::try_from(band).ok().filter(|&b| b < RTA_BANDS).map_or(0.0, rta_band_hz)
}

/// Message thread: copies the RTA bands (dB) and their peak holds (either
/// pointer may be null) and returns how many bands were written. Starts the
/// analysis thread while an RTA is on (`AnalysisControls::rtaOn`). `frame`
/// (may be null) gets a counter that moves whenever new bands are published.
#[no_mangle]
pub unsafe extern "C" fn ld_rta_read(e: *const Engine, levels: *mut f32, peaks: *mut f32, count: i32, frame: *mut u32) -> i32 {
    let Some(e) = engine(e) else { return 0 };
    let n = count.clamp(0, RTA_BANDS as i32) as usize;
    guard(0, || {
        let a = e.analysis();
        if a.controls().rta_on.load(Relaxed) {
            a.ensure_thread();
        }
        let mut l = [0.0f32; RTA_BANDS];
        let mut p = [0.0f32; RTA_BANDS];
        let (_, published) = a.read_rta(&mut l, &mut p);
        if !levels.is_null() {
            std::slice::from_raw_parts_mut(levels, n).copy_from_slice(&l[..n]);
        }
        if !peaks.is_null() {
            std::slice::from_raw_parts_mut(peaks, n).copy_from_slice(&p[..n]);
        }
        if let Some(f) = frame.as_mut() {
            *f = published;
        }
        n as i32
    })
}

/// Any thread but the audio thread: copies the newest L/R pairs of the LR
/// vector scope (interleaved, up to `max_pairs`) and returns how many.
#[no_mangle]
pub unsafe extern "C" fn ld_scope_read(e: *const Engine, lr: *mut f32, max_pairs: i32) -> i32 {
    let Some(e) = engine(e) else { return 0 };
    if lr.is_null() || max_pairs <= 0 {
        return 0;
    }
    let out = std::slice::from_raw_parts_mut(lr, max_pairs as usize * 2);
    guard(0, || e.analysis().read_scope(out) as i32)
}

/// L/R pairs per second the scope keeps.
#[no_mangle]
pub unsafe extern "C" fn ld_scope_rate(e: *const Engine) -> f32 {
    engine(e).map_or(24000.0, |e| e.analysis().scope_rate())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{ld_engine_create, ld_engine_destroy};

    #[test]
    fn analysis_through_the_c_abi() {
        unsafe {
            let e = ld_engine_create(0);
            let c = &*ld_engine_analysis(e);
            assert_eq!(c.strip_kind.load(Relaxed), -1);
            assert_eq!(ld_rta_num_bands(), 120);
            assert!((ld_rta_band_hz(0) - 20.6).abs() < 0.01 && ld_rta_band_hz(120) == 0.0);
            let mut levels = [0.0f32; 120];
            let mut frame = 0u32;
            assert_eq!(ld_rta_read(e, levels.as_mut_ptr(), std::ptr::null_mut(), 120, &mut frame), 120);
            assert!(levels.iter().all(|&l| l <= -100.0));
            let mut lr = [0.0f32; 64];
            assert_eq!(ld_scope_read(e, lr.as_mut_ptr(), 32), 0);
            c.rta_on.store(true, Relaxed);
            ld_rta_read(e, levels.as_mut_ptr(), std::ptr::null_mut(), 120, &mut frame); // starts the thread
            ld_engine_destroy(e); // stops it
        }
    }
}
