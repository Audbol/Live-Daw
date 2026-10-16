//! The live mixing engine.
//!
//! Per audio block:
//!  1. every input strip reads its hardware input(s) and runs its chain   (parallel)
//!  2. every bus sums the sends feeding it and runs its chain              (parallel)
//!  3. the master sums assigned channels and groups and runs its chain
//!  4. solo (PFL/AFL) builds the monitor bus, or the monitor follows the master
//!  5. outputs are written, then safety-clipped
//!
//! Structure changes (strips added/removed) happen on the message thread by
//! publishing a new immutable `Topology`; the old one is retired to the garbage
//! collector, which drops it (and any removed strips) once the audio thread is
//! provably done with it.

use crate::denormals::ScopedNoDenormals;
use crate::garbage::Garbage;
use crate::params::{EngineControls, StripKind};
use crate::pool::WorkerPool;
use crate::sends::MAX_BUSES;
use crate::strip::Strip;
use livedaw_dsp::util::*;
use std::cell::UnsafeCell;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::atomic::{AtomicPtr, Ordering::*};
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::Instant;

struct Topology {
    inputs: Vec<Arc<Strip>>,
    buses: Vec<Arc<Strip>>,
    master: Arc<Strip>,
}

#[derive(Clone, Copy, Debug)]
struct DeviceConfig {
    sample_rate: f64,
    block_size: usize,
    num_hw_inputs: usize,
}

struct Structure {
    inputs: Vec<Arc<Strip>>,
    buses: Vec<Arc<Strip>>,
    master: Arc<Strip>,
    next_input_id: i32,
    device: DeviceConfig,
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
    last_master_to_monitor: f32,
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
    audio: UnsafeCell<AudioState>,
    pub controls: EngineControls,
}

// SAFETY: `audio` is only touched by the audio thread (process) or by prepare
// while the device is stopped; `structure` is behind a mutex the audio thread
// never takes; everything else is atomic.
unsafe impl Sync for Engine {}
unsafe impl Send for Engine {}

const DEFAULT_SAMPLE_RATE: f64 = 48000.0;
const DEFAULT_BLOCK: usize = 512;

impl Engine {
    pub fn new(num_workers: usize) -> Self {
        let master = Arc::new(Strip::new(StripKind::Master, 0));
        master.set_name("Master");
        // SAFETY: not published yet.
        unsafe { master.prepare(DEFAULT_SAMPLE_RATE, DEFAULT_BLOCK) };

        let engine = Self {
            garbage: Garbage::default(),
            pool: WorkerPool::new(num_workers),
            topology: AtomicPtr::new(std::ptr::null_mut()),
            structure: Mutex::new(Structure {
                inputs: Vec::new(),
                buses: Vec::new(),
                master,
                next_input_id: 1,
                device: DeviceConfig { sample_rate: DEFAULT_SAMPLE_RATE, block_size: DEFAULT_BLOCK, num_hw_inputs: 0 },
            }),
            audio: UnsafeCell::new(AudioState {
                sample_rate: DEFAULT_SAMPLE_RATE,
                block_size: DEFAULT_BLOCK,
                hw_in_to_active: Vec::new(),
                hw_out_to_active: Vec::new(),
                hw_in_ptrs: Vec::new(),
                hw_out_ptrs: Vec::new(),
                monitor: [vec![0.0; DEFAULT_BLOCK], vec![0.0; DEFAULT_BLOCK]],
                last_monitor_gain: 0.0,
                last_master_to_monitor: 1.0,
            }),
            controls: EngineControls::default(),
        };
        engine.publish(&engine.lock());
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

    fn publish(&self, st: &Structure) {
        let next = Box::new(Topology { inputs: st.inputs.clone(), buses: st.buses.clone(), master: st.master.clone() });
        let old = self.topology.swap(Box::into_raw(next), AcqRel);
        if !old.is_null() {
            // SAFETY: created by Box::into_raw; now unpublished.
            self.garbage.retire(unsafe { Box::from_raw(old) });
        }
    }

    fn create_input(&self, st: &mut Structure, requested_id: i32) -> Arc<Strip> {
        let index = st.inputs.len();
        let id_free = requested_id > 0 && st.inputs.iter().all(|s| s.id() != requested_id);
        let id = if id_free { requested_id } else { st.next_input_id };
        st.next_input_id = st.next_input_id.max(id + 1);

        let strip = Arc::new(Strip::new(StripKind::Input, id));
        strip.set_name(&format!("Ch {}", index + 1));
        let patched = st.device.num_hw_inputs == 0 || index < st.device.num_hw_inputs;
        strip.params.input_left.store(if patched { index as i32 } else { -1 }, Relaxed);
        // SAFETY: not published yet.
        unsafe { strip.prepare(st.device.sample_rate, st.device.block_size) };
        strip
    }

    /// Adds an input channel. `requested_id` restores a saved id (ignored if taken).
    pub fn add_input(&self, requested_id: i32) -> Arc<Strip> {
        let mut st = self.lock();
        let strip = self.create_input(&mut st, requested_id);
        st.inputs.push(strip.clone());
        self.publish(&st);
        strip
    }

    /// Grows or shrinks the channel count in one topology change.
    pub fn set_num_inputs(&self, count: usize) {
        let mut st = self.lock();
        if count == st.inputs.len() {
            return;
        }
        while st.inputs.len() < count {
            let s = self.create_input(&mut st, -1);
            st.inputs.push(s);
        }
        st.inputs.truncate(count);
        self.publish(&st);
    }

    /// Adds an aux or group bus in the first free slot (or `requested_slot` if free).
    pub fn add_bus(&self, kind: StripKind, requested_slot: i32) -> Option<Arc<Strip>> {
        if !kind.is_bus() {
            return None;
        }
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

        // No channel may carry a send from an old bus in this slot. (The ramp state
        // is left alone: a callback may still be finishing with a removed bus.)
        for input in &st.inputs {
            if let Some(send) = input.sends.get_or_create(slot) {
                send.enabled.store(false, Relaxed);
                send.level_db.store(0.0);
                send.pre_fader.store(kind == StripKind::Aux, Relaxed);
            }
        }

        let strip = Arc::new(Strip::new(kind, slot as i32));
        let count = 1 + st.buses.iter().filter(|b| b.kind() == kind).count();
        strip.set_name(&format!("{} {count}", if kind == StripKind::Aux { "Aux" } else { "Group" }));
        // SAFETY: not published yet.
        unsafe { strip.prepare(st.device.sample_rate, st.device.block_size) };
        st.buses.push(strip.clone());
        self.publish(&st);
        Some(strip)
    }

    /// Removes an input or bus. The strip is dropped later, once unreachable.
    pub fn remove_strip(&self, strip: *const Strip) -> bool {
        let mut st = self.lock();
        let removed = match st.inputs.iter().position(|s| Arc::as_ptr(s) == strip) {
            Some(i) => Some(st.inputs.remove(i)),
            None => st.buses.iter().position(|s| Arc::as_ptr(s) == strip).map(|i| st.buses.remove(i)),
        };
        let Some(removed) = removed else { return false };
        self.publish(&st);
        if removed.kind().is_bus() {
            for input in &st.inputs {
                if let Some(send) = input.sends.find(removed.id() as usize) {
                    send.enabled.store(false, Relaxed);
                }
            }
        }
        drop(removed); // the retired topology keeps it alive until collected
        true
    }

    /// Removes all inputs and buses and installs a fresh master.
    pub fn reset(&self) {
        let mut st = self.lock();
        let master = Arc::new(Strip::new(StripKind::Master, 0));
        master.set_name("Master");
        // SAFETY: not published yet.
        unsafe { master.prepare(st.device.sample_rate, st.device.block_size) };
        st.master = master;
        st.inputs.clear();
        st.buses.clear();
        st.next_input_id = 1;
        self.publish(&st);
        self.controls.solo_afl.store(false, Relaxed);
        self.controls.monitor_level_db.store(0.0);
    }

    pub fn inputs(&self) -> Vec<Arc<Strip>> {
        self.lock().inputs.clone()
    }

    pub fn buses(&self) -> Vec<Arc<Strip>> {
        self.lock().buses.clone()
    }

    pub fn master(&self) -> Arc<Strip> {
        self.lock().master.clone()
    }

    pub fn num_inputs(&self) -> usize {
        self.lock().inputs.len()
    }

    pub fn num_buses(&self) -> usize {
        self.lock().buses.len()
    }

    pub fn input_at(&self, i: usize) -> Option<Arc<Strip>> {
        self.lock().inputs.get(i).cloned()
    }

    pub fn bus_at(&self, i: usize) -> Option<Arc<Strip>> {
        self.lock().buses.get(i).cloned()
    }

    pub fn bus_in_slot(&self, slot: i32) -> Option<Arc<Strip>> {
        self.lock().buses.iter().find(|b| b.id() == slot).cloned()
    }

    pub fn total_dsp_faults(&self) -> i32 {
        let st = self.lock();
        std::iter::once(&st.master).chain(&st.inputs).chain(&st.buses).map(|s| s.meters.dsp_faults.load(Relaxed)).sum()
    }

    pub fn num_faulted_plugins(&self) -> usize {
        let st = self.lock();
        std::iter::once(&st.master).chain(&st.inputs).chain(&st.buses).map(|s| s.inserts.num_faulted()).sum()
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

        for s in std::iter::once(&st.master).chain(&st.inputs).chain(&st.buses) {
            s.prepare(sample_rate, block_size);
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

            self.process_chunk(topo, a, n);
            start += n;
        }
    }

    unsafe fn process_chunk(&self, topo: &Topology, a: &mut AudioState, n: usize) {
        // 1. Inputs (parallel)
        let hw = SharedPtrs(&a.hw_in_ptrs);
        let hw = &hw;
        self.pool.parallel_for(topo.inputs.len(), &|i| {
            let s = &topo.inputs[i];
            s.read_hardware_inputs(hw.get(), n);
            s.process(n);
        });

        // 2. Buses (parallel; each bus owns the send ramps feeding it)
        self.pool.parallel_for(topo.buses.len(), &|i| {
            let bus = &topo.buses[i];
            sum_sends_into_bus(bus, &topo.inputs, n);
            bus.process(n);
        });

        // 3. Master
        sum_master(topo, n);

        // 4. Solo / monitor
        self.build_monitor(topo, a, n);

        // 5. Outputs, then the safety clip
        self.write_outputs(topo, a, n);
        self.protect_outputs(a, n);
    }

    unsafe fn build_monitor(&self, topo: &Topology, a: &mut AudioState, n: usize) {
        let solo = topo.inputs.iter().chain(&topo.buses).any(|s| s.params.solo.load(Relaxed));
        self.controls.any_solo_active.store(solo, Relaxed);
        let afl = self.controls.solo_afl.load(Relaxed);

        let [m0, m1] = &mut a.monitor;
        let (m0, m1) = (&mut m0[..n], &mut m1[..n]);
        m0.fill(0.0);
        m1.fill(0.0);

        for s in topo.inputs.iter().chain(&topo.buses) {
            let target = if solo && s.params.solo.load(Relaxed) { 1.0 } else { 0.0 };
            let last = s.last_solo();
            if target != 0.0 || *last != 0.0 {
                let d = s.dsp();
                if afl {
                    add_with_gain_ramp(m0, d.main_out(0, n), *last, target);
                    add_with_gain_ramp(m1, d.main_out(1, n), *last, target);
                } else {
                    let right = if d.channels() == 2 { 1 } else { 0 };
                    add_with_gain_ramp(m0, d.post_chain(0, n), *last, target);
                    add_with_gain_ramp(m1, d.post_chain(right, n), *last, target);
                }
            }
            *last = target;
        }

        // With nothing soloed the monitor follows the master.
        let master_target = if solo { 0.0 } else { 1.0 };
        let md = topo.master.dsp();
        add_with_gain_ramp(m0, md.main_out(0, n), a.last_master_to_monitor, master_target);
        add_with_gain_ramp(m1, md.main_out(1, n), a.last_master_to_monitor, master_target);
        a.last_master_to_monitor = master_target;

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

        let write_strip = |s: &Strip, direct_out: bool| {
            let l = s.params.output_left.load(Relaxed);
            let r = s.params.output_right.load(Relaxed);
            if l < 0 && r < 0 {
                return;
            }
            let d = s.dsp();
            if direct_out {
                // Input direct outs carry the processed signal before the fader.
                add_to(l, d.post_chain(0, n));
                add_to(r, d.post_chain(if d.channels() > 1 { 1 } else { 0 }, n));
            } else if d.channels() == 1 {
                add_to(l, d.post_fader(0, n));
                add_to(r, d.post_fader(0, n));
            } else {
                add_to(l, d.main_out(0, n));
                add_to(r, d.main_out(1, n));
            }
        };

        write_strip(&topo.master, false);
        topo.buses.iter().for_each(|s| write_strip(s, false));
        topo.inputs.iter().filter(|s| !s.dsp().idle()).for_each(|s| write_strip(s, true));

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

/// # Safety
/// Bus task only, after stage 1.
unsafe fn sum_sends_into_bus(bus: &Strip, inputs: &[Arc<Strip>], n: usize) {
    bus.begin_summed_block(n);
    let slot = bus.id() as usize;
    let bd = bus.dsp_mut();
    let bus_stereo = bd.channels() == 2;

    for input in inputs {
        let Some(send) = input.sends.find(slot) else { continue };
        let target = if send.enabled.load(Relaxed) { db_to_gain(send.level_db.load()) } else { 0.0 };
        let last = &mut *send.last_gain.get();
        let src = input.dsp();

        if (target == 0.0 && *last == 0.0) || src.idle() {
            *last = target; // silent strips have nothing to add
            continue;
        }

        let pre = send.pre_fader.load(Relaxed);
        let tap = |ch: usize| if pre { src.pre_fader(ch, n) } else { src.post_fader(ch, n) };
        let src_stereo = src.channels() == 2;

        if bus_stereo {
            // Sends follow the channel pan/balance into stereo buses.
            let (l, r) = src.pan();
            add_with_gain_ramp(bd.work_mut(0, n), tap(0), *last * l, target * l);
            add_with_gain_ramp(bd.work_mut(1, n), tap(if src_stereo { 1 } else { 0 }), *last * r, target * r);
        } else if src_stereo {
            add_with_gain_ramp(bd.work_mut(0, n), tap(0), *last * 0.5, target * 0.5);
            add_with_gain_ramp(bd.work_mut(0, n), tap(1), *last * 0.5, target * 0.5);
        } else {
            add_with_gain_ramp(bd.work_mut(0, n), tap(0), *last, target);
        }
        *last = target;
    }
}

/// # Safety
/// Audio thread, after stage 2.
unsafe fn sum_master(topo: &Topology, n: usize) {
    let master = &topo.master;
    master.begin_summed_block(n);
    let md = master.dsp_mut();

    for s in topo.inputs.iter().chain(&topo.buses) {
        let target = if s.params.main_assign.load(Relaxed) { 1.0 } else { 0.0 };
        let last = s.last_main_assign();
        let d = s.dsp();
        if d.idle() {
            *last = target;
            continue;
        }
        if target != 0.0 || *last != 0.0 {
            add_with_gain_ramp(md.work_mut(0, n), d.main_out(0, n), *last, target);
            add_with_gain_ramp(md.work_mut(1, n), d.main_out(1, n), *last, target);
        }
        *last = target;
    }

    master.process(n);
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
