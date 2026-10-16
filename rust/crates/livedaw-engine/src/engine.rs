//! The live mixing engine, laid out like a WING: input channels and aux
//! channels feed 16 (or more) stereo buses, 4 stereo mains and 8 stereo
//! matrices; DCAs and mute groups act on any strip; 16 FX slots serve the
//! strips' insert points.
//!
//! Per audio block:
//!  1. channels + aux channels read their inputs and run their chains (parallel)
//!  2. automix (gain sharing between channels in automix groups X/Y)
//!  3. buses 9+ sum the input sends feeding them                       (parallel)
//!  4. buses 1-8 sum input sends and sends from buses 9+               (parallel)
//!  5. mains sum channel/aux/bus main sends                            (parallel)
//!  6. matrices sum matrix sends from all strips + their direct input  (parallel)
//!  7. solo (PFL/AFL) builds the monitor bus, else it follows Main 1
//!  8. outputs are written, then safety-clipped
//!
//! Structure changes happen on the message thread by publishing a new
//! immutable `Topology`; the old one is retired to the garbage collector, which
//! drops it (and any removed strips) once the audio thread is done with it.

use crate::denormals::ScopedNoDenormals;
use crate::fx::FxRack;
use crate::garbage::Garbage;
use crate::params::*;
use crate::pool::WorkerPool;
use crate::sends::MAX_BUSES;
use crate::strip::{ProcessContext, Strip};
use livedaw_dsp::util::*;
use std::cell::UnsafeCell;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::atomic::{AtomicPtr, Ordering::*};
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::Instant;

#[derive(Default)]
struct Topology {
    channels: Vec<Arc<Strip>>,
    auxes: Vec<Arc<Strip>>,
    /// Audio thread scratch: the non-idle inputs of the current block (capacity
    /// reserved at publish, so pushing never allocates).
    active_inputs: UnsafeCell<Vec<Arc<Strip>>>,
    /// Buses that can feed buses 1-8 (slot >= 8).
    upper_buses: Vec<Arc<Strip>>,
    /// Buses 1-8 (slot < 8).
    lower_buses: Vec<Arc<Strip>>,
    mains: Vec<Arc<Strip>>,
    matrices: Vec<Arc<Strip>>,
}

// SAFETY: `active_inputs` is only touched by the audio thread (between stages,
// never while tasks run); everything else is immutable after publishing.
unsafe impl Sync for Topology {}
unsafe impl Send for Topology {}

impl Topology {
    fn inputs(&self) -> impl Iterator<Item = &Arc<Strip>> {
        self.channels.iter().chain(&self.auxes)
    }

    /// Inputs that produced signal this block (valid after stage 1).
    unsafe fn active(&self) -> &[Arc<Strip>] {
        &*self.active_inputs.get()
    }
    fn buses(&self) -> impl Iterator<Item = &Arc<Strip>> {
        self.lower_buses.iter().chain(&self.upper_buses)
    }
    fn all(&self) -> impl Iterator<Item = &Arc<Strip>> {
        self.inputs().chain(self.buses()).chain(&self.mains).chain(&self.matrices)
    }
}

#[derive(Clone, Copy, Debug)]
struct DeviceConfig {
    sample_rate: f64,
    block_size: usize,
    num_hw_inputs: usize,
}

struct Structure {
    channels: Vec<Arc<Strip>>,
    auxes: Vec<Arc<Strip>>,
    buses: Vec<Arc<Strip>>,
    mains: Vec<Arc<Strip>>,
    matrices: Vec<Arc<Strip>>,
    next_channel_id: i32,
    device: DeviceConfig,
}

impl Structure {
    fn all(&self) -> impl Iterator<Item = &Arc<Strip>> {
        self.channels.iter().chain(&self.auxes).chain(&self.buses).chain(&self.mains).chain(&self.matrices)
    }

    fn list(&self, kind: StripKind) -> &Vec<Arc<Strip>> {
        match kind {
            StripKind::Channel => &self.channels,
            StripKind::Aux => &self.auxes,
            StripKind::Bus => &self.buses,
            StripKind::Main => &self.mains,
            StripKind::Matrix => &self.matrices,
        }
    }
}

/// Names of console-wide groups (message thread only).
struct GroupNames {
    dcas: [String; NUM_DCAS],
    mute_groups: [String; NUM_MUTE_GROUPS],
}

impl Default for GroupNames {
    fn default() -> Self {
        Self {
            dcas: std::array::from_fn(|i| format!("DCA {}", i + 1)),
            mute_groups: std::array::from_fn(|i| format!("Mute {}", i + 1)),
        }
    }
}

/// Audio-thread state. Only modified by `prepare` while the device is stopped.
struct AudioState {
    sample_rate: f64,
    block_size: usize,
    hw_in_to_active: Vec<i32>,
    hw_out_to_active: Vec<i32>,
    hw_in_ptrs: Vec<*const f32>,
    hw_out_ptrs: Vec<*mut f32>,
    monitor: [Vec<f32>; 2],
    last_monitor_gain: f32,
    last_main_to_monitor: f32,
    block_counter: u64,
}

/// Shares raw pointers with the parallel tasks (they are only read).
struct SharedPtrs<'a>(&'a [*const f32]);
// SAFETY: the pointed-to buffers are owned by the host for the whole callback
// and only read by the tasks.
unsafe impl Sync for SharedPtrs<'_> {}

impl SharedPtrs<'_> {
    // A method (not field access) so closures capture the Sync wrapper as a whole.
    fn get(&self) -> &[*const f32] {
        self.0
    }
}

pub struct Engine {
    garbage: Garbage,
    pool: WorkerPool,
    topology: AtomicPtr<Topology>,
    structure: Mutex<Structure>,
    names: Mutex<GroupNames>,
    audio: UnsafeCell<AudioState>,
    pub controls: EngineControls,
    fx: Box<[FxRack]>,
}

// SAFETY: `audio` is only touched by the audio thread (process) or by prepare
// while the device is stopped; `structure`/`names` are behind mutexes the audio
// thread never takes; everything else is atomic or internally synchronised.
unsafe impl Sync for Engine {}
unsafe impl Send for Engine {}

const DEFAULT_SAMPLE_RATE: f64 = 48000.0;
const DEFAULT_BLOCK: usize = 512;

fn default_name(kind: StripKind, index: usize) -> String {
    match kind {
        StripKind::Channel => format!("Ch {}", index + 1),
        StripKind::Aux => format!("Aux {}", index + 1),
        StripKind::Bus => format!("Bus {}", index + 1),
        StripKind::Main => format!("Main {}", index + 1),
        StripKind::Matrix => format!("Mtx {}", index + 1),
    }
}

impl Engine {
    pub fn new(num_workers: usize) -> Self {
        let device = DeviceConfig { sample_rate: DEFAULT_SAMPLE_RATE, block_size: DEFAULT_BLOCK, num_hw_inputs: 0 };
        let engine = Self {
            garbage: Garbage::default(),
            pool: WorkerPool::new(num_workers),
            topology: AtomicPtr::new(std::ptr::null_mut()),
            structure: Mutex::new(Structure {
                channels: Vec::new(),
                auxes: Vec::new(),
                buses: Vec::new(),
                mains: Vec::new(),
                matrices: Vec::new(),
                next_channel_id: 1,
                device,
            }),
            names: Mutex::new(GroupNames::default()),
            audio: UnsafeCell::new(AudioState {
                sample_rate: DEFAULT_SAMPLE_RATE,
                block_size: DEFAULT_BLOCK,
                hw_in_to_active: Vec::new(),
                hw_out_to_active: Vec::new(),
                hw_in_ptrs: Vec::new(),
                hw_out_ptrs: Vec::new(),
                monitor: [vec![0.0; DEFAULT_BLOCK], vec![0.0; DEFAULT_BLOCK]],
                last_monitor_gain: 0.0,
                last_main_to_monitor: 1.0,
                block_counter: 0,
            }),
            controls: EngineControls::default(),
            fx: (0..NUM_FX).map(|_| FxRack::default()).collect(),
        };
        // SAFETY: not published yet, no audio callback.
        unsafe {
            for f in engine.fx.iter() {
                f.prepare(DEFAULT_BLOCK);
            }
        }
        let mut st = engine.lock();
        engine.build_fixed(&mut st);
        engine.publish(&st);
        drop(st);
        engine
    }

    /// Recommended worker count: leave a core for the audio thread and one for the UI/OS.
    pub fn default_num_workers() -> usize {
        std::thread::available_parallelism().map_or(0, |n| n.get().saturating_sub(2))
    }

    fn lock(&self) -> MutexGuard<'_, Structure> {
        self.structure.lock().unwrap_or_else(|e| e.into_inner())
    }

    pub fn pool(&self) -> &WorkerPool {
        &self.pool
    }

    pub fn garbage(&self) -> &Garbage {
        &self.garbage
    }

    // =========================================================================
    // Structure (message thread)

    fn new_strip(&self, st: &Structure, kind: StripKind, id: i32, index: usize) -> Arc<Strip> {
        let s = Arc::new(Strip::new(kind, id));
        s.set_name(&default_name(kind, index));
        // SAFETY: not published yet.
        unsafe { s.prepare(st.device.sample_rate, st.device.block_size) };
        s
    }

    /// Aux channels, the WING's 16 buses, 4 mains and 8 matrices.
    fn build_fixed(&self, st: &mut Structure) {
        st.auxes = (0..NUM_AUX).map(|i| self.new_strip(st, StripKind::Aux, i as i32, i)).collect();
        st.buses = (0..WING_BUSES).map(|i| self.new_strip(st, StripKind::Bus, i as i32, i)).collect();
        st.mains = (0..NUM_MAINS).map(|i| self.new_strip(st, StripKind::Main, i as i32, i)).collect();
        st.matrices = (0..NUM_MATRICES).map(|i| self.new_strip(st, StripKind::Matrix, i as i32, i)).collect();
        // Main 1 plays on the first two outputs.
        st.mains[0].params.output_left.store(0, Relaxed);
        st.mains[0].params.output_right.store(1, Relaxed);
    }

    fn publish(&self, st: &Structure) {
        let mut buses = st.buses.clone();
        buses.sort_by_key(|b| b.id());
        let split = buses.iter().position(|b| b.id() as usize >= BUS_TO_BUS_TARGETS).unwrap_or(buses.len());
        let upper = buses.split_off(split);
        let next = Box::new(Topology {
            channels: st.channels.clone(),
            auxes: st.auxes.clone(),
            active_inputs: UnsafeCell::new(Vec::with_capacity(st.channels.len() + st.auxes.len())),
            upper_buses: upper,
            lower_buses: buses,
            mains: st.mains.clone(),
            matrices: st.matrices.clone(),
        });
        let old = self.topology.swap(Box::into_raw(next), AcqRel);
        if !old.is_null() {
            // SAFETY: created by Box::into_raw; now unpublished.
            self.garbage.retire(unsafe { Box::from_raw(old) });
        }
    }

    fn create_channel(&self, st: &mut Structure, requested_id: i32) -> Arc<Strip> {
        let index = st.channels.len();
        let id_free = requested_id > 0 && st.channels.iter().all(|s| s.id() != requested_id);
        let id = if id_free { requested_id } else { st.next_channel_id };
        st.next_channel_id = st.next_channel_id.max(id + 1);

        let strip = self.new_strip(st, StripKind::Channel, id, index);
        let patched = st.device.num_hw_inputs == 0 || index < st.device.num_hw_inputs;
        strip.params.input_left.store(if patched { index as i32 } else { -1 }, Relaxed);
        strip
    }

    /// Adds an input channel. `requested_id` restores a saved id (ignored if taken).
    pub fn add_channel(&self, requested_id: i32) -> Arc<Strip> {
        let mut st = self.lock();
        let strip = self.create_channel(&mut st, requested_id);
        st.channels.push(strip.clone());
        self.publish(&st);
        strip
    }

    /// Grows or shrinks the channel count in one topology change.
    pub fn set_num_channels(&self, count: usize) {
        let mut st = self.lock();
        if count == st.channels.len() {
            return;
        }
        while st.channels.len() < count {
            let s = self.create_channel(&mut st, -1);
            st.channels.push(s);
        }
        st.channels.truncate(count);
        self.publish(&st);
    }

    /// Adds a bus in the first free slot (or `requested_slot` if free). The
    /// console has 16; more can be added up to `MAX_BUSES`.
    pub fn add_bus(&self, requested_slot: i32) -> Option<Arc<Strip>> {
        let mut st = self.lock();
        let mut used = vec![false; MAX_BUSES];
        for b in &st.buses {
            used[b.id() as usize] = true;
        }
        let slot = if requested_slot >= 0 && (requested_slot as usize) < MAX_BUSES && !used[requested_slot as usize] {
            requested_slot as usize
        } else {
            used.iter().position(|u| !u)?
        };

        // No strip may carry a send from an old bus in this slot. (The ramp state
        // is left alone: a callback may still be finishing with a removed bus.)
        for s in st.channels.iter().chain(&st.auxes).chain(&st.buses) {
            if let Some(send) = s.sends.find(slot) {
                send.enabled.store(false, Relaxed);
                send.level_db.store(0.0);
            }
        }

        let strip = self.new_strip(&st, StripKind::Bus, slot as i32, slot);
        st.buses.push(strip.clone());
        st.buses.sort_by_key(|b| b.id());
        self.publish(&st);
        Some(strip)
    }

    /// Removes a channel, or a bus beyond the console's 16. The strip is
    /// dropped later, once unreachable.
    pub fn remove_strip(&self, strip: *const Strip) -> bool {
        let mut st = self.lock();
        let removed = match st.channels.iter().position(|s| Arc::as_ptr(s) == strip) {
            Some(i) => Some(st.channels.remove(i)),
            None => {
                st.buses.iter().position(|s| Arc::as_ptr(s) == strip && s.id() as usize >= WING_BUSES).map(|i| st.buses.remove(i))
            }
        };
        let Some(removed) = removed else { return false };
        self.publish(&st);
        if removed.kind().is_bus() {
            for s in st.channels.iter().chain(&st.auxes).chain(&st.buses) {
                if let Some(send) = s.sends.find(removed.id() as usize) {
                    send.enabled.store(false, Relaxed);
                }
            }
        }
        drop(removed); // the retired topology keeps it alive until collected
        true
    }

    /// Back to a fresh console: no channels, default aux/buses/mains/matrices,
    /// DCAs, mute groups and FX settings.
    pub fn reset(&self) {
        let mut st = self.lock();
        st.channels.clear();
        st.next_channel_id = 1;
        self.build_fixed(&mut st);
        self.publish(&st);
        self.controls.reset();
        *self.names.lock().unwrap_or_else(|e| e.into_inner()) = GroupNames::default();
    }

    pub fn strips(&self, kind: StripKind) -> Vec<Arc<Strip>> {
        self.lock().list(kind).clone()
    }

    pub fn num_strips(&self, kind: StripKind) -> usize {
        self.lock().list(kind).len()
    }

    pub fn strip_at(&self, kind: StripKind, i: usize) -> Option<Arc<Strip>> {
        self.lock().list(kind).get(i).cloned()
    }

    pub fn channels(&self) -> Vec<Arc<Strip>> {
        self.strips(StripKind::Channel)
    }

    pub fn buses(&self) -> Vec<Arc<Strip>> {
        self.strips(StripKind::Bus)
    }

    pub fn num_channels(&self) -> usize {
        self.num_strips(StripKind::Channel)
    }

    pub fn channel_at(&self, i: usize) -> Option<Arc<Strip>> {
        self.strip_at(StripKind::Channel, i)
    }

    pub fn bus_in_slot(&self, slot: i32) -> Option<Arc<Strip>> {
        self.lock().buses.iter().find(|b| b.id() == slot).cloned()
    }

    pub fn find_channel(&self, id: i32) -> Option<Arc<Strip>> {
        self.lock().channels.iter().find(|s| s.id() == id).cloned()
    }

    /// Main 1 (the default monitor source).
    pub fn main(&self, i: usize) -> Option<Arc<Strip>> {
        self.strip_at(StripKind::Main, i)
    }

    pub fn dca_name(&self, i: usize) -> String {
        self.names.lock().unwrap_or_else(|e| e.into_inner()).dcas.get(i).cloned().unwrap_or_default()
    }

    pub fn set_dca_name(&self, i: usize, name: &str) {
        if let Some(n) = self.names.lock().unwrap_or_else(|e| e.into_inner()).dcas.get_mut(i) {
            *n = name.to_owned();
        }
    }

    pub fn mute_group_name(&self, i: usize) -> String {
        self.names.lock().unwrap_or_else(|e| e.into_inner()).mute_groups.get(i).cloned().unwrap_or_default()
    }

    pub fn set_mute_group_name(&self, i: usize, name: &str) {
        if let Some(n) = self.names.lock().unwrap_or_else(|e| e.into_inner()).mute_groups.get_mut(i) {
            *n = name.to_owned();
        }
    }

    /// FX slot `i` (0..16).
    pub fn fx(&self, i: usize) -> Option<&FxRack> {
        self.fx.get(i)
    }

    pub fn num_fx(&self) -> usize {
        self.fx.len()
    }

    /// Strips whose inserts currently use FX slot `i`.
    pub fn fx_users(&self, i: usize) -> Vec<Arc<Strip>> {
        self.lock().all().filter(|s| s.uses_fx(i)).cloned().collect()
    }

    pub fn total_dsp_faults(&self) -> i32 {
        self.lock().all().map(|s| s.meters.dsp_faults.load(Relaxed)).sum()
    }

    pub fn num_faulted_fx(&self) -> usize {
        self.fx.iter().filter(|f| f.is_faulted()).count()
    }

    /// Message thread: frees what the audio thread can no longer see.
    pub fn collect(&self) {
        self.garbage.collect();
    }

    pub fn set_device_running(&self, running: bool) {
        self.garbage.set_device_running(running);
    }

    // =========================================================================
    // Device

    /// Prepares for a device configuration. `in_map[hw]` / `out_map[hw]` give the
    /// index of hardware channel `hw` in the buffers passed to `process` (-1 = inactive).
    ///
    /// # Safety
    /// The audio callback must not be running (call it before the device starts).
    pub unsafe fn prepare(&self, sample_rate: f64, block_size: usize, in_map: &[i32], out_map: &[i32]) {
        let block_size = block_size.max(1);
        let mut st = self.lock();
        st.device = DeviceConfig { sample_rate, block_size, num_hw_inputs: in_map.len() };

        let a = &mut *self.audio.get();
        a.sample_rate = sample_rate;
        a.block_size = block_size;
        a.hw_in_to_active = in_map.to_vec();
        a.hw_out_to_active = out_map.to_vec();
        a.hw_in_ptrs = vec![std::ptr::null(); in_map.len()];
        a.hw_out_ptrs = vec![std::ptr::null_mut(); out_map.len()];
        a.monitor = [vec![0.0; block_size], vec![0.0; block_size]];

        for s in st.all() {
            s.prepare(sample_rate, block_size);
        }
        for f in self.fx.iter() {
            f.prepare(block_size);
        }
        self.pool.set_block_duration_hint(sample_rate, block_size);
    }

    pub fn sample_rate(&self) -> f64 {
        self.lock().device.sample_rate
    }

    pub fn block_size(&self) -> usize {
        self.lock().device.block_size
    }

    // =========================================================================
    // Audio thread

    /// Processes one callback. `ins`/`outs` are the *active* device channels.
    ///
    /// # Safety
    /// Audio thread only (never concurrently with itself or `prepare`). Every
    /// non-null pointer must be valid for `num_samples` samples.
    pub unsafe fn process(&self, ins: &[*const f32], outs: &[*mut f32], num_samples: usize) {
        let _ftz = ScopedNoDenormals::new();
        self.garbage.callback_started();
        let start = Instant::now();

        for &o in outs {
            if !o.is_null() {
                std::slice::from_raw_parts_mut(o, num_samples).fill(0.0);
            }
        }

        let result = catch_unwind(AssertUnwindSafe(|| self.process_inner(ins, outs, num_samples)));
        if result.is_err() {
            // A bug must never take the show down: silence this block and report it.
            for &o in outs {
                if !o.is_null() {
                    std::slice::from_raw_parts_mut(o, num_samples).fill(0.0);
                }
            }
            self.controls.engine_faults.fetch_add(1, Relaxed);
        }

        self.garbage.callback_finished();

        let a = &*self.audio.get();
        let load = (start.elapsed().as_secs_f64() * a.sample_rate / num_samples.max(1) as f64) as f32;
        self.controls.dsp_load.store(0.9 * self.controls.dsp_load.load() + 0.1 * load);
    }

    unsafe fn process_inner(&self, ins: &[*const f32], outs: &[*mut f32], num_samples: usize) {
        let topo = &*self.topology.load(Acquire);
        let a = &mut *self.audio.get();
        let block = a.block_size;

        // Devices may deliver more samples than announced: process in chunks.
        let mut start = 0;
        while start < num_samples {
            let n = block.min(num_samples - start);

            for (hw, &active) in a.hw_in_to_active.iter().enumerate() {
                a.hw_in_ptrs[hw] = if active >= 0 && (active as usize) < ins.len() && !ins[active as usize].is_null() {
                    ins[active as usize].add(start)
                } else {
                    std::ptr::null()
                };
            }
            for (hw, &active) in a.hw_out_to_active.iter().enumerate() {
                a.hw_out_ptrs[hw] = if active >= 0 && (active as usize) < outs.len() && !outs[active as usize].is_null() {
                    outs[active as usize].add(start)
                } else {
                    std::ptr::null_mut()
                };
            }

            a.block_counter += 1;
            self.process_chunk(topo, a, n);
            start += n;
        }
    }

    unsafe fn process_chunk(&self, topo: &Topology, a: &mut AudioState, n: usize) {
        let hw = SharedPtrs(&a.hw_in_ptrs);
        let hw = &hw;
        let mute_groups = self.controls.mute_groups.load(Relaxed);
        let block = a.block_counter;
        let ctx_of = |hw| self.context(topo, hw, mute_groups, block);
        let ctx_of = &ctx_of;

        // 1. Channels and aux channels (parallel)
        let num_ch = topo.channels.len();
        self.pool.parallel_for(num_ch + topo.auxes.len(), &|i| {
            let s = if i < num_ch { &topo.channels[i] } else { &topo.auxes[i - num_ch] };
            s.process(&ctx_of(hw.get()), n);
        });

        // Only inputs with signal feed the buses (unused channels cost nothing below).
        {
            let active = &mut *topo.active_inputs.get();
            active.clear();
            for s in topo.inputs() {
                if !s.dsp().idle() && active.len() < active.capacity() {
                    active.push(s.clone());
                }
            }
        }

        // 2. Automix
        self.automix(topo, n);

        // 3./4. Buses: 9+ first (they may feed buses 1-8), then 1-8.
        self.pool.parallel_for(topo.upper_buses.len(), &|i| {
            let bus = &topo.upper_buses[i];
            sum_bus(bus, topo.active().iter(), n);
            bus.process(&ctx_of(hw.get()), n);
        });
        self.pool.parallel_for(topo.lower_buses.len(), &|i| {
            let bus = &topo.lower_buses[i];
            sum_bus(bus, topo.active().iter().chain(&topo.upper_buses), n);
            bus.process(&ctx_of(hw.get()), n);
        });

        // 5. Mains
        self.pool.parallel_for(topo.mains.len(), &|i| {
            let main = &topo.mains[i];
            sum_main(main, i, topo.active().iter().chain(topo.buses()), n);
            main.process(&ctx_of(hw.get()), n);
        });

        // 6. Matrices
        self.pool.parallel_for(topo.matrices.len(), &|i| {
            let mtx = &topo.matrices[i];
            sum_matrix(mtx, i, topo, n);
            mtx.process(&ctx_of(hw.get()), n);
        });

        // 7. Solo / monitor
        self.build_monitor(topo, a, n);

        // 8. Outputs, then the safety clip
        self.write_outputs(topo, a, n);
        self.protect_outputs(a, n);
    }

    fn context<'a>(&'a self, topo: &'a Topology, hw: &'a [*const f32], mute_groups: u32, block: u64) -> ProcessContext<'a> {
        ProcessContext {
            fx: &self.fx,
            fx_params: &self.controls.fx,
            dcas: &self.controls.dcas,
            mute_groups,
            hw_inputs: hw,
            channels: &topo.channels,
            block,
        }
    }

    /// Gain sharing between channels whose post insert is set to AUTO_X / AUTO_Y.
    unsafe fn automix(&self, topo: &Topology, n: usize) {
        let mut totals = [0.0f32; 2];
        let group_of = |s: &Strip| -> Option<usize> {
            let ins = &s.params.post_insert;
            match ins.mode.load(Relaxed) {
                1 if ins.on.load(Relaxed) => Some(0),
                2 if ins.on.load(Relaxed) => Some(1),
                _ => None,
            }
        };
        for s in topo.inputs() {
            if let Some(g) = group_of(s) {
                totals[g] += s.dsp().automix_power();
            }
        }
        if totals == [0.0, 0.0] {
            return;
        }
        for s in topo.inputs() {
            if let Some(g) = group_of(s) {
                let share = if totals[g] > 1e-12 { s.dsp().automix_power() / totals[g] } else { 0.0 };
                // Power share -> gain; never below -40 dB so nobody vanishes completely.
                let gain = share.sqrt().max(0.01);
                s.apply_automix(gain, n);
            }
        }
    }

    unsafe fn build_monitor(&self, topo: &Topology, a: &mut AudioState, n: usize) {
        let dcas = &self.controls.dcas;
        let solo = topo.all().any(|s| s.is_soloed(dcas));
        self.controls.any_solo_active.store(solo, Relaxed);
        let afl = self.controls.solo_afl.load(Relaxed);

        let [m0, m1] = &mut a.monitor;
        let (m0, m1) = (&mut m0[..n], &mut m1[..n]);
        m0.fill(0.0);
        m1.fill(0.0);

        for s in topo.all() {
            let target = if solo && s.is_soloed(dcas) { 1.0 } else { 0.0 };
            let last = s.last_solo();
            if target != 0.0 || *last != 0.0 {
                let d = s.dsp();
                if afl || s.kind().is_output() {
                    add_with_gain_ramp(m0, d.out(0, n), *last, target);
                    add_with_gain_ramp(m1, d.out(1, n), *last, target);
                } else {
                    add_with_gain_ramp(m0, d.pfl(0, n), *last, target);
                    add_with_gain_ramp(m1, d.pfl(1, n), *last, target);
                }
            }
            *last = target;
        }

        // With nothing soloed the monitor follows Main 1.
        let main_target = if solo { 0.0 } else { 1.0 };
        if let Some(main) = topo.mains.first() {
            let md = main.dsp();
            add_with_gain_ramp(m0, md.out(0, n), a.last_main_to_monitor, main_target);
            add_with_gain_ramp(m1, md.out(1, n), a.last_main_to_monitor, main_target);
        }
        a.last_main_to_monitor = main_target;

        let level = db_to_gain(self.controls.monitor_level_db.load());
        apply_gain_ramp(m0, a.last_monitor_gain, level);
        apply_gain_ramp(m1, a.last_monitor_gain, level);
        a.last_monitor_gain = level;

        self.controls.monitor_peak[0].fetch_max(peak_level(m0));
        self.controls.monitor_peak[1].fetch_max(peak_level(m1));
    }

    unsafe fn write_outputs(&self, topo: &Topology, a: &mut AudioState, n: usize) {
        let outs = &a.hw_out_ptrs;
        let add_to = |hw: i32, src: &[f32]| {
            if hw >= 0 && (hw as usize) < outs.len() {
                let dst = outs[hw as usize];
                if !dst.is_null() {
                    let dst = std::slice::from_raw_parts_mut(dst, n);
                    dst.iter_mut().zip(src).for_each(|(d, s)| *d += s);
                }
            }
        };

        for s in topo.all() {
            let l = s.params.output_left.load(Relaxed);
            let r = s.params.output_right.load(Relaxed);
            if l < 0 && r < 0 {
                continue;
            }
            let d = s.dsp();
            if s.kind().is_input() {
                if d.idle() {
                    continue;
                }
                // Channel direct outs carry the processed signal before the fader.
                add_to(l, d.pfl(0, n));
                add_to(r, d.pfl(1, n));
            } else {
                add_to(l, d.out(0, n));
                add_to(r, d.out(1, n));
            }
        }

        add_to(self.controls.monitor_output_left.load(Relaxed), &a.monitor[0][..n]);
        add_to(self.controls.monitor_output_right.load(Relaxed), &a.monitor[1][..n]);
    }

    unsafe fn protect_outputs(&self, a: &AudioState, n: usize) {
        if !self.controls.output_protection.load(Relaxed) {
            return;
        }
        let ceiling = db_to_gain(self.controls.output_ceiling_db.load()).clamp(0.0, 4.0);
        let mut engaged = false;
        for &o in &a.hw_out_ptrs {
            if o.is_null() {
                continue;
            }
            let out = std::slice::from_raw_parts_mut(o, n);
            if !all_finite(out) {
                out.fill(0.0);
                engaged = true;
            } else if peak_level(out) > ceiling {
                out.iter_mut().for_each(|x| *x = x.clamp(-ceiling, ceiling));
                engaged = true;
            }
        }
        if engaged {
            self.controls.output_clip_events.fetch_add(1, Relaxed);
        }
    }
}

/// Adds a stereo pair into `dst`'s work buffers with ramped gains.
#[inline]
unsafe fn add_pair(dst: &Strip, n: usize, l: &[f32], r: &[f32], last: &mut [f32; 2], target: [f32; 2]) {
    let d = dst.dsp_mut();
    add_with_gain_ramp(d.work_mut(0, n), l, last[0], target[0]);
    add_with_gain_ramp(d.work_mut(1, n), r, last[1], target[1]);
    *last = target;
}

/// Pan gains of a strip for signals taken before its own pan (pre-fader sends).
#[inline]
unsafe fn pre_gains(src: &Strip, pan: Option<f32>) -> [f32; 2] {
    let d = src.dsp();
    match pan {
        None => d.pan_gains(),
        Some(p) => {
            let (l, r) = if d.channels() == 1 { constant_power_pan(p) } else { stereo_balance(p) };
            [l, r]
        }
    }
}

/// # Safety
/// Bus task only, after the stages feeding it.
unsafe fn sum_bus<'a>(bus: &Strip, sources: impl Iterator<Item = &'a Arc<Strip>>, n: usize) {
    bus.begin_summed_block(n);
    let slot = bus.id() as usize;

    for s in sources {
        let Some(send) = s.sends.find(slot) else { continue };
        let level = if send.enabled.load(Relaxed) { db_to_gain(send.level_db.load()) } else { 0.0 };
        let last = &mut *send.last_gain.get();
        let src = s.dsp();
        if (level == 0.0 && *last == [0.0, 0.0]) || src.idle() {
            *last = [0.0, 0.0]; // silent strips have nothing to add
            continue;
        }
        match send.mode.load(Relaxed) {
            2 => add_pair(bus, n, src.out(0, n), src.out(1, n), last, [level, level]),
            mode => {
                let pan = if send.pan_link.load(Relaxed) { None } else { Some(send.pan.load() / 100.0) };
                let g = pre_gains(s, pan);
                let (l, r) =
                    if mode == 0 { (src.tap_signal(0, n), src.tap_signal(1, n)) } else { (src.post(0, n), src.post(1, n)) };
                add_pair(bus, n, l, r, last, [level * g[0], level * g[1]]);
            }
        }
    }
}

/// # Safety
/// Main task `index` only, after the bus stages.
unsafe fn sum_main<'a>(main: &Strip, index: usize, sources: impl Iterator<Item = &'a Arc<Strip>>, n: usize) {
    main.begin_summed_block(n);
    for s in sources {
        let send = &s.params.mains[index];
        let level = if send.on.load(Relaxed) { db_to_gain(send.level_db.load()) } else { 0.0 };
        let last = s.last_main(index);
        let src = s.dsp();
        if (level == 0.0 && *last == [0.0, 0.0]) || src.idle() {
            *last = [0.0, 0.0];
            continue;
        }
        if send.pre.load(Relaxed) {
            let g = pre_gains(s, None);
            add_pair(main, n, src.tap_signal(0, n), src.tap_signal(1, n), last, [level * g[0], level * g[1]]);
        } else {
            add_pair(main, n, src.out(0, n), src.out(1, n), last, [level, level]);
        }
    }
}

/// # Safety
/// Matrix task `index` only, after the main stage.
unsafe fn sum_matrix(mtx: &Strip, index: usize, topo: &Topology, n: usize) {
    mtx.begin_summed_block(n);
    for s in topo.active().iter().chain(topo.buses()).chain(&topo.mains) {
        let send = &s.params.matrix[index];
        let level = if send.on.load(Relaxed) { db_to_gain(send.level_db.load()) } else { 0.0 };
        let last = s.last_mtx(index);
        let src = s.dsp();
        if (level == 0.0 && *last == [0.0, 0.0]) || src.idle() {
            *last = [0.0, 0.0];
            continue;
        }
        let (bl, br) = stereo_balance(send.pan.load() / 100.0);
        if send.pre.load(Relaxed) {
            let g = pre_gains(s, None);
            add_pair(mtx, n, src.tap_signal(0, n), src.tap_signal(1, n), last, [level * g[0] * bl, level * g[1] * br]);
        } else {
            add_pair(mtx, n, src.out(0, n), src.out(1, n), last, [level * bl, level * br]);
        }
    }

    // Direct input from an input channel (mix-minus: invert it).
    let p = &mtx.params;
    let source = p.direct_source.load(Relaxed);
    let on = p.direct_on.load(Relaxed) && source >= 0;
    let gain =
        if on { db_to_gain(p.direct_level_db.load()) * if p.direct_invert.load(Relaxed) { -1.0 } else { 1.0 } } else { 0.0 };
    let prev = mtx.dsp().last_direct();
    if let Some(ch) = topo.channels.get(source.max(0) as usize).filter(|_| source >= 0) {
        let src = ch.dsp();
        if !src.idle() && (gain != 0.0 || prev != 0.0) {
            let (l, r) = if p.direct_pre.load(Relaxed) { (src.pfl(0, n), src.pfl(1, n)) } else { (src.out(0, n), src.out(1, n)) };
            let mut ramp = [prev, prev];
            add_pair(mtx, n, l, r, &mut ramp, [gain, gain]);
        }
    }
    mtx.dsp_mut().set_last_direct(gain);
}

impl Drop for Engine {
    fn drop(&mut self) {
        self.pool.shutdown();
        let topo = self.topology.swap(std::ptr::null_mut(), AcqRel);
        if !topo.is_null() {
            // SAFETY: created by Box::into_raw in publish().
            drop(unsafe { Box::from_raw(topo) });
        }
        self.garbage.collect_all_now();
    }
}
