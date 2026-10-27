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
//!
//! The EXT device with SOURCE OUTBOARD (and no plugin) is the WING's EXTERNAL
//! effect (manual 11.11 p. 164, Fig. 169): an outboard insert. The slot sends
//! its input, at TRIM, in FX MODE (MONO, STEREO or MID/SIDE) to its FX SEND
//! (the hardware outputs ROUTING > OUTPUTS patches to it, `FxParams::send_left`
//! / `send_right`, written by the engine's output stage), and returns the
//! RETURN inputs (a source and, STEREO or MID/SIDE, its neighbour) in its
//! place. Without a RETURN it passes its signal through. The send leaves in
//! this block and comes back at the earliest in the next one, so the slot's
//! latency is the round trip Sidecar knows (one block plus the interface's
//! reported input and output latency, `set_round_trip`) plus LATENCY (the
//! outboard gear's own, in samples: the tree's `lat` is a unitless 0-800 where
//! the console's millisecond parameters say "ms", and the WING counts insert
//! latency in samples, manual 6.6 table 6.2; the manual's p. 164 says ms); the
//! delay compensation and the FX MIX's dry delay use it as they use a plugin's.
//! A built-in device with a latency of its own (the PRECISION LIMITER's
//! look-ahead) is compensated the same way.
//!
//! Meters: every slot measures its input and output (`level`: the signal the
//! device or plugin gets and gives back before FX MIX; for the outboard
//! insert, its FX SEND after TRIM and its RETURN), and copies its device's
//! readouts (`FxAlgo::readouts`: gain reductions, band levels, a scope...)
//! after each block, for the device's screen. Levels are linear peaks that
//! fall at the meters' 60 dB/s, so a screen that polls them sees what a
//! meter fed every sample would show. A slot that doesn't run (bypassed,
//! auto-bypassed, no device yet) meters its signal as passing through and
//! reports idle readouts.

use crate::atomic::AtomicF32;
use crate::garbage::Garbage;
use crate::params::{ext, FxParams, FX_EXT};
use crate::pdc::{CompRing, MAX_PDC};
use crate::strip::source_ptr;
use livedaw_dsp::util::{all_finite, db_to_gain};
use livedaw_dsp::wing::fx::meters::FALL_DB_PER_S;
use livedaw_dsp::wing::fx::{FxContext, FxDevice, FX_BANDS, FX_READOUTS};
use livedaw_dsp::wing::MAX_FX_PARAMS;
use livedaw_recorder::{PLAYBACK_SOURCE_BASE, USB_CHANNELS, USB_PLAYER_SOURCE_BASE};
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

/// The EXT device's RETURN groups (`egrp`), as the console lists them.
pub const EXT_RETURN_GROUPS: [&str; 12] = ["OFF", "LCL", "AUX", "A", "B", "C", "SC", "USB", "CRD", "MOD", "PLAY", "AES"];

/// Inputs per AES50 block on the WING's patch (A 1-48 = ports 1-48, B = 49-96, C = 97-144).
const WING_BLOCK: i32 = 48;

/// The input (a hardware port, playback channel or USB PLAYER channel) of the
/// outboard RETURN `group` / `n` (1-based), None for OFF or an input Sidecar
/// doesn't have. As the remote's patch (`livedaw_wing::map::port`): LCL n is A
/// n, the WING-LIVE card (CRD) the recorder's playback channels, the other
/// groups the port of the same number.
pub fn ext_return_input(group: &str, n: i32) -> Option<i32> {
    let n = n.max(1);
    match group {
        "A" | "LCL" | "AUX" | "SC" | "USB" | "MOD" | "AES" => Some(n - 1),
        "B" => Some(WING_BLOCK + n - 1),
        "C" => Some(2 * WING_BLOCK + n - 1),
        "CRD" => (n <= 64).then_some(PLAYBACK_SOURCE_BASE + n - 1),
        "PLAY" => (n <= USB_CHANNELS as i32).then_some(USB_PLAYER_SOURCE_BASE + n - 1),
        _ => None,
    }
}

/// The RETURN of an EXT slot's parameters (`ext::GROUP`, `ext::INPUT`).
pub fn ext_return_of(params: &FxParams) -> Option<i32> {
    let g = params.values[ext::GROUP].load().round().max(0.0) as usize;
    ext_return_input(EXT_RETURN_GROUPS.get(g)?, params.values[ext::INPUT].load().round() as i32)
}

/// The inputs an outboard RETURN reads: the processed sources of this block.
pub(crate) struct FxInputs<'a> {
    pub hw: &'a [*const f32],
    pub playback: &'a [*const f32],
}

/// The slot's levels (`FxRack::level`): input left/right, output left/right.
pub const FX_LEVELS: usize = 4;

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
    /// Input L/R and output L/R levels (linear, falling peaks).
    levels: [AtomicF32; FX_LEVELS],
    /// The device's readouts (`FxAlgo::readouts`), for the screen.
    readouts: [AtomicF32; FX_READOUTS],
    /// Set while `readouts` hold something other than the idle values.
    readouts_live: AtomicBool,
    /// The installed device's latency in samples (fixed per instance).
    device_latency: AtomicU32,
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
    /// The outboard insert's FX SEND (claiming task; read by the output stage
    /// once every strip ran) and the block it was written in.
    send: UnsafeCell<[Vec<f32>; 2]>,
    send_block: AtomicU64,
    /// The round trip through the interface in samples (one block plus its
    /// reported input and output latency).
    round_trip: AtomicU32,
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
            levels: std::array::from_fn(|_| AtomicF32::new(0.0)),
            readouts: std::array::from_fn(|_| AtomicF32::new(0.0)),
            readouts_live: AtomicBool::new(false),
            device_latency: AtomicU32::new(0),
            faulted: AtomicBool::new(false),
            load: AtomicF32::new(0.0),
            busy: AtomicBool::new(false),
            claims: AtomicU64::new(0),
            double_assigned: AtomicBool::new(false),
            dry: UnsafeCell::new([Vec::new(), Vec::new()]),
            latency: AtomicU32::new(0),
            dry_ring: UnsafeCell::new(None),
            send: UnsafeCell::new([Vec::new(), Vec::new()]),
            send_block: AtomicU64::new(0),
            round_trip: AtomicU32::new(0),
        }
    }
}

impl FxRack {
    /// # Safety
    /// The audio thread must not be processing (device stopped).
    pub(crate) unsafe fn prepare(&self, max_block: usize) {
        let dry = &mut *self.dry.get();
        for d in dry.iter_mut().chain((*self.send.get()).iter_mut()) {
            d.clear();
            d.resize(max_block, 0.0);
        }
        self.send_block.store(0, Relaxed);
        *self.dry_ring.get() = Some(Box::new(CompRing::new(max_block)));
    }

    /// The round trip of an outboard insert through the interface, in samples
    /// (the engine sets it: one block plus the reported I/O latency).
    pub fn set_round_trip(&self, samples: u32) {
        self.round_trip.store(samples, Relaxed);
    }

    /// The outboard insert's latency in samples: the round trip plus LATENCY.
    pub fn outboard_latency(&self, params: &FxParams) -> u32 {
        let extra = params.values[ext::LATENCY].load().clamp(0.0, 800.0).round() as u32;
        self.round_trip.load(Relaxed) + extra
    }

    /// The slot is an outboard insert (EXT, SOURCE OUTBOARD, no plugin).
    pub fn is_outboard(&self, params: &FxParams) -> bool {
        params.ext_outboard() && !self.is_loaded()
    }

    /// An outboard insert with a RETURN (it replaces the signal).
    pub fn outboard_returns(&self, params: &FxParams) -> bool {
        self.is_outboard(params) && ext_return_of(params).is_some()
    }

    /// The slot's latency as the screens show it: its plugin's, the
    /// outboard insert's, or its built-in device's (0 otherwise).
    pub fn latency(&self, params: &FxParams) -> u32 {
        let model = params.model.load(Relaxed);
        if model != FX_EXT as i32 {
            self.device_info().filter(|(m, _)| *m as i32 == model).map_or(0, |_| self.device_latency.load(Relaxed))
        } else if self.is_loaded() {
            self.reported_latency()
        } else if self.outboard_returns(params) {
            self.outboard_latency(params)
        } else {
            0
        }
    }

    /// Output stage (audio thread, after every strip ran): side `side` of the
    /// FX SEND written in `block`, if the slot ran as an outboard insert then.
    pub(crate) unsafe fn send(&self, side: usize, n: usize, block: u64) -> Option<&[f32]> {
        if self.send_block.load(Acquire) != block {
            return None;
        }
        (*self.send.get()).get(side).and_then(|s| s.get(..n))
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

    /// The latency the slot adds to a strip now: the plugin's, the outboard
    /// insert's or the built-in device's while it runs (not bypassed, not
    /// auto-bypassed).
    pub fn effective_latency(&self, params: &FxParams) -> u32 {
        let running = !params.bypass.load(Relaxed) && !self.is_faulted();
        if running {
            self.latency(params)
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
        self.device_latency.store(device.as_ref().map_or(0, |d| d.latency()), Relaxed);
        let new = device.map_or(std::ptr::null_mut(), Box::into_raw);
        let old = self.device.swap(new, AcqRel);
        self.faulted.store(false, Relaxed);
        self.meter.store(0.0);
        self.band_gains.iter().for_each(|g| g.store(0.0));
        self.readouts.iter().for_each(|g| g.store(0.0));
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

    /// The slot's level `i` (0/1 input left/right, 2/3 output left/right;
    /// linear, a peak falling at 60 dB/s): what its device or plugin gets
    /// and gives back (before FX MIX), or the outboard insert's FX SEND and
    /// RETURN.
    pub fn level(&self, i: usize) -> f32 {
        self.levels.get(i).map_or(0.0, AtomicF32::load)
    }

    /// The device's readout `i` (`FxAlgo::readouts`; 0 when idle).
    pub fn readout(&self, i: usize) -> f32 {
        self.readouts.get(i).map_or(0.0, AtomicF32::load)
    }

    /// Meters `x` into level `i` (one writer: the task running the slot).
    fn meter_level(&self, i: usize, x: &[f32], fall: f32) {
        let peak = x.iter().fold(0.0f32, |a, v| a.max(v.abs()));
        let prev = self.levels[i].load() * fall.powi(x.len() as i32);
        let level = if peak >= prev {
            peak
        } else if prev < 1e-9 {
            0.0
        } else {
            prev
        };
        self.levels[i].store(if level.is_finite() { level } else { 0.0 });
    }

    /// Meters `channels` (1 or 2) into levels `first` and `first + 1`.
    fn meter_channels(&self, first: usize, channels: &[&mut [f32]], fall: f32) {
        if let Some(c0) = channels.first() {
            self.meter_level(first, c0, fall);
            self.meter_level(first + 1, channels.get(1).map_or(&c0[..], |c| &c[..]), fall);
        }
    }

    /// A slot that passes its signal through: its input is its output, its
    /// device's readouts idle.
    fn meter_passthrough(&self, channels: &[&mut [f32]], fall: f32) {
        self.meter_channels(0, channels, fall);
        self.meter_channels(2, channels, fall);
        if self.readouts_live.swap(false, Relaxed) {
            self.readouts.iter().for_each(|g| g.store(0.0));
            self.band_gains.iter().for_each(|g| g.store(0.0));
            self.meter.store(0.0);
        }
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
    /// `channels` are 1 or 2 slices of equal length <= the prepared block size;
    /// `inputs`' non-null pointers hold at least as many samples.
    pub(crate) unsafe fn process(
        &self,
        channels: &mut [&mut [f32]],
        sample_rate: f64,
        params: &FxParams,
        inputs: &FxInputs,
        block: u64,
    ) {
        let n = channels.first().map_or(0, |c| c.len());
        // The meters' fall per sample (60 dB/s).
        let fall = (-FALL_DB_PER_S / 20.0 * std::f32::consts::LN_10 / (sample_rate as f32).max(1.0)).exp();
        let model = params.model.load(Relaxed);
        let plugin = self.slot.load(Acquire);
        let device = self.device.load(Acquire);
        let outboard = model == FX_EXT as i32 && plugin.is_null() && params.ext_outboard();
        let runnable = if model == FX_EXT as i32 {
            !plugin.is_null() || outboard
        } else {
            // A device for another model is still in place until the message
            // thread swaps the new one in: pass through meanwhile.
            !device.is_null() && (*device).model() as i32 == model
        };
        if n == 0 {
            return;
        }
        if !runnable || params.bypass.load(Relaxed) || self.faulted.load(Relaxed) {
            self.meter_passthrough(channels, fall);
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

        let mut returned = true;
        if !outboard {
            self.meter_channels(0, channels, fall);
        }
        let ok = if outboard {
            returned = self.run_outboard(channels, params, inputs, block);
            // The FX SEND is the outboard insert's input.
            let send = &*self.send.get();
            self.meter_level(0, &send[0][..n], fall);
            self.meter_level(1, &send[1][..n], fall);
            channels.iter().all(|c| all_finite(c))
        } else if model == FX_EXT as i32 {
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
            let mut readouts = [0.0f32; FX_READOUTS];
            dev.readouts(&mut readouts);
            for (a, v) in self.readouts.iter().zip(readouts) {
                a.store(if v.is_finite() { v } else { 0.0 });
            }
            self.readouts_live.store(true, Relaxed);
            channels.iter().all(|c| all_finite(c))
        };
        // The output: the device's or plugin's, the RETURN (or, without one,
        // the signal passing through).
        self.meter_channels(2, channels, fall);

        let mix = params.mix.load() / 100.0;
        // The dry history follows every block, so a MIX change finds it in time.
        let latency = match model == FX_EXT as i32 {
            true if outboard => self.outboard_latency(params) as usize,
            true => self.latency.load(Relaxed) as usize,
            false => self.device_latency.load(Relaxed) as usize,
        };
        let dry_ring = (*self.dry_ring.get()).as_deref();
        if let Some(ring) = dry_ring.filter(|_| latency > 0) {
            ring.write(&dry[0][..n], &dry[if channels.len() > 1 { 1 } else { 0 }][..n]);
        }
        if !returned {
            // An outboard insert without a RETURN passes its signal through.
        } else if !ok {
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

    /// The outboard insert (claiming task): sends `channels` at TRIM in FX
    /// MODE to the FX SEND and puts the RETURN in their place. False (signal
    /// left alone) without a RETURN.
    unsafe fn run_outboard(&self, channels: &mut [&mut [f32]], params: &FxParams, inputs: &FxInputs, block: u64) -> bool {
        let n = channels.first().map_or(0, |c| c.len());
        let send = &mut *self.send.get();
        if send[0].len() < n {
            return false;
        }
        let v = |i: usize| params.values[i].load();
        let mode = v(ext::MODE).round() as i32;
        let gain = db_to_gain(v(ext::TRIM).clamp(-18.0, 18.0));
        let stereo = channels.len() > 1;
        {
            let [s0, s1] = send;
            for k in 0..n {
                let l = channels[0][k];
                let r = if stereo { channels[1][k] } else { l };
                let (a, b) = match mode {
                    ext::MODE_MONO => (0.5 * (l + r), 0.5 * (l + r)),
                    ext::MODE_MID_SIDE => (0.5 * (l + r), 0.5 * (l - r)),
                    _ => (l, r),
                };
                s0[k] = a * gain;
                s1[k] = b * gain;
            }
        }
        self.send_block.store(block, Release);

        let Some(first) = ext_return_of(params) else { return false };
        let input = |port: i32| source_ptr(inputs.hw, inputs.playback, port).map(|p| std::slice::from_raw_parts(p, n));
        let a = input(first);
        let b = if mode == ext::MODE_MONO { a } else { input(first + 1) };
        let at = |x: Option<&[f32]>, k: usize| x.map_or(0.0, |x| x[k]);
        let decode = |k: usize| {
            let (x, y) = (at(a, k), at(b, k));
            match mode {
                ext::MODE_MONO => (x, x),
                ext::MODE_MID_SIDE => (x + y, x - y),
                _ => (x, y),
            }
        };
        match channels {
            [left, right, ..] => {
                for (k, (l, r)) in left.iter_mut().zip(right.iter_mut()).enumerate() {
                    (*l, *r) = decode(k);
                }
            }
            [mono] => {
                for (k, y) in mono.iter_mut().enumerate() {
                    let (l, r) = decode(k);
                    *y = 0.5 * (l + r);
                }
            }
            [] => {}
        }
        true
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
