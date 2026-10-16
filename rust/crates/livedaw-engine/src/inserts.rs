//! Plugin insert slots. The plugins themselves live in the host (C++/JUCE);
//! the engine calls them through callbacks and guards their output.

use crate::atomic::AtomicF32;
use crate::garbage::Garbage;
use livedaw_dsp::util::all_finite;
use std::cell::UnsafeCell;
use std::ffi::c_void;
use std::sync::atomic::{AtomicBool, AtomicPtr, Ordering::*};
use std::time::Instant;

pub const NUM_INSERT_SLOTS: usize = 8;

/// Processes `num_channels` (1 or 2) channels of `num_samples` in place.
/// Returns 0 on success, non-zero if the plugin failed (e.g. threw).
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

pub struct InsertChain {
    slots: [AtomicPtr<Slot>; NUM_INSERT_SLOTS],
    bypass: [AtomicBool; NUM_INSERT_SLOTS],
    faulted: [AtomicBool; NUM_INSERT_SLOTS],
    load: [AtomicF32; NUM_INSERT_SLOTS],
    /// Dry copy of the signal so a failing plugin can be bypassed seamlessly.
    dry: UnsafeCell<[Vec<f32>; 2]>,
}

// SAFETY: `dry` is only used by the single audio task processing this strip;
// everything else is atomic.
unsafe impl Sync for InsertChain {}
unsafe impl Send for InsertChain {}

impl Default for InsertChain {
    fn default() -> Self {
        Self {
            slots: Default::default(),
            bypass: Default::default(),
            faulted: Default::default(),
            load: Default::default(),
            dry: UnsafeCell::new([Vec::new(), Vec::new()]),
        }
    }
}

impl InsertChain {
    /// # Safety
    /// The audio thread must not be processing this chain (device stopped, or the
    /// strip not yet published).
    pub(crate) unsafe fn prepare(&self, max_block: usize) {
        let dry = &mut *self.dry.get();
        for d in dry.iter_mut() {
            d.clear();
            d.resize(max_block, 0.0);
        }
    }

    /// Message thread: installs (or clears, with None) a plugin in `slot`.
    pub fn set(&self, slot: usize, cb: Option<InsertCallbacks>, garbage: &Garbage) {
        if slot >= NUM_INSERT_SLOTS {
            return;
        }
        let new = cb.map_or(std::ptr::null_mut(), |cb| Box::into_raw(Box::new(Slot { cb })));
        let old = self.slots[slot].swap(new, AcqRel);
        self.bypass[slot].store(false, Relaxed);
        self.faulted[slot].store(false, Relaxed);
        self.load[slot].store(0.0);
        if !old.is_null() {
            // SAFETY: created by Box::into_raw above; now unreachable for new callbacks.
            garbage.retire(RetiredSlot(unsafe { Box::from_raw(old) }));
        }
    }

    pub fn is_loaded(&self, slot: usize) -> bool {
        slot < NUM_INSERT_SLOTS && !self.slots[slot].load(Relaxed).is_null()
    }

    pub fn is_empty(&self) -> bool {
        self.slots.iter().all(|s| s.load(Relaxed).is_null())
    }

    pub fn set_bypassed(&self, slot: usize, bypassed: bool) {
        if slot < NUM_INSERT_SLOTS {
            self.bypass[slot].store(bypassed, Relaxed);
        }
    }

    pub fn is_bypassed(&self, slot: usize) -> bool {
        slot < NUM_INSERT_SLOTS && self.bypass[slot].load(Relaxed)
    }

    /// True if the plugin was auto-bypassed for producing NaN/inf or failing.
    pub fn is_faulted(&self, slot: usize) -> bool {
        slot < NUM_INSERT_SLOTS && self.faulted[slot].load(Relaxed)
    }

    pub fn clear_fault(&self, slot: usize) {
        if slot < NUM_INSERT_SLOTS {
            self.faulted[slot].store(false, Relaxed);
        }
    }

    /// Smoothed processing time as a fraction of the block duration.
    pub fn slot_load(&self, slot: usize) -> f32 {
        if slot < NUM_INSERT_SLOTS {
            self.load[slot].load()
        } else {
            0.0
        }
    }

    pub fn num_faulted(&self) -> usize {
        self.faulted.iter().filter(|f| f.load(Relaxed)).count()
    }

    /// Audio thread: runs the active plugins over `channels` (1 or 2, same length).
    ///
    /// # Safety
    /// Only one thread may process a given chain at a time.
    pub(crate) unsafe fn process(&self, channels: &mut [&mut [f32]], sample_rate: f64) {
        let n = channels.first().map_or(0, |c| c.len());
        if n == 0 {
            return;
        }
        let dry = &mut *self.dry.get();

        for slot in 0..NUM_INSERT_SLOTS {
            let ptr = self.slots[slot].load(Acquire);
            if ptr.is_null() || self.bypass[slot].load(Relaxed) {
                continue;
            }
            let cb = (*ptr).cb;
            let start = Instant::now();

            for (c, data) in channels.iter().enumerate() {
                dry[c][..n].copy_from_slice(data);
            }

            let mut ptrs: [*mut f32; 2] = [std::ptr::null_mut(); 2];
            for (c, data) in channels.iter_mut().enumerate().take(2) {
                ptrs[c] = data.as_mut_ptr();
            }

            let status = (cb.process)(cb.ctx, ptrs.as_ptr(), channels.len() as i32, n as i32);
            let ok = status == 0 && channels.iter().all(|c| all_finite(c));

            if !ok {
                // Keep the plugin's input: the slot is bypassed from now on.
                for (c, data) in channels.iter_mut().enumerate() {
                    data.copy_from_slice(&dry[c][..n]);
                }
                self.bypass[slot].store(true, Relaxed);
                self.faulted[slot].store(true, Relaxed);
            }

            let load = (start.elapsed().as_secs_f64() * sample_rate / n as f64) as f32;
            self.load[slot].store(0.9 * self.load[slot].load() + 0.1 * load);
        }
    }
}

impl Drop for InsertChain {
    fn drop(&mut self) {
        for s in self.slots.iter() {
            let p = s.swap(std::ptr::null_mut(), AcqRel);
            if !p.is_null() {
                // SAFETY: created by Box::into_raw in set(); the strip is being
                // destroyed, so the audio thread can no longer reach it.
                drop(unsafe { Box::from_raw(p) });
            }
        }
    }
}
