//! The live mixing engine, laid out like a WING: input channels and aux
//! channels feed 16 (or more) stereo buses, 4 stereo mains and 8 stereo
//! matrices; DCAs and mute groups act on any strip; 16 FX slots serve the
//! strips' insert points.
//!
//! Per audio block:
//!  1. channels + aux channels read their inputs and run their chains (parallel;
//!     in waves when keys come from other channels: `keytap.rs`)
//!  2. automix (gain sharing between channels in automix groups X/Y)
//!  3. buses 9+ sum the input sends feeding them                       (parallel)
//!  4. buses 1-8 sum input sends and sends from buses 9+               (parallel)
//!  5. mains sum channel/aux/bus main sends                            (parallel)
//!  6. matrices sum matrix sends from all strips + their direct input  (parallel)
//!  7. solo (PFL/AFL per strip type, routed by the SOLO MODE) builds the solo
//!     buses of the two monitor paths (SPEAKERS and PHONES), each with
//!     its SOURCE (Main 1 by default) and DIRECT IN; then each path's EQ,
//!     PAN / WIDTH, INVERT, DELAY, level, DIM, mute, MONO, SWAP and LIMITER
//!     (`monitor.rs`)
//!  8. outputs are written, then safety-clipped
//!
//! Plugin delay compensation (`pdc.rs`): each summing stage reads its sources
//! delayed so that every path arrives in time; input channels write their
//! compensation rings after stage 2, the others right after they ran.
//!
//! Before stage 1, the sources (`sources.rs`) process the input ports the
//! channels read, and the console works out the block's solos and mutes
//! (`solo.rs`: SIP, MUTEGROUP/SIP OVERRIDE, talk tags) and its talkback
//! (`talk.rs`), which the bus, main and matrix stages add to their sums.
//!
//! The recorder (`livedaw-recorder`) runs around every chunk: it may shorten
//! the chunk (loop end, playback ring switch) so each chunk is contiguous on
//! the timeline, fills the 64 playback channels (input sources
//! `PLAYBACK_SOURCE_BASE + n`, like hardware inputs) before stage 1, records
//! hardware inputs at the start of the chunk and strip taps after stage 6.
//! The USB recorder / player (`livedaw_recorder::usb`) runs inside the same
//! chunks: its four player channels are sources (USB PLAYER) like the
//! playback channels, and its four RECORDER ports tap strips or sources
//! after stage 6 (the "USB deck hooks" below).
//!
//! Structure changes happen on the message thread by publishing a new
//! immutable `Topology`; the old one is retired to the garbage collector, which
//! drops it (and any removed strips) once the audio thread is done with it.

use crate::analyser::{rta_tap, Analyser, Probes};
use crate::denormals::ScopedNoDenormals;
use crate::fx::{FxRack, InsertCallbacks};
use crate::garbage::Garbage;
use crate::keytap::{key_sources, key_tap, KeySchedule, KeySource};
use crate::monitor::{direct_source, MonitorPathDsp, SoloPatch, BAND_SOLO_MAX_DB};
use crate::params::*;
use crate::pdc::{pdc_tap, PdcState, MAX_PDC};
use crate::pool::WorkerPool;
use crate::sends::{send_mode, MAX_BUSES};
use crate::solo::{self, solo_mode, SoloBlock};
use crate::sources::{SourceDsp, SourceTable};
use crate::strip::{source_ptr, KeyStrips, ProcessContext, Strip};
use crate::talk::{TalkBlock, TalkGains, TalkRamps};
use livedaw_dsp::util::*;
use livedaw_dsp::wing::fx::{allowed_in_slot, FxContext, FxDevice};
use livedaw_dsp::wing::FX_MODELS;
use livedaw_recorder::usb::{RecTap, UsbDeck, USB_CHANNELS};
use livedaw_recorder::{playback_channel, usb_player_channel, RecordStream, RecordTrackSpec, Recorder, SourceKind};
use std::cell::UnsafeCell;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::atomic::{AtomicPtr, AtomicU32, Ordering::*};
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::Instant;

#[derive(Default)]
struct Topology {
    channels: Vec<Arc<Strip>>,
    auxes: Vec<Arc<Strip>>,
    /// Audio thread scratch: the non-idle inputs of the current block (capacity
    /// reserved at publish, so pushing never allocates).
    active_inputs: UnsafeCell<Vec<Arc<Strip>>>,
    /// Audio thread scratch: stage 1's waves by key dependency (`keytap.rs`).
    key_schedule: UnsafeCell<KeySchedule>,
    /// Buses that can feed buses 1-8 (slot >= 8).
    upper_buses: Vec<Arc<Strip>>,
    /// Buses 1-8 (slot < 8).
    lower_buses: Vec<Arc<Strip>>,
    mains: Vec<Arc<Strip>>,
    matrices: Vec<Arc<Strip>>,
}

// SAFETY: `active_inputs` and `key_schedule` are only touched by the audio
// thread (between stages, never while tasks run); everything else is immutable
// after publishing.
unsafe impl Sync for Topology {}
unsafe impl Send for Topology {}

impl Topology {
    fn inputs(&self) -> impl Iterator<Item = &Arc<Strip>> + Clone {
        self.channels.iter().chain(&self.auxes)
    }

    /// Inputs that produced signal this block (valid after stage 1).
    unsafe fn active(&self) -> &[Arc<Strip>] {
        &*self.active_inputs.get()
    }
    fn buses(&self) -> impl Iterator<Item = &Arc<Strip>> + Clone {
        self.lower_buses.iter().chain(&self.upper_buses)
    }
    fn all(&self) -> impl Iterator<Item = &Arc<Strip>> + Clone {
        self.inputs().chain(self.buses()).chain(&self.mains).chain(&self.matrices)
    }

    /// A strip by kind and number as the console shows it ("CH 3", "BUS 12").
    fn by_number(&self, kind: StripKind, number: i32) -> Option<&Arc<Strip>> {
        let i = usize::try_from(number.checked_sub(1)?).ok()?;
        match kind {
            StripKind::Channel => self.channels.get(i),
            StripKind::Aux => self.auxes.get(i),
            StripKind::Bus => self.buses().find(|b| b.id() as usize == i),
            StripKind::Main => self.mains.get(i),
            StripKind::Matrix => self.matrices.get(i),
        }
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
            // The WING's factory names: DCAs unnamed, mute groups MGRP.n.
            dcas: std::array::from_fn(|_| String::new()),
            mute_groups: std::array::from_fn(|i| format!("MGRP.{}", i + 1)),
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
    /// The monitor paths SPEAKERS (0) and PHONES (1), left and right.
    monitor: [[Vec<f32>; 2]; 2],
    monitor_ramps: [MonitorRamps; 2],
    /// Each path's EQ, image, delay and limiter, DIRECT IN and BAND SOLO ramps.
    monitor_dsp: [MonitorPathDsp; 2],
    /// The sources' processing and the input pointers the channels read.
    sources: SourceDsp,
    block_counter: u64,
}

/// A monitor path's ramps from the last block.
#[derive(Clone, Copy)]
struct MonitorRamps {
    level: f32,
    /// MONO / SWAP: 2x2 matrix (ll, rl, lr, rr).
    matrix: [f32; 4],
    /// The SOURCE's gain, and which source played.
    source: f32,
    source_index: i32,
}

impl Default for MonitorRamps {
    fn default() -> Self {
        Self { level: 0.0, matrix: MONITOR_STEREO, source: 1.0, source_index: 0 }
    }
}

/// Monitor matrices (out_l = m[0]*L + m[1]*R, out_r = m[2]*L + m[3]*R).
const MONITOR_STEREO: [f32; 4] = [1.0, 0.0, 0.0, 1.0];
const MONITOR_MONO: [f32; 4] = [0.5, 0.5, 0.5, 0.5];
const MONITOR_SWAP: [f32; 4] = [0.0, 1.0, 1.0, 0.0];

/// Console-wide group settings read once per block.
struct GroupState<'a> {
    mute_groups: u32,
    dca_mute_groups: bool,
    main_link_last: usize,
    main_link_db: f32,
    /// SOURCE SOLO: the host strip and the soloed source's ports.
    source_solo: Option<(&'a Strip, SoloPatch)>,
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
    recorder: Recorder,
    /// The USB recorder / player (USB PLAYER sources, RECORDER outputs).
    usb: UsbDeck,
    /// RTA, LR scope and LISTEN on one strip at a time (`analyser.rs`).
    analysis: Analyser,
    /// ROUTING > SOURCES.
    sources: SourceTable,
    /// Talkback's ramps per destination (owned by the destination tasks).
    talk_ramps: TalkRamps,
    /// DCAs named MON A / MON B / MON LVL: bit n = DCA n+1 controls the
    /// SPEAKERS path, bit 16 + n the PHONES path.
    dca_monitor: AtomicU32,
    /// Plugin delay compensation: the setting and the plan's status.
    pdc: PdcState,
}

// SAFETY: `audio` is only touched by the audio thread (process) or by prepare
// while the device is stopped; `structure`/`names` are behind mutexes the audio
// thread never takes; everything else is atomic or internally synchronised.
unsafe impl Sync for Engine {}
unsafe impl Send for Engine {}

const DEFAULT_SAMPLE_RATE: f64 = 48000.0;
const DEFAULT_BLOCK: usize = 512;

/// The monitor paths a DCA name controls (bit 0 = SPEAKERS, bit 1 = PHONES):
/// "MON A" (PHONES, path 1 on the WING), "MON B" (SPEAKERS), "MON LVL" (both),
/// ignoring case, spaces and dots.
pub fn monitor_dca_paths(name: &str) -> u32 {
    let key: String = name.chars().filter(|c| !matches!(c, ' ' | '.' | '_' | '-')).collect::<String>().to_ascii_uppercase();
    match key.as_str() {
        "MONA" => 2,
        "MONB" => 1,
        "MONLVL" => 3,
        _ => 0,
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
                monitor: std::array::from_fn(|_| [vec![0.0; DEFAULT_BLOCK], vec![0.0; DEFAULT_BLOCK]]),
                monitor_ramps: Default::default(),
                monitor_dsp: Default::default(),
                sources: SourceDsp::default(),
                block_counter: 0,
            }),
            controls: EngineControls::default(),
            fx: (0..NUM_FX).map(|_| FxRack::default()).collect(),
            recorder: Recorder::new(),
            usb: UsbDeck::new(),
            analysis: Analyser::new(),
            sources: SourceTable::default(),
            talk_ramps: TalkRamps::default(),
            dca_monitor: AtomicU32::new(0),
            pdc: PdcState::default(),
        };
        // SAFETY: not published yet, no audio callback.
        unsafe {
            for f in engine.fx.iter() {
                f.prepare(DEFAULT_BLOCK);
            }
            (*engine.audio.get()).sources.prepare(0, DEFAULT_BLOCK, DEFAULT_SAMPLE_RATE);
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

    fn new_strip(&self, st: &Structure, kind: StripKind, id: i32) -> Arc<Strip> {
        // Unnamed, as on the WING (the screens show "CH 3" and so on).
        let s = Arc::new(Strip::new(kind, id));
        // SAFETY: not published yet.
        unsafe { s.prepare(st.device.sample_rate, st.device.block_size) };
        s
    }

    /// Aux channels, the WING's 16 buses, 4 mains and 8 matrices.
    fn build_fixed(&self, st: &mut Structure) {
        st.auxes = (0..NUM_AUX).map(|i| self.new_strip(st, StripKind::Aux, i as i32)).collect();
        st.buses = (0..WING_BUSES).map(|i| self.new_strip(st, StripKind::Bus, i as i32)).collect();
        st.mains = (0..NUM_MAINS).map(|i| self.new_strip(st, StripKind::Main, i as i32)).collect();
        st.matrices = (0..NUM_MATRICES).map(|i| self.new_strip(st, StripKind::Matrix, i as i32)).collect();
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
            key_schedule: UnsafeCell::new(KeySchedule::new(st.channels.len() + st.auxes.len())),
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

        let strip = self.new_strip(st, StripKind::Channel, id);
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
                send.reset(s.sends.default_mode(slot));
            }
        }

        let strip = self.new_strip(&st, StripKind::Bus, slot as i32);
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
        self.analysis.reset();
        self.sources.reset();
        let mut names = self.names.lock().unwrap_or_else(|e| e.into_inner());
        *names = GroupNames::default();
        self.update_monitor_dcas(&names);
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

    /// A strip by kind and number as the console shows it: channels, aux,
    /// mains and matrices by position ("CH 3" = 3), buses by slot ("BUS 12" = 12).
    pub fn strip_by_number(&self, kind: StripKind, number: i32) -> Option<Arc<Strip>> {
        let i = usize::try_from(number.checked_sub(1)?).ok()?;
        let st = self.lock();
        match kind {
            StripKind::Bus => st.buses.iter().find(|b| b.id() as usize == i).cloned(),
            kind => st.list(kind).get(i).cloned(),
        }
    }

    /// The multitrack recorder / player.
    pub fn recorder(&self) -> &Recorder {
        &self.recorder
    }

    /// The USB recorder / player.
    pub fn usb(&self) -> &UsbDeck {
        &self.usb
    }

    /// The RTA, the LR vector scope and LISTEN (their controls are shared with the UI).
    pub fn analysis(&self) -> &Analyser {
        &self.analysis
    }

    /// Message thread: starts recording `tracks` from the current position
    /// (see [`Recorder::record`]) after checking that their sources exist.
    pub fn record(&self, tracks: &[RecordTrackSpec]) -> Result<(), String> {
        let num_hw_inputs = self.lock().device.num_hw_inputs;
        for t in tracks {
            let src = &t.source;
            if src.kind == SourceKind::HardwareInput {
                for port in src.port..src.port + t.num_channels.clamp(1, 2) as i32 {
                    // Playback channels, the USB PLAYER and the oscillators always exist.
                    let internal = playback_channel(port).is_some()
                        || usb_player_channel(port).is_some()
                        || crate::sources::osc_channel(port).is_some();
                    if !internal && usize::try_from(port).map_or(true, |p| p >= num_hw_inputs) {
                        return Err(format!("Input {} doesn't exist", port + 1));
                    }
                }
            } else {
                let kind = StripKind::from_i32(src.strip_kind).ok_or_else(|| "Unknown strip kind".to_owned())?;
                if self.strip_by_number(kind, src.strip_number).is_none() {
                    return Err(format!("{} {} doesn't exist", kind.label(), src.strip_number));
                }
            }
        }
        self.recorder.record(tracks)
    }

    /// Main 1 (the default monitor source).
    pub fn main(&self, i: usize) -> Option<Arc<Strip>> {
        self.strip_at(StripKind::Main, i)
    }

    /// STARTUP MAIN MUTE: the application calls this once at start-up (after
    /// loading its show). Mutes every main if the preference is on; returns
    /// whether it did.
    pub fn apply_startup_main_mute(&self) -> bool {
        if !self.controls.startup_main_mute.load(Relaxed) {
            return false;
        }
        for m in self.strips(StripKind::Main) {
            m.params.mute.store(true, Relaxed);
        }
        true
    }

    pub fn dca_name(&self, i: usize) -> String {
        self.names.lock().unwrap_or_else(|e| e.into_inner()).dcas.get(i).cloned().unwrap_or_default()
    }

    pub fn set_dca_name(&self, i: usize, name: &str) {
        let mut names = self.names.lock().unwrap_or_else(|e| e.into_inner());
        if let Some(n) = names.dcas.get_mut(i) {
            *n = name.to_owned();
        }
        self.update_monitor_dcas(&names);
    }

    /// DCAs named MON A, MON B or MON LVL control the monitor level of the
    /// PHONES, the SPEAKERS or both, as on the WING (firmware 1.09).
    fn update_monitor_dcas(&self, names: &GroupNames) {
        let roles = names.dcas.iter().enumerate().fold(0u32, |roles, (i, name)| {
            let paths = monitor_dca_paths(name);
            roles | (paths & 1) << i | (paths >> 1) << (16 + i)
        });
        self.dca_monitor.store(roles, Relaxed);
    }

    /// The monitor paths (bit 0 = SPEAKERS, bit 1 = PHONES) whose level DCA `i` controls.
    pub fn dca_monitor_paths(&self, i: usize) -> u32 {
        let roles = self.dca_monitor.load(Relaxed);
        if i >= NUM_DCAS {
            return 0;
        }
        (roles >> i) & 1 | ((roles >> (16 + i)) & 1) << 1
    }

    /// The level offset (dB) and mute the MON DCAs give monitor path `path`.
    fn monitor_dca(&self, path: usize) -> (f32, bool) {
        let roles = self.dca_monitor.load(Relaxed) >> (16 * path);
        if roles & 0xffff == 0 {
            return (0.0, false);
        }
        self.controls
            .dcas
            .iter()
            .enumerate()
            .filter(|(i, _)| roles & (1 << i) != 0)
            .fold((0.0, false), |(db, muted), (_, d)| (db + d.fader_db.load(), muted || d.mute.load(Relaxed)))
    }

    /// ROUTING > SOURCES: every input port's source.
    pub fn sources(&self) -> &SourceTable {
        &self.sources
    }

    /// Message thread: MONO / STEREO / MID-SIDE on ROUTING > SOURCES. The
    /// channels and aux patched to the pair's first source follow it, as on the
    /// console: stereo on the pair for STEREO and MID/SIDE, mono on the first
    /// source for MONO (MAIN and ALT inputs alike).
    pub fn set_source_mode(&self, index: usize, mode: i32) {
        self.sources.set_mode(index, mode);
        let first = crate::sources::pair_first(index);
        let Some(port) = crate::sources::source_port(first) else { return };
        let stereo = self.sources.params().get(first).is_some_and(|p| p.mode.load(Relaxed) != crate::sources::source_mode::MONO);
        for s in self.strips(StripKind::Channel).iter().chain(&self.strips(StripKind::Aux)) {
            let p = &s.params;
            if p.input_left.load(Relaxed) == port {
                p.stereo.store(stereo, Relaxed);
                p.input_right.store(if stereo { port + 1 } else { -1 }, Relaxed);
            }
            if p.alt_input_left.load(Relaxed) == port {
                p.alt_input_right.store(if stereo { port + 1 } else { -1 }, Relaxed);
            }
        }
    }

    /// Message thread: the MUTE key of a strip, with MUTEGROUP/SIP OVERRIDE
    /// (see [`solo::press_mute`]).
    pub fn press_mute(&self, s: &Strip) {
        solo::press_mute(&self.controls, s);
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

    /// Message thread: frees what the audio thread can no longer see, and
    /// creates the devices of FX slots whose model was changed directly through
    /// the shared parameters (e.g. by a recall).
    pub fn collect(&self) {
        self.sync_fx_devices();
        self.update_pdc();
        self.update_keys();
        self.garbage.collect();
    }

    // =========================================================================
    // Plugin delay compensation (message thread)

    /// The compensation setting (DELAY COMPENSATION) and the plan's status.
    pub fn pdc(&self) -> &PdcState {
        &self.pdc
    }

    /// Message thread: works out the compensation plan from the plugins'
    /// latencies, the inserts and the sends (`pdc.rs`), publishes it and
    /// allocates the history rings it reads. Stage by stage as the audio
    /// thread sums: every summing strip aligns its sources to the latest one,
    /// and its own taps carry that latency on. Called by `collect`.
    pub fn update_pdc(&self) {
        let st = self.lock();
        let enabled = self.pdc.enabled.load(Relaxed);
        let fx: [u32; NUM_FX] =
            std::array::from_fn(|i| if enabled { self.fx[i].effective_latency(&self.controls.fx[i]) } else { 0 });
        let add = |own: [u32; pdc_tap::COUNT], base: u32| own.map(|l| l.saturating_add(base));

        for s in st.channels.iter().chain(&st.auxes) {
            s.pdc.set(s.own_latency(&fx), 0);
        }
        // Buses 9+ feed buses 1-8, so they come first; then mains, matrices.
        let (lower, upper): (Vec<&Arc<Strip>>, Vec<&Arc<Strip>>) =
            st.buses.iter().partition(|b| (b.id() as usize) < BUS_TO_BUS_TARGETS);
        let summing: Vec<&Arc<Strip>> = upper.into_iter().chain(lower).chain(&st.mains).chain(&st.matrices).collect();
        for dst in &summing {
            let target = pdc_feeds(&st, dst).iter().map(|(s, tap)| s.pdc.latency(*tap)).max().unwrap_or(0);
            dst.pdc.set(add(dst.own_latency(&fx), target), target);
        }

        // The rings the plan reads from, and what it can't make up.
        let mut needs_delay = false;
        let mut limited = false;
        for dst in &summing {
            let target = dst.pdc.target();
            for (s, tap) in pdc_feeds(&st, dst) {
                let delay = target.saturating_sub(s.pdc.latency(tap)) as usize;
                if delay > 0 {
                    needs_delay = true;
                    limited |= delay > MAX_PDC;
                    s.pdc.ensure_ring(tap, st.device.block_size);
                }
            }
        }
        if !needs_delay {
            for s in st.all() {
                s.pdc.release_rings(&self.garbage);
            }
        }
        let total = st.all().map(|s| s.pdc.latency(pdc_tap::OUT)).max().unwrap_or(0);
        self.pdc.total.store(total, Relaxed);
        self.pdc.limited.store(limited, Relaxed);
    }

    /// Message thread: gives every strip a buffer for each tap point some
    /// GATE or DYNAMICS key takes from it (KEY SOURCE + KEY TAP, whether the
    /// slot is on or not), and retires the others (`keytap.rs`). Called by
    /// `collect`.
    pub fn update_keys(&self) {
        let st = self.lock();
        let mut masks: Vec<(&Arc<Strip>, u32)> = st.all().map(|s| (s, 0)).collect();
        for s in st.all() {
            let p = &s.params;
            // The gate runs on input channels only.
            let slots: &[&DynSlotParams] = if s.kind() == StripKind::Channel { &[&p.gate, &p.dynamics] } else { &[&p.dynamics] };
            for slot in slots {
                let (source, buffer) = key_tap(slot);
                let src = match source {
                    KeySource::SelfKey => None,
                    KeySource::Channel(i) => st.channels.get(i),
                    KeySource::Strip(kind, i) => st.list(kind).iter().find(|t| t.id() as usize == i),
                };
                if let Some(m) = src.and_then(|src| masks.iter_mut().find(|(t, _)| Arc::ptr_eq(t, src))) {
                    m.1 |= 1 << buffer;
                }
            }
        }
        for (s, mask) in masks {
            s.keys.plan(mask, st.device.block_size, &self.garbage);
        }
    }

    /// Message thread: selects the device of FX slot `i` (an index into
    /// `FX_MODELS`), creating it now. With `defaults`, its settings are reset to
    /// the device's defaults. Premium devices are refused in slots 9-16.
    pub fn set_fx_model(&self, i: usize, model: usize, defaults: bool) -> bool {
        let (Some(rack), Some(params)) = (self.fx.get(i), self.controls.fx.get(i)) else { return false };
        if model >= FX_MODELS.len() || !allowed_in_slot(model, i) {
            return false;
        }
        params.set_model(model, defaults);
        self.install_fx_device(i, rack, model);
        true
    }

    /// Message thread: installs (or removes, with None) the plugin of FX slot
    /// `i`. Installing a plugin makes the slot an EXT slot; removing it empties
    /// an EXT slot.
    pub fn set_fx_plugin(&self, i: usize, cb: Option<InsertCallbacks>) {
        let (Some(rack), Some(params)) = (self.fx.get(i), self.controls.fx.get(i)) else { return };
        rack.set(cb, &self.garbage);
        if cb.is_some() {
            self.set_fx_model(i, FX_EXT, true);
        } else if params.model_index() == FX_EXT {
            self.set_fx_model(i, FX_NONE, true);
        }
    }

    /// Message thread: as `set_fx_plugin` with a processor holding the same
    /// plugins as the slot's current one (a plugin chain rearranged): an
    /// auto-bypassed slot stays faulted.
    pub fn replace_fx_plugin(&self, i: usize, cb: InsertCallbacks) {
        let (Some(rack), Some(params)) = (self.fx.get(i), self.controls.fx.get(i)) else { return };
        rack.replace(cb, &self.garbage);
        if params.model_index() != FX_EXT {
            self.set_fx_model(i, FX_EXT, true); // (selecting a model clears the fault)
        }
    }

    fn fx_context(&self) -> FxContext {
        let st = self.lock();
        FxContext { sample_rate: st.device.sample_rate as f32, max_block: st.device.block_size }
    }

    /// Creates (or removes) the built-in device of slot `i` for `model`.
    fn install_fx_device(&self, i: usize, rack: &FxRack, model: usize) {
        let ctx = self.fx_context();
        if rack.device_info() == Some((model, ctx)) {
            return;
        }
        let device = if model == FX_NONE || model == FX_EXT || !allowed_in_slot(model, i) {
            None
        } else {
            FxDevice::new(model, ctx).map(Box::new)
        };
        rack.set_device(device, &self.garbage);
    }

    fn sync_fx_devices(&self) {
        for (i, (rack, params)) in self.fx.iter().zip(self.controls.fx.iter()).enumerate() {
            let model = params.model_index();
            let wanted = !(model == FX_NONE || model == FX_EXT);
            if rack.device_info().map(|(m, _)| m) != wanted.then_some(model) {
                self.install_fx_device(i, rack, model);
            }
        }
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
        a.monitor = std::array::from_fn(|_| [vec![0.0; block_size], vec![0.0; block_size]]);
        for d in &mut a.monitor_dsp {
            d.prepare(sample_rate, block_size);
        }
        a.sources.prepare(in_map.len(), block_size, sample_rate);

        for s in st.all() {
            s.prepare(sample_rate, block_size);
        }
        for f in self.fx.iter() {
            f.prepare(block_size);
        }
        drop(st);
        self.update_keys();
        self.recorder.prepare(sample_rate, block_size);
        self.usb.prepare(sample_rate, block_size);
        self.analysis.prepare(sample_rate);
        // Devices are built for a sample rate and block size: rebuild them.
        for (i, (rack, params)) in self.fx.iter().zip(self.controls.fx.iter()).enumerate() {
            self.install_fx_device(i, rack, params.model_index());
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

            // The recorder may shorten the chunk (loop end, ring switch).
            let n = self.recorder.begin_chunk(n);
            self.usb.begin_chunk(n); // USB deck hook: the player buffers, the recorder's commands
            a.block_counter += 1;
            self.process_chunk(topo, a, n);
            self.recorder.end_chunk(n);
            self.usb.end_chunk(n); // USB deck hook
            start += n;
        }
    }

    unsafe fn process_chunk(&self, topo: &Topology, a: &mut AudioState, n: usize) {
        let rec = &self.recorder;
        // Sources first: the channels and the recorder's input streams read
        // the processed ports (gain, mute, polarity, stereo / mid-side pairs).
        a.sources.process(
            &self.sources,
            &a.hw_in_ptrs,
            rec.playback_ptrs(),
            self.usb.player_ptrs(),
            n,
            self.controls.mute_groups.load(Relaxed),
        );
        let streams = rec.record_streams();
        // Record hardware inputs (and playback channels) as they arrive.
        self.record_inputs(streams, a.sources.hw_ptrs(), a.sources.playback_ptrs(), n);

        // Talkback and the console's solos and mutes for this block.
        let talk = TalkBlock::new(&self.controls.talk, |kind, i| topo.by_number(kind, i as i32 + 1).map(|s| s.as_ref()));
        // SOURCE SOLO: the host plays the soloed source, and solos (monitor.rs).
        let m = &self.controls.monitor;
        let source_solo = m.host().and_then(|(kind, i)| topo.by_number(kind, i as i32 + 1)).and_then(|host| {
            m.source_solo_patch(&self.sources, host.params.solo.load(Relaxed)).map(|patch| (host.as_ref(), patch))
        });
        let solo = solo::console_pass(&self.controls, topo.all(), talk.active, source_solo.map(|(s, _)| s));
        let talk = &talk;

        let hw = SharedPtrs(a.sources.hw_ptrs());
        let hw = &hw;
        let playback = SharedPtrs(a.sources.playback_ptrs());
        let playback = &playback;
        let alt = (self.controls.input_select_global.load(Relaxed), self.controls.global_alt.load(Relaxed));
        let groups = GroupState {
            mute_groups: self.controls.mute_groups.load(Relaxed),
            dca_mute_groups: self.controls.dca_mute_groups.load(Relaxed),
            main_link_last: self.controls.main_link_last(),
            main_link_db: topo.mains.first().map_or(0.0, |m| m.params.fader_db.load()),
            source_solo,
        };
        let block = a.block_counter;
        // The analysed and listened strips of this chunk (analyser.rs).
        let probes = self.analysis.begin_block(topo.all());
        let probes = &probes;

        // 1. Channels and aux channels (parallel), in waves: a strip keyed
        // from another channel runs after it (keytap.rs).
        let num_ch = topo.channels.len();
        let stage1 = |i: usize| if i < num_ch { &topo.channels[i] } else { &topo.auxes[i - num_ch] };
        (*topo.key_schedule.get()).compute(num_ch, |i| {
            let s = stage1(i);
            key_sources(&s.params, s.kind() == StripKind::Channel)
        });
        let schedule = &*topo.key_schedule.get();
        let ctx_of = |hw, wave| self.context(topo, hw, playback.get(), alt, &groups, block, probes, schedule.waves(), wave);
        let ctx_of = &ctx_of;
        for w in 0..schedule.num_waves() {
            let strips = schedule.wave(w);
            let strips = &strips;
            self.pool.parallel_for(strips.len(), &|j| {
                stage1(strips.get(j)).process(&ctx_of(hw.get(), Some(w as u8)), n);
            });
        }

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
        // The inputs' compensation rings, now that automix has scaled them.
        for s in topo.inputs() {
            s.write_pdc(n);
        }

        // 3./4. Buses: 9+ first (they may feed buses 1-8), then 1-8.
        let ramps = &self.talk_ramps;
        self.pool.parallel_for(topo.upper_buses.len(), &|i| {
            let bus = &topo.upper_buses[i];
            sum_bus(bus, topo.active().iter(), n);
            add_talk(bus, talk.bus(bus.id() as usize), ramps.get(StripKind::Bus, bus.id() as usize), talk, n);
            bus.process(&ctx_of(hw.get(), None), n);
            bus.write_pdc(n);
        });
        self.pool.parallel_for(topo.lower_buses.len(), &|i| {
            let bus = &topo.lower_buses[i];
            sum_bus(bus, topo.active().iter().chain(&topo.upper_buses), n);
            add_talk(bus, talk.bus(bus.id() as usize), ramps.get(StripKind::Bus, bus.id() as usize), talk, n);
            bus.process(&ctx_of(hw.get(), None), n);
            bus.write_pdc(n);
        });

        // 5. Mains
        self.pool.parallel_for(topo.mains.len(), &|i| {
            let main = &topo.mains[i];
            sum_main(main, i, topo.active().iter().chain(topo.buses()), n);
            add_talk(main, talk.main(i), ramps.get(StripKind::Main, i), talk, n);
            main.process(&ctx_of(hw.get(), None), n);
            main.write_pdc(n);
        });

        // 6. Matrices
        self.pool.parallel_for(topo.matrices.len(), &|i| {
            let mtx = &topo.matrices[i];
            sum_matrix(mtx, i, topo, n);
            add_talk(mtx, talk.matrix(i), ramps.get(StripKind::Matrix, i), talk, n);
            mtx.process(&ctx_of(hw.get(), None), n);
            mtx.write_pdc(n);
        });

        // Record strip taps, now that every strip has run.
        self.record_strips(topo, streams, n);
        self.record_usb(topo, a, n);

        // 7. Solo / monitor
        self.build_monitor(topo, a, n, solo, talk, probes);

        // 8. Outputs, then the safety clip
        self.write_outputs(topo, a, n);
        self.protect_outputs(a, n);
    }

    #[allow(clippy::too_many_arguments)]
    fn context<'a>(
        &'a self,
        topo: &'a Topology,
        hw: &'a [*const f32],
        playback: &'a [*const f32],
        (input_select_global, global_alt): (bool, bool),
        g: &GroupState<'a>,
        block: u64,
        probes: &'a Probes,
        key_waves: &'a [u8],
        wave: Option<u8>,
    ) -> ProcessContext<'a> {
        ProcessContext {
            fx: &self.fx,
            fx_params: &self.controls.fx,
            dcas: &self.controls.dcas,
            mute_groups: g.mute_groups,
            dca_mute_groups: g.dca_mute_groups,
            main_link_last: g.main_link_last,
            main_link_db: g.main_link_db,
            hw_inputs: hw,
            playback,
            input_select_global,
            global_alt,
            channels: &topo.channels,
            key_strips: KeyStrips {
                auxes: &topo.auxes,
                buses: [&topo.lower_buses, &topo.upper_buses],
                mains: &topo.mains,
                matrices: &topo.matrices,
            },
            key_waves,
            wave,
            block,
            probes,
            source_solo: g.source_solo,
        }
    }

    /// Records the hardware-input streams of this chunk (a port can also be a
    /// playback channel).
    unsafe fn record_inputs(&self, streams: &[RecordStream], hw: &[*const f32], playback: &[*const f32], n: usize) {
        for (i, st) in streams.iter().enumerate() {
            if st.source.kind == SourceKind::HardwareInput {
                let port = st.source.port + st.channel as i32;
                let src = source_ptr(hw, playback, port).map(|p| std::slice::from_raw_parts(p, n));
                self.recorder.write_record_stream(i, src, n);
            }
        }
    }

    /// Records the strip taps of this chunk: the pre-fader (PFL) tap, or the
    /// output after fader and pan. A strip that doesn't exist records silence.
    unsafe fn record_strips(&self, topo: &Topology, streams: &[RecordStream], n: usize) {
        for (i, st) in streams.iter().enumerate() {
            let pre = match st.source.kind {
                SourceKind::HardwareInput => continue,
                SourceKind::StripPreFader => true,
                SourceKind::StripPostFader => false,
            };
            let strip = StripKind::from_i32(st.source.strip_kind).and_then(|k| topo.by_number(k, st.source.strip_number));
            let src = strip.map(|s| if pre { s.dsp().pfl(st.channel, n) } else { s.dsp().out(st.channel, n) });
            self.recorder.write_record_stream(i, src, n);
        }
    }

    // ---- USB deck hooks -------------------------------------------------------------

    /// The RECORDER output group (`/io/out/REC`): each of the USB recorder's
    /// four ports records a source or a strip's output as its hardware output
    /// carries it (input and aux channels their direct out, the PFL copy;
    /// the others after fader and pan). Metered while not recording.
    unsafe fn record_usb(&self, topo: &Topology, a: &AudioState, n: usize) {
        let (hw, playback) = (a.sources.hw_ptrs(), a.sources.playback_ptrs());
        for port in 0..USB_CHANNELS {
            let src = match self.usb.rec_tap(port) {
                RecTap::Off => None,
                RecTap::Source(p) => source_ptr(hw, playback, p).map(|p| std::slice::from_raw_parts(p, n)),
                RecTap::Strip { kind, number, channel } => StripKind::from_i32(kind)
                    .and_then(|k| topo.by_number(k, number))
                    .map(|s| if s.kind().is_input() { s.dsp().pfl(channel, n) } else { s.dsp().out(channel, n) }),
            };
            self.usb.write_rec(port, src, n);
        }
    }

    /// Gain sharing between input channels whose INS 2 is set to AUTO_X /
    /// AUTO_Y, for the groups whose X / Y ENABLE is on. Channels leaving
    /// automix (or in a disabled group) glide back to unity.
    unsafe fn automix(&self, topo: &Topology, n: usize) {
        let enabled = [self.controls.automix_enable[0].load(Relaxed), self.controls.automix_enable[1].load(Relaxed)];
        let group_of = |s: &Strip| s.automix_group().filter(|&g| enabled[g]);
        let mut totals = [0.0f32; 2];
        for s in topo.active() {
            if let Some(g) = group_of(s) {
                totals[g] += s.dsp().automix_power();
            }
        }
        for s in topo.active() {
            let gain = match group_of(s) {
                // Power share -> gain; never below -40 dB so nobody vanishes completely.
                Some(g) if totals[g] > 1e-12 => (s.dsp().automix_power() / totals[g]).sqrt().max(0.01),
                // A silent group keeps its gains: the last active channel stays on.
                Some(_) => s.dsp().automix_gain(),
                None => 1.0,
            };
            if gain != 1.0 || s.dsp().automix_gain() != 1.0 {
                s.apply_automix(gain, n);
            }
        }
    }

    /// Stage 7: the two monitor paths, SPEAKERS and PHONES. Soloed
    /// strips go to the solo bus of the paths the SOLO MODE routes them to
    /// (PFL attenuated by the path's PFL DIM; a BAND SOLO boosted by its BAND
    /// SOLO); each path adds its SOURCE (at SRC MIX while a solo reaches it)
    /// and DIRECT IN, runs its EQ, PAN / WIDTH, INVERT and DELAY, then its
    /// level (with the MON DCAs), on the selected path(s) DIM, mute, the talk
    /// MON DIM, MONO and SWAP, and last its LIMITER (`monitor.rs`).
    unsafe fn build_monitor(
        &self,
        topo: &Topology,
        a: &mut AudioState,
        n: usize,
        solo: SoloBlock,
        talk: &TalkBlock,
        probes: &Probes,
    ) {
        let c = &self.controls;
        // LISTEN (band / key / xover solo) replaces the soloed strips (analyser.rs).
        let listen = self.analysis.listen_overrides_solo();
        let any_solo = solo.any_solo || listen;
        c.any_solo_active.store(any_solo, Relaxed);
        if !any_solo && c.solo_bus_override.load(Relaxed) {
            c.solo_bus_override.store(false, Relaxed);
        }
        let selected = solo::path_mask(c.solo_bus.load(Relaxed));
        let live = c.solo_mode.load(Relaxed) == solo_mode::LIVE && !c.solo_bus_override.load(Relaxed);
        let afl = StripKind::ALL.map(|k| c.solo_is_afl(k)); // indexed by kind
        let sources = [&c.speakers, &c.phones.source];
        let pfl_dim = sources.map(|s| db_to_gain(-s.pfl_dim_db.load().max(0.0)));

        for path in &mut a.monitor {
            for side in path.iter_mut() {
                side[..n].fill(0.0);
            }
        }

        // The solo buses.
        let mut solo_paths = 0;
        for s in topo.all() {
            let soloed = any_solo && !listen && s.console.is_soloed();
            let route = match (soloed, live) {
                (false, _) => 0,
                (true, true) => s.live_solo_paths(&c.dcas),
                (true, false) => selected,
            };
            solo_paths |= route;
            let is_afl = afl[s.kind() as usize];
            for (path, bus) in a.monitor.iter_mut().enumerate() {
                let target = if route & (1 << path) == 0 {
                    0.0
                } else if is_afl {
                    1.0
                } else {
                    pfl_dim[path]
                };
                let last = if path == 0 { *s.last_solo() } else { s.console.solo_ramp_b.load() };
                if target != 0.0 || last != 0.0 {
                    let [l, r] = bus;
                    add_with_gain_ramp(&mut l[..n], solo_tap(s, is_afl, 0, n), last, target);
                    add_with_gain_ramp(&mut r[..n], solo_tap(s, is_afl, 1, n), last, target);
                }
                if path == 0 {
                    *s.last_solo() = target;
                } else {
                    s.console.solo_ramp_b.store(target);
                }
            }
        }

        // LISTEN plays on the first selected path (the solo bus it replaces).
        // Called every block: it runs its own fades in and out.
        let listen_path = if selected & 1 != 0 { 0 } else { 1 };
        {
            let boost_db = c.monitor.paths[listen_path].band_solo_db.load();
            let boost = db_to_gain(if boost_db.is_finite() { boost_db.clamp(0.0, BAND_SOLO_MAX_DB) } else { 0.0 });
            let d = &mut a.monitor_dsp[listen_path];
            let [l, r] = &mut a.monitor[listen_path];
            self.analysis.add_listen(&mut l[..n], &mut r[..n], n, d.band_gain, boost);
            d.band_gain = boost;
        }
        if listen {
            solo_paths |= 1 << listen_path;
        }

        // Each path's SOURCE (Main 1 by default), crossfaded when it changes.
        for (path, src) in sources.iter().enumerate() {
            let index = src.source.load(Relaxed);
            let mut target = db_to_gain(src.source_level_db.load());
            if solo_paths & (1 << path) != 0 {
                target *= db_to_gain(src.source_mix_db.load());
            }
            let r = &mut a.monitor_ramps[path];
            let bus = &mut a.monitor[path];
            if index != r.source_index {
                add_monitor_source(topo, bus, r.source_index, r.source, 0.0, n);
                r.source = 0.0;
                r.source_index = index;
            }
            add_monitor_source(topo, bus, index, r.source, target, n);
            r.source = target;
        }

        // DIRECT IN: any input, aux, bus, main or matrix output at its level,
        // crossfaded when it changes.
        for (path, (p, d)) in c.monitor.paths.iter().zip(a.monitor_dsp.iter_mut()).enumerate() {
            let index = p.direct_source.load(Relaxed);
            let level = p.direct_level_db.load();
            let target = if direct_source(index).is_some() && level.is_finite() { db_to_gain(level) } else { 0.0 };
            let bus = &mut a.monitor[path];
            if index != d.direct_source {
                add_direct_in(topo, bus, d.direct_source, d.direct_gain, 0.0, n);
                d.direct_gain = 0.0;
                d.direct_source = index;
            }
            add_direct_in(topo, bus, index, d.direct_gain, target, n);
            d.direct_gain = target;
        }

        // Level (+ MON DCAs); DIM, mute, talk MON DIM, MONO and SWAP on the selected path(s).
        let (dim, mute) = (c.monitor_dim.load(Relaxed), c.monitor_mute.load(Relaxed));
        let (mono, swap) = (c.monitor_mono.load(Relaxed), c.monitor_swap.load(Relaxed));
        let talk_dim = talk.mon_dim();
        for path in 0..2 {
            let (level_db, dim_db, peak) = if path == 0 {
                (&c.monitor_level_db, &c.monitor_dim_db, &c.monitor_peak)
            } else {
                (&c.phones.level_db, &c.phones.dim_db, &c.phones.peak)
            };
            let (dca_db, dca_muted) = self.monitor_dca(path);
            let mut level = if dca_muted { 0.0 } else { db_to_gain(level_db.load() + dca_db) };
            let on_selected = selected & (1 << path) != 0;
            if on_selected {
                if dim {
                    level *= db_to_gain(-dim_db.load().max(0.0));
                }
                if mute {
                    level = 0.0;
                }
                level *= talk_dim;
            }
            let r = &mut a.monitor_ramps[path];
            let [l, rr] = &mut a.monitor[path];
            let (l, rr) = (&mut l[..n], &mut rr[..n]);
            // EQ, PAN / WIDTH, INVERT, DELAY; the RTA of MONITORS EQ reads
            // either side of the EQ.
            let dsp = &mut a.monitor_dsp[path];
            let mon = &c.monitor.paths[path];
            let rta = probes.monitor_rta(path);
            if rta == Some(rta_tap::MONITOR_PRE_EQ) {
                probes.push_rta([&*l, &*rr], 2, n);
            }
            dsp.process_eq(mon, l, rr);
            if rta == Some(rta_tap::MONITOR_POST_EQ) {
                probes.push_rta([&*l, &*rr], 2, n);
            }
            dsp.process_image(mon, l, rr);
            apply_gain_ramp(l, r.level, level);
            apply_gain_ramp(rr, r.level, level);
            r.level = level;

            // MONO (L+R)/2 on both sides, else SWAP L and R; crossfaded over a block.
            let matrix = match (on_selected && mono, on_selected && swap) {
                (true, _) => MONITOR_MONO,
                (false, true) => MONITOR_SWAP,
                _ => MONITOR_STEREO,
            };
            apply_monitor_matrix(l, rr, r.matrix, matrix);
            r.matrix = matrix;

            mon.limiter_gr_db.fetch_max(dsp.process_limiter(mon, l, rr));
            if !all_finite(l) || !all_finite(rr) {
                // A blown-up EQ or delay must not stay stuck: start the path over.
                dsp.reset();
                l.fill(0.0);
                rr.fill(0.0);
            }
            peak[0].fetch_max(peak_level(l));
            peak[1].fetch_max(peak_level(rr));
        }
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

        let c = &self.controls;
        let [speakers, phones] = &a.monitor;
        add_to(c.monitor_output_left.load(Relaxed), &speakers[0][..n]);
        add_to(c.monitor_output_right.load(Relaxed), &speakers[1][..n]);
        add_to(c.phones.output_left.load(Relaxed), &phones[0][..n]);
        add_to(c.phones.output_right.load(Relaxed), &phones[1][..n]);
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

/// The signal a soloed strip puts on the solo bus: its PFL copy or, AFL, its
/// output.
///
/// Solo-tap override hook: a solo that listens elsewhere in the strip (EQ
/// band, gate key or crossover solo) replaces the signal here.
#[inline]
unsafe fn solo_tap(s: &Strip, afl: bool, ch: usize, n: usize) -> &[f32] {
    let d = s.dsp();
    if afl {
        d.out(ch, n)
    } else {
        d.pfl(ch, n)
    }
}

/// Adds monitor SOURCE `index` (a main, matrix, bus or aux output) to a
/// monitor path with a ramped gain.
unsafe fn add_monitor_source(topo: &Topology, bus: &mut [Vec<f32>; 2], index: i32, last: f32, target: f32, n: usize) {
    if last == 0.0 && target == 0.0 {
        return;
    }
    let strip = match monitor_source(index) {
        Some((StripKind::Main, i)) => topo.mains.get(i),
        Some((StripKind::Matrix, i)) => topo.matrices.get(i),
        Some((StripKind::Bus, i)) => topo.buses().find(|b| b.id() as usize == i),
        Some((StripKind::Aux, i)) => topo.auxes.get(i),
        _ => None,
    };
    let Some(s) = strip else { return };
    let d = s.dsp();
    if d.idle() {
        return;
    }
    let [l, r] = bus;
    add_with_gain_ramp(&mut l[..n], d.out(0, n), last, target);
    add_with_gain_ramp(&mut r[..n], d.out(1, n), last, target);
}

/// Adds monitor DIRECT IN `index` (an input, aux, bus, main or matrix
/// output) to a monitor path with a ramped gain.
unsafe fn add_direct_in(topo: &Topology, bus: &mut [Vec<f32>; 2], index: i32, last: f32, target: f32, n: usize) {
    if last == 0.0 && target == 0.0 {
        return;
    }
    let Some(s) = direct_source(index).and_then(|(kind, i)| topo.by_number(kind, i as i32 + 1)) else { return };
    let d = s.dsp();
    if d.idle() {
        return;
    }
    let [l, r] = bus;
    add_with_gain_ramp(&mut l[..n], d.out(0, n), last, target);
    add_with_gain_ramp(&mut r[..n], d.out(1, n), last, target);
}

/// Talkback into a destination's sum (`talk.rs`): dims what the destination
/// summed (BUS DIM) and adds the talkback channel's PFL signal, both ramped.
///
/// # Safety
/// The destination's task only, after its sum and before it processes.
unsafe fn add_talk(dst: &Strip, g: TalkGains, ramp: Option<&mut TalkGains>, talk: &TalkBlock, n: usize) {
    let Some(ramp) = ramp else { return };
    if g == TalkGains::NONE && *ramp == TalkGains::NONE {
        return;
    }
    let d = dst.dsp_mut();
    if g.dim != 1.0 || ramp.dim != 1.0 {
        for ch in 0..2 {
            apply_gain_ramp(d.work_mut(ch, n), ramp.dim, g.dim);
        }
    }
    match talk.strip() {
        Some(src) if !src.dsp().idle() => {
            let s = src.dsp();
            add_with_gain_ramp(d.work_mut(0, n), s.pfl(0, n), ramp.talk, g.talk);
            add_with_gain_ramp(d.work_mut(1, n), s.pfl(1, n), ramp.talk, g.talk);
        }
        _ => {}
    }
    *ramp = g;
}

/// Applies a 2x2 matrix to the monitor pair, crossfading from `last` to `target`.
fn apply_monitor_matrix(l: &mut [f32], r: &mut [f32], last: [f32; 4], target: [f32; 4]) {
    if last == MONITOR_STEREO && target == MONITOR_STEREO {
        return;
    }
    let step = 1.0 / l.len().max(1) as f32;
    let mut t = 0.0;
    for (x, y) in l.iter_mut().zip(r.iter_mut()) {
        t += step;
        let m: [f32; 4] = std::array::from_fn(|i| last[i] + (target[i] - last[i]) * t);
        let (a, b) = (*x, *y);
        *x = m[0] * a + m[1] * b;
        *y = m[2] * a + m[3] * b;
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
            let (l, r) = if d.channels() == 1 { wing_mono_pan(p) } else { stereo_balance(p) };
            [l, r]
        }
    }
}

/// A TAP-mode (pre-fader) send is cut while its source is muted, unless it is
/// an input or aux channel's send set to IGNORE CHANNEL MUTE.
#[inline]
unsafe fn tap_send_muted(src: &Strip, ignore_mute: bool) -> bool {
    src.dsp().muted() && !(ignore_mute && src.kind().is_input())
}

/// A source's signal at `tap` (`pdc_tap`) as a destination aligned to
/// `target` sums it: delayed by the difference when the plan says so.
#[inline]
unsafe fn tap_at(src: &Strip, tap: usize, target: u32, ch: usize, n: usize) -> &[f32] {
    if let Some(delayed) = src.pdc.delayed(tap, target, ch, n) {
        return delayed;
    }
    let d = src.dsp();
    match tap {
        pdc_tap::TAP => d.tap_signal(ch, n),
        pdc_tap::PFL => d.pfl(ch, n),
        pdc_tap::POST => d.post(ch, n),
        _ => d.out(ch, n),
    }
}

/// Where a matrix send reads its source (`pdc_tap`) and the pan it applies
/// there (`None`: the strip's own). Input and aux channels: PAN LINK follows
/// the channel's pan (POST: its panned output, with the width), LINK
/// REVERSED mirrors it, else the send pans itself. Buses and mains send
/// their output (or tap) as panned, balanced by the send's pan.
fn matrix_send_route(s: &Strip, send: &MatrixSendParams) -> (usize, Option<f32>) {
    let pre = send.pre.load(Relaxed);
    if !s.kind().is_input() {
        return (if pre { pdc_tap::TAP } else { pdc_tap::OUT }, None);
    }
    let pan = match (send.pan_link.load(Relaxed), send.pan_reversed.load(Relaxed)) {
        (true, false) => None,
        (true, true) => Some(-s.params.pan.load() / 100.0),
        (false, _) => Some(send.pan.load() / 100.0),
    };
    let tap = match (pre, pan) {
        (true, _) => pdc_tap::TAP,
        (false, None) => pdc_tap::OUT,
        (false, Some(_)) => pdc_tap::POST,
    };
    (tap, pan)
}

/// The (source, tap) pairs summing strip `dst` adds up, as its stage reads
/// them: the enabled sends (and a matrix's direct input).
fn pdc_feeds<'a>(st: &'a Structure, dst: &Strip) -> Vec<(&'a Arc<Strip>, usize)> {
    let inputs = st.channels.iter().chain(&st.auxes);
    let index = dst.id().max(0) as usize;
    let tap_or_out = |pre: bool| if pre { pdc_tap::TAP } else { pdc_tap::OUT };
    let mut feeds = Vec::new();
    match dst.kind() {
        StripKind::Bus => {
            let upper = st.buses.iter().filter(|b| index < BUS_TO_BUS_TARGETS && b.id() as usize >= BUS_TO_BUS_TARGETS);
            for s in inputs.chain(upper) {
                let Some(send) = s.sends.find(index).filter(|send| send.enabled.load(Relaxed)) else { continue };
                let tap = match send.mode.load(Relaxed) {
                    send_mode::PRE => pdc_tap::TAP,
                    send_mode::GROUP => pdc_tap::OUT,
                    _ => pdc_tap::POST,
                };
                feeds.push((s, tap));
            }
        }
        StripKind::Main => {
            for s in inputs.chain(&st.buses) {
                let send = &s.params.mains[index.min(NUM_MAINS - 1)];
                if send.on.load(Relaxed) {
                    feeds.push((s, tap_or_out(send.pre.load(Relaxed))));
                }
            }
        }
        StripKind::Matrix => {
            for s in inputs.chain(&st.buses).chain(&st.mains) {
                let send = &s.params.matrix[index.min(NUM_MATRICES - 1)];
                if send.on.load(Relaxed) {
                    feeds.push((s, matrix_send_route(s, send).0));
                }
            }
            let p = &dst.params;
            let source = usize::try_from(p.direct_source.load(Relaxed)).ok();
            if let Some(ch) = source.and_then(|i| st.channels.get(i)).filter(|_| p.direct_on.load(Relaxed)) {
                feeds.push((ch, if p.direct_pre.load(Relaxed) { pdc_tap::PFL } else { pdc_tap::OUT }));
            }
        }
        StripKind::Channel | StripKind::Aux => {}
    }
    feeds
}

/// # Safety
/// Bus task only, after the stages feeding it.
unsafe fn sum_bus<'a>(bus: &Strip, sources: impl Iterator<Item = &'a Arc<Strip>>, n: usize) {
    bus.begin_summed_block(n);
    let slot = bus.id() as usize;
    let target = bus.pdc.target();

    for s in sources {
        let Some(send) = s.sends.find(slot) else { continue };
        let mode = send.mode.load(Relaxed);
        let level = if !send.enabled.load(Relaxed) {
            0.0
        } else if mode == send_mode::GROUP {
            1.0 // GROUP has no send level
        } else if mode == send_mode::PRE && tap_send_muted(s, send.ignore_mute.load(Relaxed)) {
            0.0
        } else {
            db_to_gain(send.level_db.load())
        };
        let last = &mut *send.last_gain.get();
        let src = s.dsp();
        if (level == 0.0 && *last == [0.0, 0.0]) || src.idle() {
            *last = [0.0, 0.0]; // silent strips have nothing to add
            continue;
        }
        match mode {
            send_mode::GROUP => {
                let (l, r) = (tap_at(s, pdc_tap::OUT, target, 0, n), tap_at(s, pdc_tap::OUT, target, 1, n));
                add_pair(bus, n, l, r, last, [level, level])
            }
            mode => {
                let pan = match (send.pan_link.load(Relaxed), send.pan_reversed.load(Relaxed)) {
                    (true, false) => None,
                    (true, true) => Some(-s.params.pan.load() / 100.0), // LINK REVERSED
                    (false, _) => Some(send.pan.load() / 100.0),
                };
                let g = pre_gains(s, pan);
                let tap = if mode == send_mode::PRE { pdc_tap::TAP } else { pdc_tap::POST };
                let (l, r) = (tap_at(s, tap, target, 0, n), tap_at(s, tap, target, 1, n));
                add_pair(bus, n, l, r, last, [level * g[0], level * g[1]]);
            }
        }
    }
}

/// # Safety
/// Main task `index` only, after the bus stages.
unsafe fn sum_main<'a>(main: &Strip, index: usize, sources: impl Iterator<Item = &'a Arc<Strip>>, n: usize) {
    main.begin_summed_block(n);
    let target = main.pdc.target();
    for s in sources {
        let send = &s.params.mains[index];
        let pre = send.pre.load(Relaxed);
        // Pre-fader main sends still follow the strip's mute.
        let on = send.on.load(Relaxed) && !(pre && s.dsp().muted());
        let level = if on { db_to_gain(send.level_db.load()) } else { 0.0 };
        let last = s.last_main(index);
        let src = s.dsp();
        if (level == 0.0 && *last == [0.0, 0.0]) || src.idle() {
            *last = [0.0, 0.0];
            continue;
        }
        if pre {
            let g = pre_gains(s, None);
            let (l, r) = (tap_at(s, pdc_tap::TAP, target, 0, n), tap_at(s, pdc_tap::TAP, target, 1, n));
            add_pair(main, n, l, r, last, [level * g[0], level * g[1]]);
        } else {
            let (l, r) = (tap_at(s, pdc_tap::OUT, target, 0, n), tap_at(s, pdc_tap::OUT, target, 1, n));
            add_pair(main, n, l, r, last, [level, level]);
        }
    }
}

/// # Safety
/// Matrix task `index` only, after the main stage.
unsafe fn sum_matrix(mtx: &Strip, index: usize, topo: &Topology, n: usize) {
    mtx.begin_summed_block(n);
    let target = mtx.pdc.target();
    for s in topo.active().iter().chain(topo.buses()).chain(&topo.mains) {
        let send = &s.params.matrix[index];
        let on = send.on.load(Relaxed) && !(send.pre.load(Relaxed) && tap_send_muted(s, send.ignore_mute.load(Relaxed)));
        let level = if on { db_to_gain(send.level_db.load()) } else { 0.0 };
        let last = s.last_mtx(index);
        let src = s.dsp();
        if (level == 0.0 && *last == [0.0, 0.0]) || src.idle() {
            *last = [0.0, 0.0];
            continue;
        }
        let (tap, pan) = matrix_send_route(s, send);
        if s.kind().is_input() {
            // The pan gains, unless the channel's panned output is taken as it is.
            let g = if tap == pdc_tap::OUT { [1.0, 1.0] } else { pre_gains(s, pan) };
            let (l, r) = (tap_at(s, tap, target, 0, n), tap_at(s, tap, target, 1, n));
            add_pair(mtx, n, l, r, last, [level * g[0], level * g[1]]);
            continue;
        }
        let (bl, br) = stereo_balance(send.pan.load() / 100.0);
        if tap == pdc_tap::TAP {
            let g = pre_gains(s, None);
            let (l, r) = (tap_at(s, pdc_tap::TAP, target, 0, n), tap_at(s, pdc_tap::TAP, target, 1, n));
            add_pair(mtx, n, l, r, last, [level * g[0] * bl, level * g[1] * br]);
        } else {
            let (l, r) = (tap_at(s, pdc_tap::OUT, target, 0, n), tap_at(s, pdc_tap::OUT, target, 1, n));
            add_pair(mtx, n, l, r, last, [level * bl, level * br]);
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
            let tap = if p.direct_pre.load(Relaxed) { pdc_tap::PFL } else { pdc_tap::OUT };
            let (l, r) = (tap_at(ch, tap, target, 0, n), tap_at(ch, tap, target, 1, n));
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
