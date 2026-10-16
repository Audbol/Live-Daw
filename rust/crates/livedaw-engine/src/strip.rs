//! One console strip: trim -> polarity -> HPF -> gate -> EQ -> compressor ->
//! plugin inserts -> mute -> fader -> pan. Used for inputs, buses and the master.

use crate::inserts::InsertChain;
use crate::params::{StripKind, StripMeters, StripParams};
use crate::sends::SendBank;
use livedaw_dsp::util::*;
use livedaw_dsp::{Biquad, BiquadCoeffs, Compressor, NoiseGate, ParametricEq};
use std::cell::UnsafeCell;
use std::sync::atomic::{AtomicU32, Ordering::Relaxed};
use std::sync::Mutex;

/// Realtime state, touched by exactly one audio task at a time (see the
/// stage rules on [`Strip`]).
pub(crate) struct StripDsp {
    sample_rate: f64,
    max_block: usize,
    num_channels: usize,
    input_connected: bool,
    idle: bool,
    work: [Vec<f32>; 2],
    pre: [Vec<f32>; 2],
    post: [Vec<f32>; 2],
    main: [Vec<f32>; 2],
    hpf: Biquad,
    last_hpf_freq: f32,
    hpf_was_on: bool,
    gate: NoiseGate,
    eq: ParametricEq,
    comp: Compressor,
    last_trim: f32,
    last_mute: f32,
    last_fader: f32,
    pan_left: f32,
    pan_right: f32,
    last_pan_left: f32,
    last_pan_right: f32,
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
            pre: [Vec::new(), Vec::new()],
            post: [Vec::new(), Vec::new()],
            main: [Vec::new(), Vec::new()],
            hpf: Biquad::default(),
            last_hpf_freq: -1.0,
            hpf_was_on: false,
            gate: NoiseGate::default(),
            eq: ParametricEq::default(),
            comp: Compressor::default(),
            last_trim: 1.0,
            last_mute: 1.0,
            last_fader: 1.0,
            pan_left: 1.0,
            pan_right: 1.0,
            last_pan_left: 1.0,
            last_pan_right: 1.0,
        }
    }

    fn reset_dsp(&mut self) {
        self.hpf.reset();
        self.gate.reset();
        self.eq.reset();
        self.comp.reset();
    }
}

/// A console strip.
///
/// Thread rules for the realtime state (`dsp`, `last_main_assign`, `last_solo`):
/// * stage 1 (inputs) / stage 2 (buses): the task processing a strip owns its `dsp`;
///   bus tasks only *read* input strips' output buffers, which are complete by then;
/// * master and monitor stages run on the audio thread alone and own the
///   per-strip summing ramps (`last_main_assign`, `last_solo`);
/// * the message thread only touches `dsp` through `prepare`, while the strip is
///   not reachable by the audio thread (device stopped, or not yet published).
pub struct Strip {
    kind: StripKind,
    id: i32,
    pub params: StripParams,
    pub meters: StripMeters,
    pub sends: SendBank,
    pub inserts: InsertChain,
    /// UI metadata (never touched by the audio thread).
    name: Mutex<String>,
    colour: AtomicU32,
    dsp: UnsafeCell<StripDsp>,
    last_main_assign: UnsafeCell<f32>,
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
            inserts: InsertChain::default(),
            name: Mutex::new(String::new()),
            colour: AtomicU32::new(Self::default_colour(kind)),
            dsp: UnsafeCell::new(StripDsp::new()),
            last_main_assign: UnsafeCell::new(0.0),
            last_solo: UnsafeCell::new(0.0),
        }
    }

    pub fn kind(&self) -> StripKind {
        self.kind
    }

    /// Inputs: a stable id. Buses: the bus slot sends refer to. Master: 0.
    pub fn id(&self) -> i32 {
        self.id
    }

    /// Default strip colour per kind (0xAARRGGBB).
    pub fn default_colour(kind: StripKind) -> u32 {
        match kind {
            StripKind::Input => 0xff3d7ab8,
            StripKind::Aux => 0xff3daa6e,
            StripKind::Group => 0xffb8863d,
            StripKind::Master => 0xffb83d4f,
        }
    }

    pub fn name(&self) -> String {
        self.name.lock().unwrap_or_else(|e| e.into_inner()).clone()
    }

    pub fn set_name(&self, name: &str) {
        *self.name.lock().unwrap_or_else(|e| e.into_inner()) = name.to_owned();
    }

    /// 0xAARRGGBB
    pub fn colour(&self) -> u32 {
        self.colour.load(Relaxed)
    }

    pub fn set_colour(&self, argb: u32) {
        self.colour.store(argb, Relaxed);
    }

    /// # Safety
    /// The audio thread must not be able to reach this strip (device stopped or
    /// strip not yet published).
    pub(crate) unsafe fn prepare(&self, sample_rate: f64, max_block: usize) {
        let d = &mut *self.dsp.get();
        d.sample_rate = sample_rate;
        d.max_block = max_block.max(1);
        for buffers in [&mut d.work, &mut d.pre, &mut d.post, &mut d.main] {
            for b in buffers.iter_mut() {
                b.clear();
                b.resize(d.max_block, 0.0); // zeroed: an idle strip relies on it
            }
        }
        d.idle = false;
        d.hpf.reset();
        d.last_hpf_freq = -1.0;
        d.gate.prepare(sample_rate);
        d.eq.prepare(sample_rate);
        d.comp.prepare(sample_rate);
        self.inserts.prepare(d.max_block);
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
    pub(crate) unsafe fn last_main_assign(&self) -> &mut f32 {
        &mut *self.last_main_assign.get()
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

    // ---- audio thread ---------------------------------------------------------

    /// Inputs: pull from the hardware inputs (indexed by hardware channel; null = inactive).
    ///
    /// # Safety
    /// Stage-1 task for this strip only. Non-null pointers must be valid for `n` samples.
    pub(crate) unsafe fn read_hardware_inputs(&self, hw: &[*const f32], n: usize) {
        let d = self.dsp_mut();
        let p = &self.params;
        let left = p.input_left.load(Relaxed);
        let mut right = p.input_right.load(Relaxed);
        d.num_channels = if p.stereo.load(Relaxed) { 2 } else { 1 };
        if d.num_channels == 2 && right < 0 && left >= 0 {
            right = left + 1; // stereo without an explicit right input: next channel
        }
        self.meters.current_width.store(d.num_channels as i32, Relaxed);

        let sources = [left, right];
        let source = |ch: usize| -> Option<*const f32> {
            let hw_index = sources[ch];
            (hw_index >= 0 && (hw_index as usize) < hw.len() && !hw[hw_index as usize].is_null()).then(|| hw[hw_index as usize])
        };

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

    /// Buses/master: clear the work buffer before sums are accumulated into it.
    ///
    /// # Safety
    /// Owning task only.
    pub(crate) unsafe fn begin_summed_block(&self, n: usize) {
        let d = self.dsp_mut();
        let stereo = self.kind == StripKind::Master || self.params.stereo.load(Relaxed);
        d.num_channels = if stereo { 2 } else { 1 };
        self.meters.current_width.store(d.num_channels as i32, Relaxed);
        for ch in 0..d.num_channels {
            d.work[ch][..n].fill(0.0);
        }
    }

    /// Runs the chain and output stage on the work buffer.
    ///
    /// # Safety
    /// Owning task only.
    pub(crate) unsafe fn process(&self, n: usize) {
        let d = self.dsp_mut();
        debug_assert!(n <= d.max_block);

        // Fast path: an input with nothing patched and no plugins can only output
        // silence, so skip its DSP entirely (big consoles with unused channels).
        if self.kind == StripKind::Input && !d.input_connected && self.inserts.is_empty() {
            if !d.idle {
                // Clear once on entering idle; nothing writes the buffers after that.
                for buffers in [&mut d.work, &mut d.pre, &mut d.post, &mut d.main] {
                    buffers.iter_mut().for_each(|b| b.fill(0.0));
                }
            }
            self.meters.gate_gain.store(1.0);
            self.meters.comp_reduction_db.store(0.0);
            d.idle = true;
            return;
        }

        if d.idle {
            d.reset_dsp(); // coming back from idle: no stale filter state
            d.idle = false;
        }

        if self.kind != StripKind::Input {
            for ch in 0..d.num_channels {
                self.meters.input_peak[ch].fetch_max(peak_level(&d.work[ch][..n]));
            }
        }

        self.run_chain(d, n);

        // Never let NaN/inf escape a strip: it would poison every bus it feeds.
        let finite = (0..d.num_channels).all(|ch| all_finite(&d.work[ch][..n]));
        if !finite {
            d.work.iter_mut().for_each(|b| b[..n].fill(0.0));
            d.reset_dsp();
            self.meters.dsp_faults.fetch_add(1, Relaxed);
        }

        self.run_output_stage(d, n);
    }

    fn run_chain(&self, d: &mut StripDsp, n: usize) {
        let p = &self.params;
        let nc = d.num_channels;

        // Trim + polarity
        let polarity = if p.polarity_invert.load(Relaxed) { -1.0 } else { 1.0 };
        let trim = db_to_gain(p.trim_db.load()) * polarity;
        for ch in 0..nc {
            apply_gain_ramp(&mut d.work[ch][..n], d.last_trim, trim);
        }
        d.last_trim = trim;

        // High-pass filter (12 dB/oct)
        if p.hpf_enabled.load(Relaxed) {
            let f = p.hpf_frequency.load();
            if !d.hpf_was_on {
                d.hpf.reset();
            }
            if f != d.last_hpf_freq {
                d.hpf.set_coefficients(BiquadCoeffs::high_pass(d.sample_rate, f as f64, std::f64::consts::FRAC_1_SQRT_2));
                d.last_hpf_freq = f;
            }
            for ch in 0..nc {
                d.hpf.process(&mut d.work[ch][..n], ch);
            }
            d.hpf_was_on = true;
        } else {
            d.hpf_was_on = false;
        }

        let [w0, w1] = &mut d.work;
        let mut chans: [&mut [f32]; 2] = [&mut w0[..n], &mut w1[..n]];
        let chans = &mut chans[..nc];

        d.gate.set_settings(p.gate.snapshot());
        d.gate.process(chans);
        self.meters.gate_gain.store(d.gate.last_block_min_gain());

        d.eq.set_settings(p.eq.snapshot());
        d.eq.process(chans);

        d.comp.set_settings(p.comp.snapshot());
        d.comp.process(chans);
        self.meters.comp_reduction_db.store(d.comp.last_block_gain_reduction_db());

        // SAFETY: this task owns the strip, so it owns the insert chain's scratch.
        unsafe { self.inserts.process(chans, d.sample_rate) };
    }

    fn run_output_stage(&self, d: &mut StripDsp, n: usize) {
        let p = &self.params;
        let nc = d.num_channels;
        let mute = if p.mute.load(Relaxed) { 0.0 } else { 1.0 };
        let fader = db_to_gain(p.fader_db.load());

        for ch in 0..nc {
            d.pre[ch][..n].copy_from_slice(&d.work[ch][..n]);
            apply_gain_ramp(&mut d.pre[ch][..n], d.last_mute, mute);
            d.post[ch][..n].copy_from_slice(&d.pre[ch][..n]);
            apply_gain_ramp(&mut d.post[ch][..n], d.last_fader, fader);
        }
        d.last_mute = mute;
        d.last_fader = fader;

        let pan = p.pan.load();
        (d.pan_left, d.pan_right) = if nc == 1 { constant_power_pan(pan) } else { stereo_balance(pan) };

        let right_src = if nc > 1 { 1 } else { 0 };
        let [m0, m1] = &mut d.main;
        m0[..n].copy_from_slice(&d.post[0][..n]);
        m1[..n].copy_from_slice(&d.post[right_src][..n]);
        apply_gain_ramp(&mut m0[..n], d.last_pan_left, d.pan_left);
        apply_gain_ramp(&mut m1[..n], d.last_pan_right, d.pan_right);
        d.last_pan_left = d.pan_left;
        d.last_pan_right = d.pan_right;

        self.meters.output_peak[0].fetch_max(peak_level(&m0[..n]));
        self.meters.output_peak[1].fetch_max(peak_level(&m1[..n]));
    }
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
    #[inline]
    pub(crate) fn post_chain(&self, ch: usize, n: usize) -> &[f32] {
        &self.work[ch][..n]
    }
    #[inline]
    pub(crate) fn pre_fader(&self, ch: usize, n: usize) -> &[f32] {
        &self.pre[ch][..n]
    }
    #[inline]
    pub(crate) fn post_fader(&self, ch: usize, n: usize) -> &[f32] {
        &self.post[ch][..n]
    }
    #[inline]
    pub(crate) fn main_out(&self, ch: usize, n: usize) -> &[f32] {
        &self.main[ch][..n]
    }
    #[inline]
    pub(crate) fn pan(&self) -> (f32, f32) {
        (self.pan_left, self.pan_right)
    }
    #[inline]
    pub(crate) fn work_mut(&mut self, ch: usize, n: usize) -> &mut [f32] {
        &mut self.work[ch][..n]
    }
}
