//! The 16 FX slots of the EFFECTS screen. A slot runs one of the console's
//! built-in devices (reverbs, delays, EQs... from `livedaw_dsp::wing::fx`) or,
//! with the EXT model, a processor hosted by the application (a VST/AU/LV2
//! plugin through C callbacks). Strips reach a slot through their PRE/POST
//! insert points.
//!
//! Devices are created on the message thread (they allocate their delay lines
//! there) and swapped in like plugins; the old one goes to the garbage list.
//!
//! A slot processes one strip per block: a claim flag makes sure that two
//! strips pointing at the same slot (a misconfiguration) never run the plugin
//! concurrently - the second one passes its signal through and the slot
//! reports itself as double-assigned.
//!
//! A plugin's reported latency (`set_latency`) feeds the plugin delay
//! compensation (`pdc.rs`); with a wet/dry MIX below 100 % the dry signal is
//! delayed by the same amount so the two stay in time.

use crate::atomic::AtomicF32;
use crate::garbage::Garbage;
use crate::params::{FxParams, FX_EXT};
use crate::pdc::{CompRing, MAX_PDC};
use livedaw_dsp::util::all_finite;
use livedaw_dsp::wing::fx::{FxContext, FxDevice, FX_BANDS};
use livedaw_dsp::wing::MAX_FX_PARAMS;
use std::cell::UnsafeCell;
use std::ffi::c_void;
use std::sync::atomic::{AtomicBool, AtomicPtr, AtomicU32, AtomicU64, Ordering::*};
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

/// Wrapper so a replaced built-in device can go to the garbage collector.
struct RetiredDevice(#[allow(dead_code)] Box<FxDevice>);

pub struct FxRack {
    slot: AtomicPtr<Slot>,
    /// The built-in device (models other than NONE and EXT).
    device: AtomicPtr<FxDevice>,
    /// The device's meter (gain reduction, dB), for the screen.
    meter: AtomicF32,
    /// The device's dynamic bands' live gains (dB, signed), for the screen.
    band_gains: [AtomicF32; FX_BANDS],
    faulted: AtomicBool,
    load: AtomicF32,
    /// Set while a strip is running the processor.
    busy: AtomicBool,
    /// Block counter of the last two claims, to report double assignment.
    claims: AtomicU64,
    double_assigned: AtomicBool,
    /// Dry copy for auto-bypass and wet/dry mix (only the claiming task touches it).
    dry: UnsafeCell<[Vec<f32>; 2]>,
    /// The plugin's latency in samples, as the host reports it.
    latency: AtomicU32,
    /// The dry signal's history, to delay it by the plugin's latency in a
    /// wet/dry mix (claiming task only; allocated in `prepare`).
    dry_ring: UnsafeCell<Option<Box<CompRing>>>,
}

// SAFETY: `dry` is only used by the task that holds `busy`; everything else is atomic.
unsafe impl Sync for FxRack {}
unsafe impl Send for FxRack {}

impl Default for FxRack {
    fn default() -> Self {
        Self {
            slot: AtomicPtr::new(std::ptr::null_mut()),
            device: AtomicPtr::new(std::ptr::null_mut()),
            meter: AtomicF32::new(0.0),
            band_gains: std::array::from_fn(|_| AtomicF32::new(0.0)),
            faulted: AtomicBool::new(false),
            load: AtomicF32::new(0.0),
            busy: AtomicBool::new(false),
            claims: AtomicU64::new(0),
            double_assigned: AtomicBool::new(false),
            dry: UnsafeCell::new([Vec::new(), Vec::new()]),
            latency: AtomicU32::new(0),
            dry_ring: UnsafeCell::new(None),
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
        *self.dry_ring.get() = Some(Box::new(CompRing::new(max_block)));
    }

    /// Message thread (or any): the plugin's latency in samples, as reported
    /// by the host (`AudioProcessor::getLatencySamples`).
    pub fn set_latency(&self, samples: u32) {
        self.latency.store(samples, Relaxed);
    }

    /// The latency the host reported for the plugin (0 without one).
    pub fn reported_latency(&self) -> u32 {
        if self.is_loaded() {
            self.latency.load(Relaxed)
        } else {
            0
        }
    }

    /// The latency the slot adds to a strip now: the plugin's while it runs
    /// (EXT, not bypassed, not auto-bypassed); built-in devices have none.
    pub fn effective_latency(&self, params: &FxParams) -> u32 {
        let running = params.model.load(Relaxed) == FX_EXT as i32 && !params.bypass.load(Relaxed) && !self.is_faulted();
        if running {
            self.reported_latency()
        } else {
            0
        }
    }

    /// Message thread: installs (or clears, with None) the slot's processor.
    pub fn set(&self, cb: Option<InsertCallbacks>, garbage: &Garbage) {
        self.swap(cb, false, garbage);
    }

    /// Message thread: swaps in a processor holding the same plugins as the
    /// current one (a plugin chain rearranged): as `set`, but an auto-bypassed
    /// slot stays so until its fault is cleared.
    pub fn replace(&self, cb: InsertCallbacks, garbage: &Garbage) {
        self.swap(Some(cb), true, garbage);
    }

    fn swap(&self, cb: Option<InsertCallbacks>, keep_fault: bool, garbage: &Garbage) {
        let new = cb.map_or(std::ptr::null_mut(), |cb| Box::into_raw(Box::new(Slot { cb })));
        let old = self.slot.swap(new, AcqRel);
        if !keep_fault {
            self.faulted.store(false, Relaxed);
        }
        self.load.store(0.0);
        self.latency.store(0, Relaxed); // the host reports the new plugin's
        if !old.is_null() {
            // SAFETY: created by Box::into_raw above; now unreachable for new callbacks.
            garbage.retire(RetiredSlot(unsafe { Box::from_raw(old) }));
        }
    }

    /// Message thread: installs (or removes, with None) the built-in device.
    pub fn set_device(&self, device: Option<Box<FxDevice>>, garbage: &Garbage) {
        let new = device.map_or(std::ptr::null_mut(), Box::into_raw);
        let old = self.device.swap(new, AcqRel);
        self.faulted.store(false, Relaxed);
        self.meter.store(0.0);
        self.band_gains.iter().for_each(|g| g.store(0.0));
        if !old.is_null() {
            // SAFETY: created by Box::into_raw above; now unreachable for new blocks.
            garbage.retire(RetiredDevice(unsafe { Box::from_raw(old) }));
        }
    }

    /// Message thread only: the model and context of the installed device.
    pub fn device_info(&self) -> Option<(usize, FxContext)> {
        let p = self.device.load(Acquire);
        // SAFETY: a non-null device stays alive until replaced (message thread) and collected.
        unsafe { p.as_ref() }.map(|d| (d.model(), d.context()))
    }

    /// The device's meter: gain reduction in dB (<= 0).
    pub fn meter(&self) -> f32 {
        self.meter.load()
    }

    /// A dynamic band's live gain in dB, signed (TRIPLE DEQ; 0 otherwise).
    pub fn band_gain(&self, band: usize) -> f32 {
        self.band_gains.get(band).map_or(0.0, AtomicF32::load)
    }

    /// Message thread only: the host context of the processor (null if empty).
    pub fn ctx(&self) -> *mut c_void {
        let p = self.slot.load(Acquire);
        // SAFETY: a non-null slot stays alive until replaced (message thread) and collected.
        unsafe { p.as_ref() }.map_or(std::ptr::null_mut(), |s| s.cb.ctx)
    }

    /// A plugin is installed.
    pub fn is_loaded(&self) -> bool {
        !self.slot.load(Relaxed).is_null()
    }

    /// A built-in device is installed.
    pub fn has_device(&self) -> bool {
        !self.device.load(Relaxed).is_null()
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

    /// Audio thread: runs the slot's device (or, for EXT, its plugin) over
    /// `channels` with the slot's settings. `block` identifies the audio block
    /// (for double-assignment detection).
    ///
    /// # Safety
    /// `channels` are 1 or 2 slices of equal length <= the prepared block size.
    pub(crate) unsafe fn process(&self, channels: &mut [&mut [f32]], sample_rate: f64, params: &FxParams, block: u64) {
        let n = channels.first().map_or(0, |c| c.len());
        let model = params.model.load(Relaxed);
        let plugin = self.slot.load(Acquire);
        let device = self.device.load(Acquire);
        let runnable = if model == FX_EXT as i32 {
            !plugin.is_null()
        } else {
            // A device for another model is still in place until the message
            // thread swaps the new one in: pass through meanwhile.
            !device.is_null() && (*device).model() as i32 == model
        };
        if n == 0 || !runnable || params.bypass.load(Relaxed) || self.faulted.load(Relaxed) {
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
        let start = Instant::now();
        for (c, data) in channels.iter().enumerate().take(2) {
            dry[c][..n].copy_from_slice(data);
        }

        let ok = if model == FX_EXT as i32 {
            let cb = (*plugin).cb;
            let mut ptrs: [*mut f32; 2] = [std::ptr::null_mut(); 2];
            for (c, data) in channels.iter_mut().enumerate().take(2) {
                ptrs[c] = data.as_mut_ptr();
            }
            let status = (cb.process)(cb.ctx, ptrs.as_ptr(), channels.len().min(2) as i32, n as i32);
            status == 0 && channels.iter().all(|c| all_finite(c))
        } else {
            let dev = &mut *device;
            let mut values = [0.0f32; MAX_FX_PARAMS];
            params.values_snapshot(&mut values);
            if channels.len() >= 2 {
                let (l, r) = channels.split_at_mut(1);
                dev.process(&values, &mut l[0][..n], &mut r[0][..n]);
            } else {
                // Mono strip: feed both inputs, take the sum of the outputs back
                // (dry[1] is free as the right channel's buffer).
                let r = &mut dry[1][..n];
                r.copy_from_slice(&channels[0][..n]);
                dev.process(&values, &mut channels[0][..n], r);
                for (y, r) in channels[0].iter_mut().zip(r.iter()) {
                    *y = 0.5 * (*y + r);
                }
            }
            self.meter.store(dev.meter());
            for (i, g) in self.band_gains.iter().enumerate() {
                g.store(dev.band_gain(i));
            }
            channels.iter().all(|c| all_finite(c))
        };

        let mix = params.mix.load() / 100.0;
        // The dry history follows every block, so a MIX change finds it in time.
        let latency = if model == FX_EXT as i32 { self.latency.load(Relaxed) as usize } else { 0 };
        let dry_ring = (*self.dry_ring.get()).as_deref();
        if let Some(ring) = dry_ring.filter(|_| latency > 0) {
            ring.write(&dry[0][..n], &dry[if channels.len() > 1 { 1 } else { 0 }][..n]);
        }
        if !ok {
            // Keep the dry signal; the slot stays bypassed until the fault is cleared.
            for (c, data) in channels.iter_mut().enumerate().take(2) {
                data.copy_from_slice(&dry[c][..n]);
            }
            self.faulted.store(true, Relaxed);
        } else if mix < 0.999 {
            let m = mix.clamp(0.0, 1.0);
            for (c, data) in channels.iter_mut().enumerate().take(2) {
                let dry = match dry_ring.filter(|_| latency > 0) {
                    Some(ring) => ring.read(c, latency.min(MAX_PDC), n),
                    None => &dry[c][..n],
                };
                for (y, x) in data.iter_mut().zip(dry) {
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
        let d = self.device.swap(std::ptr::null_mut(), AcqRel);
        if !d.is_null() {
            // SAFETY: created by Box::into_raw in set_device(); the engine is being destroyed.
            drop(unsafe { Box::from_raw(d) });
        }
    }
}
