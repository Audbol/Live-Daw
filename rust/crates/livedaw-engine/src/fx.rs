//! The 16 FX slots of the EFFECTS screen. A slot holds a processor hosted by
//! the application (a VST/AU/LV2 plugin through C callbacks - the console's
//! "EXT" model). Strips reach a slot through their PRE/POST insert points.
//!
//! A slot processes one strip per block: a claim flag makes sure that two
//! strips pointing at the same slot (a misconfiguration) never run the plugin
//! concurrently - the second one passes its signal through and the slot
//! reports itself as double-assigned.

use crate::atomic::AtomicF32;
use crate::garbage::Garbage;
use livedaw_dsp::util::all_finite;
use std::cell::UnsafeCell;
use std::ffi::c_void;
use std::sync::atomic::{AtomicBool, AtomicPtr, AtomicU64, Ordering::*};
use std::time::Instant;

/// Processes `num_channels` (1 or 2) channels of `num_samples` in place.
/// Returns 0 on success, non-zero if the processor failed (e.g. threw).
pub type ProcessFn =
    unsafe extern "C" fn(ctx: *mut c_void, channels: *const *mut f32, num_channels: i32, num_samples: i32) -> i32;
/// Called on the message thread once the engine can no longer call `process`.
pub type ReleaseFn = unsafe extern "C" fn(ctx: *mut c_void);

#[repr(C)]
#[derive(Clone, Copy)]
pub struct InsertCallbacks {
    pub process: ProcessFn,
    pub release: Option<ReleaseFn>,
    pub ctx: *mut c_void,
}

struct Slot {
    cb: InsertCallbacks,
}

// SAFETY: the host guarantees its callbacks may be called from the audio thread
// (process) and the message thread (release).
unsafe impl Send for Slot {}

impl Drop for Slot {
    fn drop(&mut self) {
        if let Some(release) = self.cb.release {
            // SAFETY: host contract; the slot is unreachable from the audio thread.
            unsafe { release(self.cb.ctx) };
        }
    }
}

/// Wrapper so a retired slot can go to the garbage collector.
struct RetiredSlot(#[allow(dead_code)] Box<Slot>);

pub struct FxRack {
    slot: AtomicPtr<Slot>,
    faulted: AtomicBool,
    load: AtomicF32,
    /// Set while a strip is running the processor.
    busy: AtomicBool,
    /// Block counter of the last two claims, to report double assignment.
    claims: AtomicU64,
    double_assigned: AtomicBool,
    /// Dry copy for auto-bypass and wet/dry mix (only the claiming task touches it).
    dry: UnsafeCell<[Vec<f32>; 2]>,
}

// SAFETY: `dry` is only used by the task that holds `busy`; everything else is atomic.
unsafe impl Sync for FxRack {}
unsafe impl Send for FxRack {}

impl Default for FxRack {
    fn default() -> Self {
        Self {
            slot: AtomicPtr::new(std::ptr::null_mut()),
            faulted: AtomicBool::new(false),
            load: AtomicF32::new(0.0),
            busy: AtomicBool::new(false),
            claims: AtomicU64::new(0),
            double_assigned: AtomicBool::new(false),
            dry: UnsafeCell::new([Vec::new(), Vec::new()]),
        }
    }
}

impl FxRack {
    /// # Safety
    /// The audio thread must not be processing (device stopped).
    pub(crate) unsafe fn prepare(&self, max_block: usize) {
        let dry = &mut *self.dry.get();
        for d in dry.iter_mut() {
            d.clear();
            d.resize(max_block, 0.0);
        }
    }

    /// Message thread: installs (or clears, with None) the slot's processor.
    pub fn set(&self, cb: Option<InsertCallbacks>, garbage: &Garbage) {
        let new = cb.map_or(std::ptr::null_mut(), |cb| Box::into_raw(Box::new(Slot { cb })));
        let old = self.slot.swap(new, AcqRel);
        self.faulted.store(false, Relaxed);
        self.load.store(0.0);
        if !old.is_null() {
            // SAFETY: created by Box::into_raw above; now unreachable for new callbacks.
            garbage.retire(RetiredSlot(unsafe { Box::from_raw(old) }));
        }
    }

    /// Message thread only: the host context of the processor (null if empty).
    pub fn ctx(&self) -> *mut c_void {
        let p = self.slot.load(Acquire);
        // SAFETY: a non-null slot stays alive until replaced (message thread) and collected.
        unsafe { p.as_ref() }.map_or(std::ptr::null_mut(), |s| s.cb.ctx)
    }

    pub fn is_loaded(&self) -> bool {
        !self.slot.load(Relaxed).is_null()
    }

    /// True if the processor was auto-bypassed for failing or producing NaN/inf.
    pub fn is_faulted(&self) -> bool {
        self.faulted.load(Relaxed)
    }

    pub fn clear_fault(&self) {
        self.faulted.store(false, Relaxed);
    }

    /// Smoothed processing time as a fraction of the block duration.
    pub fn slot_load(&self) -> f32 {
        self.load.load()
    }

    /// True if two strips tried to use this slot in the same block recently.
    pub fn is_double_assigned(&self) -> bool {
        self.double_assigned.load(Relaxed)
    }

    /// Audio thread: runs the processor over `channels` with `mix` (0..1) wet/dry.
    /// `block` identifies the audio block (for double-assignment detection).
    ///
    /// # Safety
    /// `channels` are 1 or 2 slices of equal length <= the prepared block size.
    pub(crate) unsafe fn process(&self, channels: &mut [&mut [f32]], sample_rate: f64, bypass: bool, mix: f32, block: u64) {
        let n = channels.first().map_or(0, |c| c.len());
        let ptr = self.slot.load(Acquire);
        if n == 0 || ptr.is_null() || bypass || self.faulted.load(Relaxed) {
            return;
        }
        // One strip per slot per block; a second one passes through.
        let prev = self.claims.swap(block, AcqRel);
        if prev == block {
            self.double_assigned.store(true, Relaxed);
            return;
        }
        if self.busy.compare_exchange(false, true, Acquire, Relaxed).is_err() {
            self.double_assigned.store(true, Relaxed);
            return;
        }

        let dry = &mut *self.dry.get();
        if dry[0].len() < n {
            self.busy.store(false, Release);
            return;
        }
        let cb = (*ptr).cb;
        let start = Instant::now();
        for (c, data) in channels.iter().enumerate().take(2) {
            dry[c][..n].copy_from_slice(data);
        }
        let mut ptrs: [*mut f32; 2] = [std::ptr::null_mut(); 2];
        for (c, data) in channels.iter_mut().enumerate().take(2) {
            ptrs[c] = data.as_mut_ptr();
        }
        let status = (cb.process)(cb.ctx, ptrs.as_ptr(), channels.len().min(2) as i32, n as i32);
        let ok = status == 0 && channels.iter().all(|c| all_finite(c));

        if !ok {
            // Keep the dry signal; the slot stays bypassed until the fault is cleared.
            for (c, data) in channels.iter_mut().enumerate().take(2) {
                data.copy_from_slice(&dry[c][..n]);
            }
            self.faulted.store(true, Relaxed);
        } else if mix < 0.999 {
            let m = mix.clamp(0.0, 1.0);
            for (c, data) in channels.iter_mut().enumerate().take(2) {
                for (y, x) in data.iter_mut().zip(&dry[c][..n]) {
                    *y = x + m * (*y - x);
                }
            }
        }

        let load = (start.elapsed().as_secs_f64() * sample_rate / n as f64) as f32;
        self.load.store(0.9 * self.load.load() + 0.1 * load);
        self.busy.store(false, Release);
    }

    /// Message thread: forgets a double assignment once it has been fixed.
    pub fn clear_double_assigned(&self) {
        self.double_assigned.store(false, Relaxed);
    }
}

impl Drop for FxRack {
    fn drop(&mut self) {
        let p = self.slot.swap(std::ptr::null_mut(), AcqRel);
        if !p.is_null() {
            // SAFETY: created by Box::into_raw in set(); the engine is being destroyed.
            drop(unsafe { Box::from_raw(p) });
        }
    }
}
