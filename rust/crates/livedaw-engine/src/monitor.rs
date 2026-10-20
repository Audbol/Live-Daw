//! Monitor path processing (MONITORS, manual ch. 7 pp. 80-82, figs. 72-73;
//! the WING's `/cfg/mon/1` PHONES and `/cfg/mon/2` SPEAKERS) and SOURCE
//! SOLO (SETUP > AUDIO, ROUTING > SOURCES).
//!
//! Each monitor path sums its solo bus, SOURCE (`params::MonitorSourceParams`)
//! and DIRECT IN (any input, aux, bus, main or matrix output at its own
//! level), then runs
//!
//! ```text
//! EQ (8 bands: low shelf, 6 PEQ, high shelf) ─ PAN / WIDTH ─ INVERT ─ DELAY
//!    ─ level (DIM, MUTE, MON DCAs, talk MON DIM) ─ MONO / SWAP ─ LIMITER ─► outputs
//! ```
//!
//! The EQ is the WING EQ of the buses (`livedaw_dsp::wing::eq`, model STD with
//! shelving outer bands), so its curves and smoothing are the console's. PAN,
//! WIDTH and INVERT form one 2x2 matrix crossfaded over a block; the delay
//! crossfades between its old and new tap when the distance changes; the
//! limiter is a stereo-linked peak limiter (instant attack, so nothing passes
//! the threshold) whose gain reduction is metered.
//!
//! SOURCE SOLO: with SOURCE SOLO set to CH39 or AUX7, soloing a source on
//! ROUTING > SOURCES patches it to that channel for as long as the solo lasts
//! (the channel's own patch is untouched) and solos the channel, so the
//! source is heard on the monitor paths like any solo (SOLO MODE applies).
//! The host's own SOLO key also starts it (firmware 1.12), so CLR SOLO ends it.

use crate::atomic::AtomicF32;
use crate::params::{StripKind, NUM_AUX, NUM_MAINS, NUM_MATRICES, WING_BUSES, WING_CHANNELS};
use crate::sources::{pair_first, source_index, source_mode, source_port, SourceTable};
use livedaw_dsp::biquad::BiquadCoeffs;
use livedaw_dsp::delay::SPEED_OF_SOUND;
use livedaw_dsp::util::{db_to_gain, gain_to_db, stereo_balance};
use livedaw_dsp::wing::eq::{self, WingEq};
use livedaw_dsp::wing::{model_index, ModelSpec, EQ_MODELS_BUS, MAX_EQ_PARAMS};
use std::sync::atomic::{AtomicBool, AtomicI32, Ordering::Relaxed};
use std::sync::OnceLock;

/// Monitor EQ bands: L (low shelf), 1-6 (PEQ), H (high shelf).
pub const MONITOR_EQ_BANDS: usize = 8;
/// Default (frequency, Q) of the bands (the bus WING EQ's).
pub const MONITOR_EQ_DEFAULTS: [(f32, f32); MONITOR_EQ_BANDS] =
    [(80.0, 0.7), (160.0, 1.0), (315.0, 1.0), (630.0, 1.0), (1250.0, 1.0), (2500.0, 1.0), (5000.0, 1.0), (12000.0, 0.7)];
/// Frequency range of each band (`/cfg/mon/n/eq/lsf` 20-2000 Hz, `hsf` 200-20000 Hz).
pub const MONITOR_EQ_FREQ_RANGE: [(f32, f32); MONITOR_EQ_BANDS] = [
    (20.0, 2000.0),
    (20.0, 20000.0),
    (20.0, 20000.0),
    (20.0, 20000.0),
    (20.0, 20000.0),
    (20.0, 20000.0),
    (20.0, 20000.0),
    (200.0, 20000.0),
];
pub const MONITOR_EQ_GAIN_RANGE: f32 = 15.0;
pub const MONITOR_EQ_Q_RANGE: (f32, f32) = (0.442, 10.0);
/// The shelves' Q (the console has no Q control for them).
const SHELF_Q: f32 = 0.7;
/// PAN (-100..100) and WIDTH (-150..150 %).
pub const MONITOR_WIDTH_RANGE: f32 = 150.0;
/// DELAY distance (`dly/m`), meters.
pub const MONITOR_DELAY_RANGE_M: (f32, f32) = (0.1, 100.0);
/// LIMITER threshold, dB.
pub const MONITOR_LIMITER_RANGE_DB: (f32, f32) = (-40.0, 0.0);
/// BAND SOLO boost (`eqbdtrim`), dB.
pub const BAND_SOLO_MAX_DB: f32 = 24.0;
pub const BAND_SOLO_DEFAULT_DB: f32 = 12.0;

/// Milliseconds of delay for a distance in meters.
pub fn delay_ms(meters: f32) -> f32 {
    meters / SPEED_OF_SOUND * 1000.0
}

// ---- DIRECT IN ---------------------------------------------------------------------

/// DIRECT IN choices (`MonitorDspParams::direct_source`, the WING's `dirin`
/// without OFF = -1): CH.1-40, AUX.1-8, BUS.1-16, MAIN.1-4, MTX.1-8.
pub const NUM_DIRECT_SOURCES: usize = WING_CHANNELS + NUM_AUX + WING_BUSES + NUM_MAINS + NUM_MATRICES;

/// The strip a DIRECT IN index plays: (kind, 0-based number; buses by slot).
pub fn direct_source(index: i32) -> Option<(StripKind, usize)> {
    let mut i = usize::try_from(index).ok()?;
    for (kind, count) in [
        (StripKind::Channel, WING_CHANNELS),
        (StripKind::Aux, NUM_AUX),
        (StripKind::Bus, WING_BUSES),
        (StripKind::Main, NUM_MAINS),
        (StripKind::Matrix, NUM_MATRICES),
    ] {
        if i < count {
            return Some((kind, i));
        }
        i -= count;
    }
    None
}

/// The DIRECT IN index of a strip (kind, 0-based number), or -1.
pub fn direct_index(kind: StripKind, number: usize) -> i32 {
    let mut base = 0;
    for (k, count) in [
        (StripKind::Channel, WING_CHANNELS),
        (StripKind::Aux, NUM_AUX),
        (StripKind::Bus, WING_BUSES),
        (StripKind::Main, NUM_MAINS),
        (StripKind::Matrix, NUM_MATRICES),
    ] {
        if k == kind {
            return if number < count { (base + number) as i32 } else { -1 };
        }
        base += count;
    }
    -1
}

/// The display name of a DIRECT IN index ("CH.12", "AUX.6", "OFF").
pub fn direct_source_name(index: i32) -> String {
    match direct_source(index) {
        Some((StripKind::Channel, i)) => format!("CH.{}", i + 1),
        Some((StripKind::Matrix, i)) => format!("MTX.{}", i + 1),
        Some((kind, i)) => format!("{}.{}", kind.label(), i + 1),
        None => "OFF".into(),
    }
}

// ---- SOURCE SOLO -------------------------------------------------------------------

/// SOURCE SOLO (`MonitorControls::source_solo`): the channel that hosts a
/// soloed source.
pub const SOURCE_SOLO_HOSTS: [&str; 3] = ["OFF", "CH39", "AUX7"];

/// The host strip of a SOURCE SOLO choice: (kind, 0-based number).
pub fn source_solo_host(choice: i32) -> Option<(StripKind, usize)> {
    match choice {
        1 => Some((StripKind::Channel, 38)),
        2 => Some((StripKind::Aux, 6)),
        _ => None,
    }
}

/// What the SOURCE SOLO host plays this block: its input ports and width.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SoloPatch {
    pub left: i32,
    pub right: i32,
    pub stereo: bool,
}

/// The ports a soloed source patches to its host: a paired source (STEREO,
/// MID/SIDE) plays both ports in stereo, a mono source alone.
pub fn solo_patch(table: &SourceTable, port: i32) -> Option<SoloPatch> {
    let index = source_index(port)?;
    let p = table.get(index)?;
    if p.mode.load(Relaxed) == source_mode::MONO {
        return Some(SoloPatch { left: port, right: -1, stereo: false });
    }
    let first = pair_first(index);
    Some(SoloPatch { left: source_port(first)?, right: source_port(first + 1)?, stereo: true })
}

// ---- parameters --------------------------------------------------------------------

/// One monitor EQ band (shelves ignore the Q).
#[repr(C)]
#[derive(Debug)]
pub struct MonitorEqBand {
    pub gain_db: AtomicF32,
    pub freq: AtomicF32,
    pub q: AtomicF32,
}

/// A monitor path's processing (MONITORS > PHONES / SPEAKERS SETTINGS, the
/// MONITORS EQ and LEVEL), shared with the UI.
#[repr(C)]
#[derive(Debug)]
pub struct MonitorDspParams {
    /// DIRECT IN ([`direct_source`]; -1 = OFF), mixed in at `direct_level_db`.
    pub direct_source: AtomicI32,
    pub direct_level_db: AtomicF32,
    /// BAND SOLO: the boost of an EQ band solo (LISTEN) on this path, 0..24 dB.
    pub band_solo_db: AtomicF32,
    pub eq_on: AtomicBool,
    /// INVERT: polarity of both sides.
    pub invert: AtomicBool,
    /// DELAY on, by `delay_m` meters.
    pub delay_on: AtomicBool,
    pub eq: [MonitorEqBand; MONITOR_EQ_BANDS],
    /// PAN (-100..100) and WIDTH (-150..150 %).
    pub pan: AtomicF32,
    pub width: AtomicF32,
    pub delay_m: AtomicF32,
    /// LIMITER threshold (-40..0 dB).
    pub limiter_db: AtomicF32,
    /// Meter: the limiter's largest gain reduction since the last read (dB, >= 0).
    pub limiter_gr_db: AtomicF32,
}

impl Default for MonitorDspParams {
    fn default() -> Self {
        let band = |i: usize| MonitorEqBand {
            gain_db: AtomicF32::new(0.0),
            freq: AtomicF32::new(MONITOR_EQ_DEFAULTS[i].0),
            q: AtomicF32::new(MONITOR_EQ_DEFAULTS[i].1),
        };
        Self {
            direct_source: AtomicI32::new(-1),
            direct_level_db: AtomicF32::new(0.0),
            band_solo_db: AtomicF32::new(BAND_SOLO_DEFAULT_DB),
            eq_on: AtomicBool::new(false),
            invert: AtomicBool::new(false),
            delay_on: AtomicBool::new(false),
            eq: std::array::from_fn(band),
            pan: AtomicF32::new(0.0),
            width: AtomicF32::new(100.0),
            delay_m: AtomicF32::new(MONITOR_DELAY_RANGE_M.0),
            limiter_db: AtomicF32::new(0.0),
            limiter_gr_db: AtomicF32::new(0.0),
        }
    }
}

impl MonitorDspParams {
    /// Back to the defaults.
    pub fn reset(&self) {
        self.copy_from(&Self::default());
    }

    /// Copies every setting (not the meter).
    pub fn copy_from(&self, o: &MonitorDspParams) {
        self.direct_source.store(o.direct_source.load(Relaxed), Relaxed);
        self.direct_level_db.store(o.direct_level_db.load());
        self.band_solo_db.store(o.band_solo_db.load());
        self.eq_on.store(o.eq_on.load(Relaxed), Relaxed);
        self.invert.store(o.invert.load(Relaxed), Relaxed);
        self.delay_on.store(o.delay_on.load(Relaxed), Relaxed);
        for (a, b) in self.eq.iter().zip(&o.eq) {
            a.gain_db.store(b.gain_db.load());
            a.freq.store(b.freq.load());
            a.q.store(b.q.load());
        }
        self.pan.store(o.pan.load());
        self.width.store(o.width.load());
        self.delay_m.store(o.delay_m.load());
        self.limiter_db.store(o.limiter_db.load());
    }

    /// The EQ as the bus WING EQ's (STD) parameter values, flat when off.
    pub fn eq_values(&self, out: &mut [f32; MAX_EQ_PARAMS]) {
        let ids = std_ids();
        let spec = &EQ_MODELS_BUS[ids.model];
        spec.defaults_into(out);
        let finite = |v: f32, d: f32| if v.is_finite() { v } else { d };
        for (i, b) in self.eq.iter().enumerate() {
            let (lo, hi) = MONITOR_EQ_FREQ_RANGE[i];
            let [g, f, q] = ids.bands[i];
            out[g] = finite(b.gain_db.load(), 0.0).clamp(-MONITOR_EQ_GAIN_RANGE, MONITOR_EQ_GAIN_RANGE);
            out[f] = finite(b.freq.load(), MONITOR_EQ_DEFAULTS[i].0).clamp(lo, hi);
            out[q] = if i == 0 || i == MONITOR_EQ_BANDS - 1 {
                SHELF_Q
            } else {
                finite(b.q.load(), 1.0).clamp(MONITOR_EQ_Q_RANGE.0, MONITOR_EQ_Q_RANGE.1)
            };
        }
        out[ids.low_type] = 1.0; // SHV
        out[ids.high_type] = 1.0;
    }

    /// The EQ's filters as set (for its graph; empty when off).
    pub fn eq_design(&self, sample_rate: f64) -> eq::EqDesign {
        if !self.eq_on.load(Relaxed) {
            return eq::EqDesign::default();
        }
        let mut v = [0.0; MAX_EQ_PARAMS];
        self.eq_values(&mut v);
        eq::design(&EQ_MODELS_BUS[std_ids().model], &v, sample_rate, 1.0)
    }

    /// One band's filter as set (None when flat), for its own curve.
    pub fn band_design(&self, band: usize, sample_rate: f64) -> Option<BiquadCoeffs> {
        let mut v = [0.0; MAX_EQ_PARAMS];
        self.eq_values(&mut v);
        let [g, f, q] = std_ids().bands.get(band)?;
        let (g, f, q) = (v[*g] as f64, v[*f] as f64, v[*q] as f64);
        if g.abs() < 1e-3 {
            return None;
        }
        Some(match band {
            0 => BiquadCoeffs::low_shelf(sample_rate, f, q, g),
            b if b == MONITOR_EQ_BANDS - 1 => BiquadCoeffs::high_shelf(sample_rate, f, q, g),
            _ => BiquadCoeffs::peak(sample_rate, f, q, g),
        })
    }
}

/// The monitor processing of both paths and SOURCE SOLO, shared with the UI
/// (`EngineControls::monitor`).
#[repr(C)]
#[derive(Debug)]
pub struct MonitorControls {
    /// Path 0 = SPEAKERS, 1 = PHONES (as the engine's monitor buffers).
    pub paths: [MonitorDspParams; 2],
    /// SOURCE SOLO (SETUP > AUDIO): [`SOURCE_SOLO_HOSTS`] index.
    pub source_solo: AtomicI32,
    /// A source is soloed (the WING's `$srcsolo`; not saved). The host's own
    /// SOLO does the same.
    pub source_solo_on: AtomicBool,
    /// The soloed source's input port (a hardware input or a playback channel).
    pub source_solo_port: AtomicI32,
}

impl Default for MonitorControls {
    fn default() -> Self {
        Self {
            paths: Default::default(),
            source_solo: AtomicI32::new(0),
            source_solo_on: AtomicBool::new(false),
            source_solo_port: AtomicI32::new(-1),
        }
    }
}

impl MonitorControls {
    pub fn reset(&self) {
        for p in &self.paths {
            p.reset();
        }
        self.source_solo.store(0, Relaxed);
        self.source_solo_on.store(false, Relaxed);
        self.source_solo_port.store(-1, Relaxed);
    }

    /// The SOURCE SOLO host (kind, 0-based number), None while OFF.
    pub fn host(&self) -> Option<(StripKind, usize)> {
        source_solo_host(self.source_solo.load(Relaxed))
    }

    /// The patch SOURCE SOLO puts on its host this block, if a source is
    /// soloed (`source_solo_on`, or `host_solo`: the host's own SOLO).
    pub fn source_solo_patch(&self, table: &SourceTable, host_solo: bool) -> Option<SoloPatch> {
        if !(self.source_solo_on.load(Relaxed) || host_solo) {
            return None;
        }
        solo_patch(table, self.source_solo_port.load(Relaxed))
    }
}

/// Parameter positions of the bus WING EQ (STD) the monitor EQ drives.
struct StdIds {
    model: usize,
    bands: [[usize; 3]; MONITOR_EQ_BANDS],
    low_type: usize,
    high_type: usize,
}

fn std_ids() -> &'static StdIds {
    static IDS: OnceLock<StdIds> = OnceLock::new();
    IDS.get_or_init(|| {
        let model = model_index(EQ_MODELS_BUS, "STD").unwrap_or(0);
        let spec: &ModelSpec = &EQ_MODELS_BUS[model];
        let at = |id: &str| spec.param_index(id).unwrap_or(0);
        let names = ["l", "1", "2", "3", "4", "5", "6", "h"];
        StdIds {
            model,
            bands: std::array::from_fn(|i| {
                [at(&format!("{}g", names[i])), at(&format!("{}f", names[i])), at(&format!("{}q", names[i]))]
            }),
            low_type: at("leq"),
            high_type: at("heq"),
        }
    })
}

// ---- audio thread ------------------------------------------------------------------

/// Monitor matrices (out_l = m[0]*L + m[1]*R, out_r = m[2]*L + m[3]*R).
const IDENTITY: [f32; 4] = [1.0, 0.0, 0.0, 1.0];

/// PAN, WIDTH and INVERT as one matrix: width as mid/side (1 = as is, 0 =
/// mono, negative swaps the sides), then the pan as a balance.
fn image_matrix(pan: f32, width: f32, invert: bool) -> [f32; 4] {
    let w = (if width.is_finite() { width } else { 100.0 }).clamp(-MONITOR_WIDTH_RANGE, MONITOR_WIDTH_RANGE) / 100.0;
    let (gl, gr) = stereo_balance((if pan.is_finite() { pan } else { 0.0 }) / 100.0);
    let sign = if invert { -1.0 } else { 1.0 };
    let (a, b) = ((1.0 + w) * 0.5, (1.0 - w) * 0.5);
    [sign * gl * a, sign * gl * b, sign * gr * b, sign * gr * a]
}

/// Applies a 2x2 matrix, crossfading from `last` to `target` over the block.
fn apply_matrix(l: &mut [f32], r: &mut [f32], last: [f32; 4], target: [f32; 4]) {
    if last == IDENTITY && target == IDENTITY {
        return;
    }
    let step = 1.0 / l.len().max(1) as f32;
    let mut t = 0.0;
    for (x, y) in l.iter_mut().zip(r.iter_mut()) {
        t += step;
        let m: [f32; 4] = if last == target { target } else { std::array::from_fn(|i| last[i] + (target[i] - last[i]) * t) };
        let (a, b) = (*x, *y);
        *x = m[0] * a + m[1] * b;
        *y = m[2] * a + m[3] * b;
    }
}

/// A stereo delay line that crossfades between its old and new tap when the
/// delay changes (no clicks when the distance moves or DELAY switches).
#[derive(Default)]
struct MonitorDelay {
    buf: [Vec<f32>; 2],
    pos: usize,
    /// The delay of the last block, samples.
    current: usize,
}

impl MonitorDelay {
    fn prepare(&mut self, sample_rate: f64, block: usize) {
        let max = (delay_ms(MONITOR_DELAY_RANGE_M.1) as f64 * 0.001 * sample_rate).ceil() as usize;
        let len = max + block.max(1) + 2;
        for b in &mut self.buf {
            b.clear();
            b.resize(len, 0.0);
        }
        self.pos = 0;
        self.current = 0;
    }

    fn reset(&mut self) {
        for b in &mut self.buf {
            b.fill(0.0);
        }
        self.current = 0;
    }

    /// Delays both sides in place by `target` samples.
    fn process(&mut self, l: &mut [f32], r: &mut [f32], target: usize) {
        let len = self.buf[0].len();
        let n = l.len().min(r.len());
        if len < n + 2 {
            return;
        }
        let target = target.min(len - n - 1);
        let from = self.current;
        if from == 0 && target == 0 {
            // Keep the line filled, so switching DELAY on fades into real signal.
            for (ch, data) in [&*l, &*r].into_iter().enumerate() {
                let buf = &mut self.buf[ch];
                let mut w = self.pos;
                for &x in &data[..n] {
                    buf[w] = x;
                    w = if w + 1 == len { 0 } else { w + 1 };
                }
            }
            self.pos = (self.pos + n) % len;
            return;
        }
        let step = 1.0 / n.max(1) as f32;
        for (ch, data) in [l, r].into_iter().enumerate() {
            let buf = &mut self.buf[ch];
            let mut w = self.pos;
            let mut t = 0.0;
            for x in data[..n].iter_mut() {
                buf[w] = *x;
                let tap = |d: usize| buf[(w + len - d) % len];
                *x = if from == target {
                    tap(target)
                } else {
                    t += step;
                    tap(from) * (1.0 - t) + tap(target) * t
                };
                w = if w + 1 == len { 0 } else { w + 1 };
            }
        }
        self.pos = (self.pos + n) % len;
        self.current = target;
    }
}

/// Stereo-linked peak limiter: instant attack, smooth release.
struct Limiter {
    gain: f32,
    release: f32,
}

impl Default for Limiter {
    fn default() -> Self {
        Self { gain: 1.0, release: 0.0 }
    }
}

impl Limiter {
    const RELEASE_MS: f64 = 80.0;

    fn prepare(&mut self, sample_rate: f64) {
        self.release = (1.0 - (-1.0 / (Self::RELEASE_MS * 0.001 * sample_rate)).exp()) as f32;
        self.gain = 1.0;
    }

    /// Limits in place; returns the smallest gain of the block.
    fn process(&mut self, l: &mut [f32], r: &mut [f32], threshold: f32) -> f32 {
        let mut min_gain = 1.0f32;
        if self.gain >= 1.0 && l.iter().chain(r.iter()).all(|x| x.abs() <= threshold) {
            return 1.0;
        }
        for (x, y) in l.iter_mut().zip(r.iter_mut()) {
            let peak = x.abs().max(y.abs());
            let wanted = if peak > threshold { threshold / peak } else { 1.0 };
            self.gain = if wanted < self.gain { wanted } else { self.gain + (wanted - self.gain) * self.release };
            // (f32 steps near 1 are too coarse for the release to get there.)
            if self.gain > 0.9998 {
                self.gain = 1.0;
            }
            *x *= self.gain;
            *y *= self.gain;
            min_gain = min_gain.min(self.gain);
        }
        min_gain
    }
}

/// How long the EQ keeps running after it is switched off (its glide to flat
/// takes ~20 sub-blocks of 32 samples).
const EQ_SETTLE_SAMPLES: usize = 4096;

/// One path's processing state (audio thread).
pub(crate) struct MonitorPathDsp {
    sr: f64,
    eq: WingEq,
    /// Samples since the EQ was switched off (it glides to flat first).
    eq_off_samples: usize,
    image: [f32; 4],
    delay: MonitorDelay,
    limiter: Limiter,
    /// DIRECT IN's gain and source of the last block.
    pub direct_gain: f32,
    pub direct_source: i32,
    /// BAND SOLO's gain of the last block.
    pub band_gain: f32,
}

impl Default for MonitorPathDsp {
    fn default() -> Self {
        let mut d = Self {
            sr: 48000.0,
            eq: WingEq::default(),
            eq_off_samples: EQ_SETTLE_SAMPLES,
            image: IDENTITY,
            delay: MonitorDelay::default(),
            limiter: Limiter::default(),
            direct_gain: 0.0,
            direct_source: -1,
            band_gain: 1.0,
        };
        d.prepare(48000.0, 512);
        d
    }
}

impl MonitorPathDsp {
    /// Allocates for a device (message thread, device stopped).
    pub fn prepare(&mut self, sample_rate: f64, block: usize) {
        // The EQ's parameter positions are looked up once, here (they allocate),
        // never on the audio thread.
        std_ids();
        self.sr = sample_rate;
        self.eq.prepare(sample_rate);
        self.eq_off_samples = EQ_SETTLE_SAMPLES;
        self.image = IDENTITY;
        self.delay.prepare(sample_rate, block);
        self.limiter.prepare(sample_rate);
    }

    pub fn reset(&mut self) {
        self.eq.reset();
        self.delay.reset();
        self.limiter.gain = 1.0;
    }

    /// The EQ: the first stage before the level (the RTA of MONITORS EQ
    /// reads either side of it), then `process_image`.
    pub fn process_eq(&mut self, p: &MonitorDspParams, l: &mut [f32], r: &mut [f32]) {
        let on = p.eq_on.load(Relaxed);
        if on || self.eq_off_samples < EQ_SETTLE_SAMPLES {
            let mut v = [0.0; MAX_EQ_PARAMS];
            p.eq_values(&mut v);
            self.eq.process(&mut [&mut *l, &mut *r], EQ_MODELS_BUS, std_ids().model, on, 100.0, &v);
            // Switched off, WingEq glides its gains to flat first.
            self.eq_off_samples = if on { 0 } else { self.eq_off_samples + l.len() };
        }
    }

    /// PAN / WIDTH / INVERT and DELAY, after the EQ.
    pub fn process_image(&mut self, p: &MonitorDspParams, l: &mut [f32], r: &mut [f32]) {
        let image = image_matrix(p.pan.load(), p.width.load(), p.invert.load(Relaxed));
        apply_matrix(l, r, self.image, image);
        self.image = image;

        let samples = if p.delay_on.load(Relaxed) {
            let m = p.delay_m.load();
            let m = if m.is_finite() { m } else { 0.0 }.clamp(MONITOR_DELAY_RANGE_M.0, MONITOR_DELAY_RANGE_M.1);
            (delay_ms(m) as f64 * 0.001 * self.sr).round() as usize
        } else {
            0
        };
        self.delay.process(l, r, samples);
    }

    /// The LIMITER, after the level: returns the gain reduction in dB.
    pub fn process_limiter(&mut self, p: &MonitorDspParams, l: &mut [f32], r: &mut [f32]) -> f32 {
        let db = p.limiter_db.load();
        let db = if db.is_finite() { db } else { 0.0 }.clamp(MONITOR_LIMITER_RANGE_DB.0, MONITOR_LIMITER_RANGE_DB.1);
        let g = self.limiter.process(l, r, db_to_gain(db));
        if g < 1.0 {
            -gain_to_db(g)
        } else {
            0.0
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn direct_sources_round_trip() {
        assert_eq!(direct_source(-1), None);
        assert_eq!(direct_source(0), Some((StripKind::Channel, 0)));
        assert_eq!(direct_source(45), Some((StripKind::Aux, 5)));
        assert_eq!(direct_source(NUM_DIRECT_SOURCES as i32 - 1), Some((StripKind::Matrix, 7)));
        assert_eq!(direct_source(NUM_DIRECT_SOURCES as i32), None);
        for i in 0..NUM_DIRECT_SOURCES as i32 {
            let (k, n) = direct_source(i).unwrap();
            assert_eq!(direct_index(k, n), i);
        }
        assert_eq!(direct_source_name(45), "AUX.6");
        assert_eq!(direct_source_name(48), "BUS.1");
        assert_eq!(direct_source_name(70), "MTX.3");
        assert_eq!(direct_source_name(-1), "OFF");
    }

    #[test]
    fn image_matrix_is_identity_by_default() {
        assert_eq!(image_matrix(0.0, 100.0, false), IDENTITY);
        assert_eq!(image_matrix(0.0, 0.0, false), [0.5, 0.5, 0.5, 0.5]);
        assert_eq!(image_matrix(0.0, -100.0, false), [0.0, 1.0, 1.0, 0.0]);
        assert_eq!(image_matrix(0.0, 100.0, true), [-1.0, -0.0, -0.0, -1.0]);
        let m = image_matrix(-100.0, 100.0, false);
        assert_eq!((m[0], m[3]), (1.0, 0.0));
    }

    #[test]
    fn delay_crossfades_to_its_new_tap() {
        let mut d = MonitorDelay::default();
        d.prepare(48000.0, 64);
        let mut l = vec![1.0; 64];
        let mut r = vec![1.0; 64];
        d.process(&mut l, &mut r, 0);
        assert!(l.iter().all(|&x| x == 1.0));
        // 10 samples: the first block fades from the dry tap to the delayed one.
        let (mut l, mut r) = (vec![1.0; 64], vec![1.0; 64]);
        d.process(&mut l, &mut r, 10);
        assert!(l.iter().all(|&x| (x - 1.0).abs() < 1e-6), "the line was kept filled");
        // An impulse comes out 10 samples later.
        let (mut l, mut r) = (vec![0.0; 64], vec![0.0; 64]);
        for _ in 0..3 {
            d.process(&mut l, &mut r, 10);
            l.fill(0.0);
            r.fill(0.0);
        }
        l[0] = 1.0;
        d.process(&mut l, &mut r, 10);
        assert_eq!(l[10], 1.0);
        assert_eq!(l[0], 0.0);
    }

    #[test]
    fn limiter_holds_the_threshold() {
        let mut lim = Limiter::default();
        lim.prepare(48000.0);
        let t = db_to_gain(-6.0);
        let mut l: Vec<f32> = (0..256).map(|i| (i as f32 * 0.1).sin()).collect();
        let mut r = l.clone();
        let g = lim.process(&mut l, &mut r, t);
        assert!(g < 0.6, "{g}");
        assert!(l.iter().chain(&r).all(|x| x.abs() <= t + 1e-6));
        // Below the threshold it gets back to unity.
        let mut l = vec![0.01; 48000];
        let mut r = l.clone();
        lim.process(&mut l, &mut r, t);
        assert_eq!(lim.gain, 1.0);
    }

    #[test]
    fn eq_values_drive_the_bus_wing_eq() {
        let p = MonitorDspParams::default();
        p.eq_on.store(true, Relaxed);
        assert_eq!(p.eq_design(48000.0).len, 0, "flat");
        p.eq[3].gain_db.store(6.0);
        p.eq[3].freq.store(1000.0);
        let d = p.eq_design(48000.0);
        assert!((d.response_db(1000.0, 48000.0) - 6.0).abs() < 0.1);
        assert!(p.band_design(3, 48000.0).is_some() && p.band_design(2, 48000.0).is_none());
        // The low shelf can't go above 2 kHz.
        p.eq[0].freq.store(5000.0);
        let mut v = [0.0; MAX_EQ_PARAMS];
        p.eq_values(&mut v);
        assert_eq!(v[std_ids().bands[0][1]], 2000.0);
        p.eq_on.store(false, Relaxed);
        assert_eq!(p.eq_design(48000.0).len, 0);
    }
}
