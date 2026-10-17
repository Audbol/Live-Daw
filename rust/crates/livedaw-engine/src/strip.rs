//! One console strip, processed the WING way (Operation Manual, chapter 6):
//!
//! * input channels: INPUT (trim, balance, polarity) -> FILTER -> the GATE
//!   slot, EQ, DYNAMICS slot and INS 1 in the strip's process order -> fader
//!   (with DCAs, mute and mute groups) -> INS 2 (or automix) -> delay (post-fader)
//!   -> width/pan. The TAP point is selectable.
//! * aux channels: INPUT -> delay (pre-fader) -> DYNAMICS -> EQ -> INS 1 ->
//!   fader. No filter, gate or INS 2; the TAP point is fixed after INS 1.
//! * buses, mains and matrices: INPUT -> DYNAMICS -> INS 1 -> fader -> EQ
//!   (post-fader) -> INS 2 -> delay -> width/pan. The TAP point is fixed after
//!   INS 1 (pre-fader).
//!
//! Signal taps along the way feed TAP-mode sends, solo (PFL/AFL) and direct outs.

use crate::fx::FxRack;
use crate::params::{DcaParams, FxParams, StripKind, StripMeters, StripParams, NUM_MAINS, NUM_MATRICES};
use crate::sends::SendBank;
use crate::solo::StripConsole;
use livedaw_dsp::delay::{delay_to_ms, DelayLine};
use livedaw_dsp::util::*;
use livedaw_dsp::wing::dynamics::{DynStatus, WingDynamics};
use livedaw_dsp::wing::eq::WingEq;
use livedaw_dsp::wing::filter::WingFilter;
use livedaw_dsp::wing::{EQ_MODELS_BUS, EQ_MODELS_CH, MAX_DYN_PARAMS, MAX_EQ_PARAMS, PROCESS_ORDERS};
use livedaw_recorder::playback_channel;
use std::cell::UnsafeCell;
use std::sync::atomic::Ordering::Relaxed;
use std::sync::{Arc, Mutex};

/// Tap points (TAP_POINTS order). WING names: INPUT, FILTER, TAP 3/4/5 (after
/// the 1st/2nd/3rd block), PRE FDR, POST FDR (before INS 2), POST PROC (after
/// INS 2 and the post-fader delay, before width/pan).
pub mod tap {
    pub const IN: usize = 0;
    pub const FILT: usize = 1;
    pub const PFL: usize = 5;
    pub const AFL: usize = 6;
    pub const POST: usize = 7;
}

/// What a strip needs from the engine while processing.
pub(crate) struct ProcessContext<'a> {
    pub fx: &'a [FxRack],
    pub fx_params: &'a [FxParams],
    pub dcas: &'a [DcaParams],
    pub mute_groups: u32,
    /// DCA MUTEGROUPS: a muted DCA mutes its strips.
    pub dca_mute_groups: bool,
    /// MAIN LINK: mains 2..=`main_link_last` (0-based 1..) add Main 1's fader level.
    pub main_link_last: usize,
    pub main_link_db: f32,
    /// Hardware inputs by hardware channel (null = inactive).
    pub hw_inputs: &'a [*const f32],
    /// The recorder's playback channels (input `PLAYBACK_SOURCE_BASE + n`).
    pub playback: &'a [*const f32],
    /// INPUT SELECT: every input strip follows `global_alt` (else its own switch).
    pub input_select_global: bool,
    pub global_alt: bool,
    /// The input channels (for sidechain keys from other channels).
    pub channels: &'a [Arc<Strip>],
    pub block: u64,
}

impl ProcessContext<'_> {
    /// The buffer of input source `index` (a hardware input or a playback channel).
    fn source(&self, index: i32) -> Option<*const f32> {
        source_ptr(self.hw_inputs, self.playback, index)
    }
}

/// Realtime state, touched by exactly one audio task at a time (see the
/// stage rules on [`Strip`]).
pub(crate) struct StripDsp {
    sample_rate: f64,
    max_block: usize,
    num_channels: usize,
    input_connected: bool,
    idle: bool,
    work: [Vec<f32>; 2],
    tap: [Vec<f32>; 2],
    pfl: [Vec<f32>; 2],
    post: [Vec<f32>; 2],
    out: [Vec<f32>; 2],
    key: Vec<f32>,
    delay: DelayLine,
    filter: WingFilter,
    gate: WingDynamics,
    eq: WingEq,
    dynamics: WingDynamics,
    gate_values: [f32; MAX_DYN_PARAMS],
    dyn_values: [f32; MAX_DYN_PARAMS],
    eq_values: [f32; MAX_EQ_PARAMS],
    last_in: [f32; 2],
    last_gain: f32,
    /// 2x2 output matrix (ll, rl, lr, rr): out_l = ll*L + rl*R, out_r = lr*L + rr*R.
    out_matrix: [f32; 4],
    /// Pan gains for taking pre-fader signals into stereo destinations.
    pan_gains: [f32; 2],
    automix_gain: f32,
    automix_power: f32,
    tap_point: usize,
    /// Effective mute in the last block (TAP-mode sends follow it).
    muted: bool,
    /// Matrices: ramp of the direct input (owned by the matrix task).
    last_direct: f32,
}

impl StripDsp {
    fn new() -> Self {
        Self {
            sample_rate: 48000.0,
            max_block: 0,
            num_channels: 1,
            input_connected: true,
            idle: false,
            work: [Vec::new(), Vec::new()],
            tap: [Vec::new(), Vec::new()],
            pfl: [Vec::new(), Vec::new()],
            post: [Vec::new(), Vec::new()],
            out: [Vec::new(), Vec::new()],
            key: Vec::new(),
            delay: DelayLine::default(),
            filter: WingFilter::default(),
            gate: WingDynamics::default(),
            eq: WingEq::default(),
            dynamics: WingDynamics::default(),
            gate_values: [0.0; MAX_DYN_PARAMS],
            dyn_values: [0.0; MAX_DYN_PARAMS],
            eq_values: [0.0; MAX_EQ_PARAMS],
            last_in: [1.0; 2],
            last_gain: 1.0,
            out_matrix: [0.0; 4],
            pan_gains: [1.0; 2],
            automix_gain: 1.0,
            automix_power: 0.0,
            tap_point: tap::PFL,
            muted: false,
            last_direct: 0.0,
        }
    }

    fn reset_dsp(&mut self) {
        self.filter.reset();
        self.gate.reset();
        self.eq.reset();
        self.dynamics.reset();
        self.delay.reset();
    }

    fn buffers_mut(&mut self) -> [&mut [Vec<f32>; 2]; 5] {
        [&mut self.work, &mut self.tap, &mut self.pfl, &mut self.post, &mut self.out]
    }
}

/// A console strip.
///
/// Thread rules for the realtime state (`dsp` and the `last_*` ramps):
/// * the stage task processing a strip owns its `dsp`; later stages only *read*
///   the buffers of strips processed in earlier stages;
/// * each destination task owns the ramps of the sends feeding it: bus tasks the
///   `SendParams::last_gain` of their slot, main task `i` `last_main[i]`, matrix
///   task `j` `last_mtx[j]`;
/// * the monitor stage runs on the audio thread alone and owns `last_solo`;
/// * the message thread only touches `dsp` through `prepare`, while the strip is
///   not reachable by the audio thread (device stopped, or not yet published).
pub struct Strip {
    kind: StripKind,
    id: i32,
    pub params: StripParams,
    pub meters: StripMeters,
    /// Sends to buses (channels/aux: all buses; buses 9+: buses 1-8).
    pub sends: SendBank,
    /// UI metadata (never touched by the audio thread).
    name: Mutex<String>,
    /// The free tags of HOME > TAGS, comma-separated (the WING's `/ch/n/tags`).
    tags: Mutex<String>,
    /// Console-level state: talk tags, MUTEGROUP/SIP OVERRIDE, the block's
    /// effective solo and mute decision (`solo.rs`).
    pub console: StripConsole,
    dsp: UnsafeCell<StripDsp>,
    last_main: [UnsafeCell<[f32; 2]>; NUM_MAINS],
    last_mtx: [UnsafeCell<[f32; 2]>; NUM_MATRICES],
    last_solo: UnsafeCell<f32>,
}

// SAFETY: see the thread rules above; everything shared is atomic.
unsafe impl Sync for Strip {}
unsafe impl Send for Strip {}

impl Strip {
    pub fn new(kind: StripKind, id: i32) -> Self {
        Self {
            kind,
            id,
            params: StripParams::new(kind),
            meters: StripMeters::default(),
            sends: SendBank::default(),
            name: Mutex::new(String::new()),
            tags: Mutex::new(String::new()),
            console: StripConsole::default(),
            dsp: UnsafeCell::new(StripDsp::new()),
            last_main: Default::default(),
            last_mtx: Default::default(),
            last_solo: UnsafeCell::new(0.0),
        }
    }

    pub fn kind(&self) -> StripKind {
        self.kind
    }

    /// Channels: a stable id. Buses: the slot sends refer to. Others: 0-based index.
    pub fn id(&self) -> i32 {
        self.id
    }

    pub fn name(&self) -> String {
        self.name.lock().unwrap_or_else(|e| e.into_inner()).clone()
    }

    pub fn set_name(&self, name: &str) {
        *self.name.lock().unwrap_or_else(|e| e.into_inner()) = name.to_owned();
    }

    /// The free tags (HOME > TAGS), comma-separated.
    pub fn tags(&self) -> String {
        self.tags.lock().unwrap_or_else(|e| e.into_inner()).clone()
    }

    pub fn set_tags(&self, tags: &str) {
        *self.tags.lock().unwrap_or_else(|e| e.into_inner()) = tags.to_owned();
    }

    /// The strip has an INS 2 slot (every kind but aux).
    pub fn has_post_insert(&self) -> bool {
        self.kind != StripKind::Aux
    }

    /// The strip can join automix X/Y (the INS 2 of input channels only).
    pub fn has_automix(&self) -> bool {
        self.kind == StripKind::Channel
    }

    /// The TAP point can be chosen (input channels; the others are fixed after INS 1).
    pub fn has_selectable_tap(&self) -> bool {
        self.kind == StripKind::Channel
    }

    /// Input and aux channels: the strip takes its ALT input now - it has one,
    /// and ALT is selected (globally with INPUT SELECT global, else on the strip).
    pub fn uses_alt_input(&self, input_select_global: bool, global_alt: bool) -> bool {
        let p = &self.params;
        self.kind.is_input()
            && p.alt_input_left.load(Relaxed) >= 0
            && if input_select_global { global_alt } else { p.alt_selected.load(Relaxed) }
    }

    /// The input sources (left, right) in use: MAIN or ALT.
    pub fn active_inputs(&self, input_select_global: bool, global_alt: bool) -> (i32, i32) {
        let p = &self.params;
        if self.uses_alt_input(input_select_global, global_alt) {
            (p.alt_input_left.load(Relaxed), p.alt_input_right.load(Relaxed))
        } else {
            (p.input_left.load(Relaxed), p.input_right.load(Relaxed))
        }
    }

    /// The INS 2 slot runs an FX slot (not automix), with `fx` its slot index.
    fn post_insert_fx(&self) -> Option<i32> {
        let ins = &self.params.post_insert;
        (self.has_post_insert() && ins.on.load(Relaxed) && ins.mode.load(Relaxed) == 0).then(|| ins.fx.load(Relaxed))
    }

    /// True if this strip feeds `slot` from its pre/post insert.
    pub fn uses_fx(&self, slot: usize) -> bool {
        let p = &self.params;
        (p.pre_insert.on.load(Relaxed) && p.pre_insert.fx.load(Relaxed) == slot as i32)
            || self.post_insert_fx() == Some(slot as i32)
    }

    fn has_inserts(&self) -> bool {
        let p = &self.params;
        (p.pre_insert.on.load(Relaxed) && p.pre_insert.fx.load(Relaxed) >= 0) || self.post_insert_fx().is_some_and(|fx| fx >= 0)
    }

    /// # Safety
    /// The audio thread must not be able to reach this strip (device stopped or
    /// strip not yet published).
    pub(crate) unsafe fn prepare(&self, sample_rate: f64, max_block: usize) {
        let d = &mut *self.dsp.get();
        d.sample_rate = sample_rate;
        d.max_block = max_block.max(1);
        let mb = d.max_block;
        for buffers in d.buffers_mut() {
            for b in buffers.iter_mut() {
                b.clear();
                b.resize(mb, 0.0); // zeroed: an idle strip relies on it
            }
        }
        d.key.clear();
        d.key.resize(mb, 0.0);
        d.idle = false;
        d.filter.prepare(sample_rate);
        d.gate.prepare(sample_rate);
        d.eq.prepare(sample_rate);
        d.dynamics.prepare(sample_rate);
        d.delay.prepare(sample_rate);
    }

    // ---- read-only views (valid after the strip's stage has run) ------------

    #[inline]
    pub(crate) unsafe fn dsp(&self) -> &StripDsp {
        &*self.dsp.get()
    }

    #[inline]
    #[allow(clippy::mut_from_ref)]
    pub(crate) unsafe fn dsp_mut(&self) -> &mut StripDsp {
        &mut *self.dsp.get()
    }

    #[inline]
    #[allow(clippy::mut_from_ref)]
    pub(crate) unsafe fn last_main(&self, main: usize) -> &mut [f32; 2] {
        &mut *self.last_main[main].get()
    }

    #[inline]
    #[allow(clippy::mut_from_ref)]
    pub(crate) unsafe fn last_mtx(&self, mtx: usize) -> &mut [f32; 2] {
        &mut *self.last_mtx[mtx].get()
    }

    #[inline]
    #[allow(clippy::mut_from_ref)]
    pub(crate) unsafe fn last_solo(&self) -> &mut f32 {
        &mut *self.last_solo.get()
    }

    /// Channels processed in the last block (1 or 2). Racy read for the UI is fine.
    pub fn num_channels(&self) -> usize {
        self.meters.current_width.load(Relaxed).clamp(1, 2) as usize
    }

    /// True if the last block took the silent fast path (all outputs zero).
    pub fn is_idle(&self) -> bool {
        // SAFETY: a plain bool read; the UI tolerates a stale value.
        unsafe { std::ptr::read_volatile(&(*self.dsp.get()).idle) }
    }

    /// Effective mute: own mute, an engaged mute group or (with DCA MUTEGROUPS)
    /// a muted DCA.
    pub(crate) fn is_muted(&self, dcas: &[DcaParams], mute_groups: u32, dca_mute_groups: bool) -> bool {
        let p = &self.params;
        if p.mute.load(Relaxed) || p.mute_group_mask.load(Relaxed) & mute_groups != 0 {
            return true;
        }
        let mask = p.dca_mask.load(Relaxed);
        dca_mute_groups && mask != 0 && dcas.iter().enumerate().any(|(i, d)| mask & (1 << i) != 0 && d.mute.load(Relaxed))
    }

    /// Effective solo: own solo or a soloed DCA.
    pub(crate) fn is_soloed(&self, dcas: &[DcaParams]) -> bool {
        let p = &self.params;
        if p.solo.load(Relaxed) {
            return true;
        }
        let mask = p.dca_mask.load(Relaxed);
        mask != 0 && dcas.iter().enumerate().any(|(i, d)| mask & (1 << i) != 0 && d.solo.load(Relaxed))
    }

    fn dca_offset_db(&self, dcas: &[DcaParams]) -> f32 {
        let mask = self.params.dca_mask.load(Relaxed);
        if mask == 0 {
            return 0.0;
        }
        dcas.iter().enumerate().filter(|(i, _)| mask & (1 << i) != 0).map(|(_, d)| d.fader_db.load()).sum()
    }

    // ---- audio thread ---------------------------------------------------------

    /// Input channels: pull from their source (MAIN or ALT input: hardware
    /// inputs or playback channels).
    ///
    /// # Safety
    /// Owning task only. Non-null pointers must be valid for `n` samples.
    unsafe fn read_hardware_inputs(&self, d: &mut StripDsp, ctx: &ProcessContext, n: usize) {
        let p = &self.params;
        let (left, mut right) = self.active_inputs(ctx.input_select_global, ctx.global_alt);
        d.num_channels = if p.stereo.load(Relaxed) { 2 } else { 1 };
        if d.num_channels == 2 && right < 0 && left >= 0 {
            right = left + 1; // stereo without an explicit right input: next channel
        }
        self.meters.current_width.store(d.num_channels as i32, Relaxed);

        let sources = [left, right];
        let source = |ch: usize| ctx.source(sources[ch]);

        // Already idle and still unpatched: buffers are still zero, nothing to do.
        if d.idle && (0..d.num_channels).all(|ch| source(ch).is_none()) {
            d.input_connected = false;
            return;
        }

        d.input_connected = false;
        for ch in 0..d.num_channels {
            let dst = &mut d.work[ch][..n];
            match source(ch) {
                Some(src) => {
                    dst.copy_from_slice(std::slice::from_raw_parts(src, n));
                    d.input_connected = true;
                }
                None => dst.fill(0.0),
            }
            self.meters.input_peak[ch].fetch_max(peak_level(dst));
        }
    }

    /// Summing buses: clear the work buffer before sums are accumulated into it.
    ///
    /// # Safety
    /// Owning task only.
    pub(crate) unsafe fn begin_summed_block(&self, n: usize) {
        let d = self.dsp_mut();
        d.num_channels = 2;
        self.meters.current_width.store(2, Relaxed);
        for ch in 0..2 {
            d.work[ch][..n].fill(0.0);
        }
    }

    /// Runs the whole strip for one block.
    ///
    /// # Safety
    /// Owning task only; inputs are read from `ctx.hw_inputs`, summing buses must
    /// have accumulated their input with `begin_summed_block` + sums first.
    pub(crate) unsafe fn process(&self, ctx: &ProcessContext, n: usize) {
        let d = self.dsp_mut();
        debug_assert!(n <= d.max_block);

        if self.kind.is_input() {
            self.read_hardware_inputs(d, ctx, n);
            // Fast path: an unpatched input without inserts can only output
            // silence, so skip its DSP entirely (big consoles with unused channels).
            if !d.input_connected && !self.has_inserts() {
                if !d.idle {
                    // Clear once on entering idle; nothing writes the buffers after that.
                    for buffers in d.buffers_mut() {
                        buffers.iter_mut().for_each(|b| b.fill(0.0));
                    }
                }
                self.meters.gate_reduction_db.store(0.0);
                self.meters.dyn_reduction_db.store(0.0);
                self.meters.gate_active.store(false, Relaxed);
                self.meters.dyn_active.store(false, Relaxed);
                d.idle = true;
                return;
            }
        } else {
            for ch in 0..d.num_channels {
                self.meters.input_peak[ch].fetch_max(peak_level(&d.work[ch][..n]));
            }
        }

        if d.idle {
            d.reset_dsp(); // coming back from idle: no stale filter state
            d.idle = false;
        }

        self.run_chain(d, ctx, n);

        // Never let NaN/inf escape a strip: it would poison every bus it feeds.
        let finite = (0..d.num_channels).all(|ch| all_finite(&d.work[ch][..n]) && all_finite(&d.post[ch][..n]));
        if !finite {
            for buffers in d.buffers_mut() {
                buffers.iter_mut().for_each(|b| b[..n].fill(0.0));
            }
            d.reset_dsp();
            self.meters.dsp_faults.fetch_add(1, Relaxed);
        }

        self.run_output_stage(d, n);
    }

    fn copy_tap(d: &mut StripDsp, point: usize, n: usize) {
        if d.tap_point == point {
            let nc = d.num_channels;
            let (work, tap) = (&d.work, &mut d.tap);
            for ch in 0..nc {
                tap[ch][..n].copy_from_slice(&work[ch][..n]);
            }
        }
    }

    /// The pre-fader blocks in processing order: input channels use their
    /// process order, aux channels run DYNAMICS, EQ, INS 1 (Fig. 68), and
    /// buses, mains and matrices DYNAMICS and INS 1 (their EQ is post-fader).
    fn pre_fader_order(&self) -> &'static [u8] {
        match self.kind {
            StripKind::Channel => {
                let i = self.params.process_order.load(Relaxed).clamp(0, PROCESS_ORDERS.len() as i32 - 1) as usize;
                PROCESS_ORDERS[i].as_bytes()
            }
            StripKind::Aux => b"DEI",
            _ => b"DI",
        }
    }

    unsafe fn run_chain(&self, d: &mut StripDsp, ctx: &ProcessContext, n: usize) {
        let p = &self.params;
        let nc = d.num_channels;
        let kind = self.kind;
        d.tap_point = if self.has_selectable_tap() { p.tap.load(Relaxed).clamp(0, 7) as usize } else { tap::PFL };

        // INPUT: trim, balance, polarity
        let polarity = if p.input.invert.load(Relaxed) { -1.0 } else { 1.0 };
        let trim = db_to_gain(p.input.trim_db.load()) * polarity;
        let bal = if nc == 2 { p.input.balance_db.load() } else { 0.0 };
        let gains = [trim * db_to_gain(-bal.max(0.0)), trim * db_to_gain(bal.min(0.0))];
        for (ch, &gain) in gains.iter().enumerate().take(nc) {
            apply_gain_ramp(&mut d.work[ch][..n], d.last_in[ch], gain);
            self.meters.trim_peak[ch].fetch_max(peak_level(&d.work[ch][..n]));
        }
        d.last_in = gains;

        // Aux channels delay before the fader; the others after it (below).
        if kind == StripKind::Aux {
            self.apply_delay(d, n);
        }
        Self::copy_tap(d, tap::IN, n);

        // FILTER (input channels only)
        if kind == StripKind::Channel {
            let settings = p.filter.snapshot();
            if settings.is_active() {
                let [w0, w1] = &mut d.work;
                let mut chans: [&mut [f32]; 2] = [&mut w0[..n], &mut w1[..n]];
                d.filter.process(&mut chans[..nc], &settings);
            }
        }
        Self::copy_tap(d, tap::FILT, n);

        for (pos, block) in self.pre_fader_order().iter().enumerate() {
            match block {
                b'G' => self.run_gate(d, ctx, n),
                b'E' => self.run_eq(d, n),
                b'D' => self.run_dynamics(d, ctx, n),
                b'I' => self.run_insert(d, ctx, n, false),
                _ => {}
            }
            // Tap points "3", "4", "5" sit after the 1st, 2nd and 3rd block.
            if pos < 3 {
                Self::copy_tap(d, 2 + pos, n);
            }
        }

        // PFL: pre-fader, after INS 1 (solo, direct outs, the fixed tap of aux,
        // buses, mains and matrices, the METERS screen).
        for ch in 0..nc {
            d.pfl[ch][..n].copy_from_slice(&d.work[ch][..n]);
            self.meters.pfl_peak[ch].fetch_max(peak_level(&d.pfl[ch][..n]));
        }
        Self::copy_tap(d, tap::PFL, n);

        // Fader with DCAs, mute and mute groups (and Main 1's level on linked mains);
        // the console decides over that mute (SIP, OVERRIDE, TALK ON: solo.rs).
        let muted = self.console.apply_mute(self.is_muted(ctx.dcas, ctx.mute_groups, ctx.dca_mute_groups));
        d.muted = muted;
        let mut level = p.fader_db.load() + self.dca_offset_db(ctx.dcas);
        if kind == StripKind::Main && (1..=ctx.main_link_last).contains(&(self.id.max(0) as usize)) {
            level += ctx.main_link_db;
        }
        let gain = if muted { 0.0 } else { db_to_gain(level) };
        for ch in 0..nc {
            apply_gain_ramp(&mut d.work[ch][..n], d.last_gain, gain);
        }
        d.last_gain = gain;
        Self::copy_tap(d, tap::AFL, n);

        // Buses, mains and matrices: the EQ is post-fader.
        if kind.is_output() {
            self.run_eq(d, n);
        }

        // INS 2 (automix is applied by the engine after all channels ran; it
        // follows the post-fader level, so the fader shapes the automix response).
        if self.post_insert_fx().is_some() {
            self.run_insert(d, ctx, n, true);
        }
        if self.has_automix() && p.post_insert.on.load(Relaxed) && p.post_insert.mode.load(Relaxed) != 0 {
            let power: f32 = (0..nc).map(|ch| d.work[ch][..n].iter().map(|x| x * x).sum::<f32>()).sum::<f32>() / (n * nc) as f32;
            let c = 0.7;
            d.automix_power = c * d.automix_power + (1.0 - c) * power * db_to_gain(p.post_insert.weight.load() * 2.0);
        }

        // Post-fader delay: input channels (DELAY (POST)), buses, mains, matrices.
        if kind != StripKind::Aux {
            self.apply_delay(d, n);
        }
        for ch in 0..nc {
            d.post[ch][..n].copy_from_slice(&d.work[ch][..n]);
        }
        Self::copy_tap(d, tap::POST, n);
    }

    fn apply_delay(&self, d: &mut StripDsp, n: usize) {
        let p = &self.params.input;
        if !p.delay_on.load(Relaxed) {
            return;
        }
        let ms = delay_to_ms(p.delay.load(), p.delay_mode.load(Relaxed).clamp(0, 3) as usize, d.sample_rate);
        let samples = (ms.max(0.0) * 0.001 * d.sample_rate as f32).round() as usize;
        let nc = d.num_channels;
        let [w0, w1] = &mut d.work;
        let mut chans: [&mut [f32]; 2] = [&mut w0[..n], &mut w1[..n]];
        d.delay.process(&mut chans[..nc], samples);
    }

    /// Key signal for a dynamics slot: another channel's input, or None (self).
    unsafe fn key_for(&self, d: &mut StripDsp, ctx: &ProcessContext, source: i32, n: usize) -> bool {
        if source < 0 {
            return false;
        }
        let Some(ch) = ctx.channels.get(source as usize) else { return false };
        let ptr = ctx.source(ch.active_inputs(ctx.input_select_global, ctx.global_alt).0);
        match ptr {
            Some(src) => d.key[..n].copy_from_slice(std::slice::from_raw_parts(src, n)),
            None => d.key[..n].fill(0.0),
        }
        true
    }

    unsafe fn run_gate(&self, d: &mut StripDsp, ctx: &ProcessContext, n: usize) {
        let slot = &self.params.gate;
        if !slot.on.load(Relaxed) {
            self.meters.gate_reduction_db.store(0.0);
            self.meters.gate_active.store(false, Relaxed);
            return;
        }
        slot.values_snapshot(&mut d.gate_values);
        let external = self.key_for(d, ctx, slot.sc_source.load(Relaxed), n);
        let nc = d.num_channels;
        let [w0, w1] = &mut d.work;
        let mut chans: [&mut [f32]; 2] = [&mut w0[..n], &mut w1[..n]];
        let key = if external { Some(&d.key[..n]) } else { None };
        let status: DynStatus = d.gate.process(
            &mut chans[..nc],
            key,
            slot.model_index(),
            &d.gate_values,
            &slot.sidechain(),
            &slot.xover(),
            slot.mix.load() / 100.0,
            slot.gain.load(),
        );
        self.meters.gate_reduction_db.store(status.gain_reduction_db);
        self.meters.gate_active.store(status.active, Relaxed);
    }

    unsafe fn run_dynamics(&self, d: &mut StripDsp, ctx: &ProcessContext, n: usize) {
        let slot = &self.params.dynamics;
        if !slot.on.load(Relaxed) {
            self.meters.dyn_reduction_db.store(0.0);
            self.meters.dyn_active.store(false, Relaxed);
            return;
        }
        slot.values_snapshot(&mut d.dyn_values);
        let external = self.key_for(d, ctx, slot.sc_source.load(Relaxed), n);
        let nc = d.num_channels;
        let [w0, w1] = &mut d.work;
        let mut chans: [&mut [f32]; 2] = [&mut w0[..n], &mut w1[..n]];
        let key = if external { Some(&d.key[..n]) } else { None };
        let status = d.dynamics.process(
            &mut chans[..nc],
            key,
            slot.model_index(),
            &d.dyn_values,
            &slot.sidechain(),
            &slot.xover(),
            slot.mix.load() / 100.0,
            slot.gain.load(),
        );
        self.meters.dyn_reduction_db.store(status.gain_reduction_db);
        self.meters.dyn_active.store(status.active, Relaxed);
    }

    fn run_eq(&self, d: &mut StripDsp, n: usize) {
        let eq = &self.params.eq;
        eq.values_snapshot(&mut d.eq_values);
        let table = if self.kind.is_output() { EQ_MODELS_BUS } else { EQ_MODELS_CH };
        let model = (eq.model.load(Relaxed).max(0) as usize).min(table.len() - 1);
        let nc = d.num_channels;
        let [w0, w1] = &mut d.work;
        let mut chans: [&mut [f32]; 2] = [&mut w0[..n], &mut w1[..n]];
        d.eq.process(&mut chans[..nc], table, model, eq.on.load(Relaxed), eq.mix.load(), &d.eq_values);
    }

    unsafe fn run_insert(&self, d: &mut StripDsp, ctx: &ProcessContext, n: usize, post: bool) {
        let ins = if post { &self.params.post_insert } else { &self.params.pre_insert };
        if !ins.on.load(Relaxed) {
            return;
        }
        let slot = ins.fx.load(Relaxed);
        let (Some(rack), Some(fxp)) = (ctx.fx.get(slot.max(0) as usize), ctx.fx_params.get(slot.max(0) as usize)) else { return };
        if slot < 0 {
            return;
        }
        let nc = d.num_channels;
        let [w0, w1] = &mut d.work;
        let mut chans: [&mut [f32]; 2] = [&mut w0[..n], &mut w1[..n]];
        rack.process(&mut chans[..nc], d.sample_rate, fxp, ctx.block);
    }

    fn run_output_stage(&self, d: &mut StripDsp, n: usize) {
        let p = &self.params;
        let nc = d.num_channels;
        let pan = p.pan.load() / 100.0;

        let target = if nc == 1 {
            let (l, r) = constant_power_pan(pan);
            d.pan_gains = [l, r];
            [l, 0.0, r, 0.0]
        } else {
            let (gl, gr) = stereo_balance(pan);
            d.pan_gains = [gl, gr];
            let w = (p.width.load() / 100.0).clamp(-1.5, 1.5);
            let (same, cross) = ((1.0 + w) * 0.5, (1.0 - w) * 0.5);
            [gl * same, gl * cross, gr * cross, gr * same]
        };
        let mono = self.kind.is_output() && p.mono.load(Relaxed);

        let last = d.out_matrix;
        let right = if nc > 1 { 1 } else { 0 };
        let [o0, o1] = &mut d.out;
        let (o0, o1) = (&mut o0[..n], &mut o1[..n]);
        o0.fill(0.0);
        o1.fill(0.0);
        add_with_gain_ramp(o0, &d.post[0][..n], last[0], target[0]);
        add_with_gain_ramp(o1, &d.post[0][..n], last[2], target[2]);
        if nc > 1 {
            add_with_gain_ramp(o0, &d.post[right][..n], last[1], target[1]);
            add_with_gain_ramp(o1, &d.post[right][..n], last[3], target[3]);
        }
        d.out_matrix = target;
        if mono {
            for (l, r) in o0.iter_mut().zip(o1.iter_mut()) {
                let m = (*l + *r) * 0.5;
                *l = m;
                *r = m;
            }
        }

        self.meters.output_peak[0].fetch_max(peak_level(o0));
        self.meters.output_peak[1].fetch_max(peak_level(o1));
    }

    /// Engine automix pass: scales everything after INS 2 (where automix sits)
    /// by `gain`; the POST FDR tap, before INS 2, keeps the plain fader level.
    ///
    /// # Safety
    /// Audio thread, after the input stage, before anything reads this strip.
    pub(crate) unsafe fn apply_automix(&self, gain: f32, n: usize) {
        let d = self.dsp_mut();
        let last = d.automix_gain;
        let nc = d.num_channels;
        for ch in 0..nc {
            apply_gain_ramp(&mut d.post[ch][..n], last, gain);
            if d.tap_point == tap::POST {
                apply_gain_ramp(&mut d.tap[ch][..n], last, gain);
            }
        }
        for ch in 0..2 {
            apply_gain_ramp(&mut d.out[ch][..n], last, gain);
        }
        d.automix_gain = gain;
        self.meters.automix_db.store(gain_to_db(gain));
    }
}

/// The buffer of input source `index`: hardware input `index`, or playback
/// channel `index - PLAYBACK_SOURCE_BASE` (None = not connected).
pub(crate) fn source_ptr(hw: &[*const f32], playback: &[*const f32], index: i32) -> Option<*const f32> {
    let p = match playback_channel(index) {
        Some(n) => playback.get(n),
        None => hw.get(usize::try_from(index).ok()?),
    };
    p.copied().filter(|p| !p.is_null())
}

/// Read access to a processed strip's signal taps (valid after its stage).
impl StripDsp {
    #[inline]
    pub(crate) fn channels(&self) -> usize {
        self.num_channels
    }
    #[inline]
    pub(crate) fn idle(&self) -> bool {
        self.idle
    }
    /// Right-channel index for this strip's width (mono strips repeat the left).
    #[inline]
    fn rc(&self, ch: usize) -> usize {
        if self.num_channels > 1 {
            ch.min(1)
        } else {
            0
        }
    }
    #[inline]
    pub(crate) fn tap_signal(&self, ch: usize, n: usize) -> &[f32] {
        &self.tap[self.rc(ch)][..n]
    }
    #[inline]
    pub(crate) fn pfl(&self, ch: usize, n: usize) -> &[f32] {
        &self.pfl[self.rc(ch)][..n]
    }
    #[inline]
    pub(crate) fn post(&self, ch: usize, n: usize) -> &[f32] {
        &self.post[self.rc(ch)][..n]
    }
    #[inline]
    pub(crate) fn out(&self, ch: usize, n: usize) -> &[f32] {
        &self.out[ch.min(1)][..n]
    }
    #[inline]
    pub(crate) fn pan_gains(&self) -> [f32; 2] {
        self.pan_gains
    }
    #[inline]
    pub(crate) fn automix_power(&self) -> f32 {
        self.automix_power
    }
    #[inline]
    pub(crate) fn automix_gain(&self) -> f32 {
        self.automix_gain
    }
    /// The strip was muted (own mute, mute group, DCA) in this block.
    #[inline]
    pub(crate) fn muted(&self) -> bool {
        self.muted
    }
    #[inline]
    pub(crate) fn last_direct(&self) -> f32 {
        self.last_direct
    }
    #[inline]
    pub(crate) fn set_last_direct(&mut self, g: f32) {
        self.last_direct = g;
    }
    #[inline]
    pub(crate) fn work_mut(&mut self, ch: usize, n: usize) -> &mut [f32] {
        &mut self.work[ch][..n]
    }
}
