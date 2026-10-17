//! Analysis and listening on one strip at a time (WING RTA, LR VECTOR SCOPE,
//! LISTEN / BAND SOLO / KEY SOLO / XOVER SOLO).
//!
//! * The audio side only copies samples: the analysed strip pushes its signal
//!   at the RTA tap (mono) and after its INPUT block (L/R, decimated) into
//!   lock-free rings ([`SampleRing`]).
//! * A non-realtime thread (started on first use, see
//!   [`Analyser::ensure_thread`]) reads the RTA ring and computes 120 semitone
//!   bands, E0 (20.6 Hz) to D#10 (19.9 kHz): the piano-key ruler the console
//!   draws over its graphs. Two FFT resolutions cover them ([`RtaCore`]); the
//!   detector (PEAK / RMS / AVG), the decay (SLOW / MED / FAST) and a peak
//!   hold follow. The results are atomics the UI reads (`ld_rta_read`).
//! * LISTEN replaces the solo bus, console-wide and for one strip at a time,
//!   with the selected EQ band (band-passed), a dynamics slot's key signal
//!   (after the key filter) or the compressor's crossover band. Switching
//!   fades out, then in, so it never clicks.

use crate::atomic::AtomicF32;
use crate::fft::Fft;
use crate::strip::Strip;
use livedaw_dsp::biquad::{Biquad, BiquadCoeffs};
use livedaw_dsp::util::{add_with_gain_ramp, db_to_gain};
use std::cell::UnsafeCell;
use std::sync::atomic::{fence, AtomicBool, AtomicI32, AtomicU32, AtomicU64, Ordering::*};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

// ---- constants ----------------------------------------------------------------------

/// RTA bands: one per semitone (MIDI notes 16..=135).
pub const RTA_BANDS: usize = 120;
const RTA_FIRST_NOTE: i32 = 16;
/// Level of an empty band.
pub const RTA_FLOOR_DB: f32 = -144.0;

/// Centre frequency of RTA band `band` (a piano key: E0 = 20.6 Hz ... D#10).
pub fn rta_band_hz(band: usize) -> f32 {
    440.0 * 2f32.powf((RTA_FIRST_NOTE + band as i32 - 69) as f32 / 12.0)
}

/// Where the RTA reads the analysed strip.
pub mod rta_tap {
    /// The raw source (before trim).
    pub const IN: usize = 0;
    pub const PRE_FILTER: usize = 1;
    pub const POST_FILTER: usize = 2;
    pub const PRE_EQ: usize = 3;
    pub const POST_EQ: usize = 4;
    /// After the fader.
    pub const POST_FADER: usize = 5;
}

/// The graph showing the RTA: it decides the tap and the settings used.
pub mod rta_view {
    /// HOME > OVERVIEW (RTA SETTINGS: IN | EQ | POST).
    pub const HOME: i32 = 0;
    /// The EQ screen (EQ/RTA SETTINGS: PRE | POST the EQ).
    pub const EQ: i32 = 1;
    /// FILTER SETUP (PRE | POST the filter; the EQ view's other settings).
    pub const FILTER: i32 = 2;
}

/// LISTEN modes (`AnalysisControls::listen_mode`).
pub mod listen_mode {
    pub const OFF: i32 = 0;
    /// BAND SOLO / LISTEN: the selected EQ band, band-passed.
    pub const BAND: i32 = 1;
    /// KEY SOLO of the GATE slot: its key after the key filter.
    pub const GATE_KEY: i32 = 2;
    /// KEY SOLO of the DYNAMICS slot.
    pub const DYN_KEY: i32 = 3;
    /// XOVER SOLO: the DYNAMICS slot's crossover band.
    pub const XOVER: i32 = 4;
}

/// RTA DECAY: how fast a bar falls, in dB per second (SLOW, MED, FAST).
pub const RTA_DECAY_DB_PER_S: [f32; 3] = [10.0, 25.0, 60.0];
/// RTA DETECTOR time constants in seconds (RMS: power, AVG: magnitude).
const RMS_SECONDS: f32 = 0.3;
const AVG_SECONDS: f32 = 1.0;
/// Peak hold time before the hold mark falls.
const HOLD_SECONDS: f32 = 1.5;
/// The scope keeps about this many L/R pairs per second.
const SCOPE_RATE: f64 = 24000.0;
/// The analysis thread idles when the bands weren't read for this long.
const UNREAD_MS: u64 = 2000;

// ---- the shared controls -------------------------------------------------------------

/// The settings of one RTA view (the WING's `/cfg/rta` and `/$ctl/cfg/rta`).
#[repr(C)]
#[derive(Debug)]
pub struct RtaViewSettings {
    /// SIZE/MODE. HOME: 0 OFF, 1 1/3, 2 FULL. EQ: 0 OFF, 1 1/4, 2 1/3, 3 1/2,
    /// 4 OVL/3, 5 OVL.
    pub display: AtomicI32,
    /// COLOR: 0..8 = RD25, RD50, RD75, AM25, AM50, AM75, BL25, BL50, BL75.
    pub colour: AtomicI32,
    /// RTA SOURCE. HOME: 0 IN, 1 EQ, 2 POST. EQ: 0 PRE, 1 POST.
    pub tap: AtomicI32,
    /// RTA RANGE: 0 = 30 dB, 1 = 60 dB.
    pub range: AtomicI32,
    /// RTA DECAY: 0 SLOW, 1 MED, 2 FAST.
    pub decay: AtomicI32,
    /// RTA DETECTOR: 0 PEAK, 1 RMS, 2 AVG.
    pub detector: AtomicI32,
    /// RTA AUTOGAIN: the UI centres the bars vertically.
    pub autogain: AtomicBool,
    /// FIXED GAIN, -5..50 dB.
    pub gain_db: AtomicF32,
}

impl RtaViewSettings {
    fn new(display: i32, tap: i32) -> Self {
        // The console's defaults (manual figs. 24-25).
        Self {
            display: AtomicI32::new(display),
            colour: AtomicI32::new(8),
            tap: AtomicI32::new(tap),
            range: AtomicI32::new(1),
            decay: AtomicI32::new(2),
            detector: AtomicI32::new(2),
            autogain: AtomicBool::new(true),
            gain_db: AtomicF32::new(0.0),
        }
    }

    fn reset_to(&self, d: &RtaViewSettings) {
        for (a, b) in [(&self.display, &d.display), (&self.colour, &d.colour), (&self.tap, &d.tap), (&self.range, &d.range)] {
            a.store(b.load(Relaxed), Relaxed);
        }
        self.decay.store(d.decay.load(Relaxed), Relaxed);
        self.detector.store(d.detector.load(Relaxed), Relaxed);
        self.autogain.store(d.autogain.load(Relaxed), Relaxed);
        self.gain_db.store(d.gain_db.load());
    }
}

/// Analysis and listening controls, shared with the UI like `EngineControls`.
#[repr(C)]
#[derive(Debug)]
pub struct AnalysisControls {
    /// The analysed strip (the selected strip): `StripKind` and `Strip::id()`;
    /// -1 = none.
    pub strip_kind: AtomicI32,
    pub strip_id: AtomicI32,
    /// An RTA is shown (the analysis thread runs only then).
    pub rta_on: AtomicBool,
    /// FREEZE: the bands stop updating.
    pub rta_freeze: AtomicBool,
    /// The LR VECTOR SCOPE is shown.
    pub scope_on: AtomicBool,
    /// The graph showing the RTA (`rta_view`).
    pub view: AtomicI32,
    /// LISTEN (`listen_mode`), for one strip (kind, id) and EQ band (an index
    /// into the WING EQ's bands as the EQ screen lists them).
    pub listen_mode: AtomicI32,
    pub listen_kind: AtomicI32,
    pub listen_id: AtomicI32,
    pub listen_band: AtomicI32,
    /// BAND SOLO trim (0..24 dB, the WING's `/cfg/mon/n/eqbdtrim`).
    pub band_solo_trim_db: AtomicF32,
    /// RTA SETTINGS (HOME) and EQ/RTA SETTINGS (EQ and FILTER).
    pub home: RtaViewSettings,
    pub eq: RtaViewSettings,
    /// FILTER SETUP's RTA SOURCE: 0 PRE, 1 POST the filter.
    pub filter_tap: AtomicI32,
}

impl Default for AnalysisControls {
    fn default() -> Self {
        Self {
            strip_kind: AtomicI32::new(-1),
            strip_id: AtomicI32::new(-1),
            rta_on: AtomicBool::new(false),
            rta_freeze: AtomicBool::new(false),
            scope_on: AtomicBool::new(false),
            view: AtomicI32::new(rta_view::HOME),
            listen_mode: AtomicI32::new(listen_mode::OFF),
            listen_kind: AtomicI32::new(-1),
            listen_id: AtomicI32::new(-1),
            listen_band: AtomicI32::new(0),
            band_solo_trim_db: AtomicF32::new(12.0),
            home: RtaViewSettings::new(2, 1),
            eq: RtaViewSettings::new(1, 1),
            filter_tap: AtomicI32::new(1),
        }
    }
}

impl AnalysisControls {
    /// The settings the current view uses.
    pub fn view_settings(&self) -> &RtaViewSettings {
        if self.view.load(Relaxed) == rta_view::HOME {
            &self.home
        } else {
            &self.eq
        }
    }

    /// Where the RTA reads the strip for the current view.
    pub fn engine_tap(&self) -> usize {
        match self.view.load(Relaxed) {
            rta_view::EQ => [rta_tap::PRE_EQ, rta_tap::POST_EQ][self.eq.tap.load(Relaxed).clamp(0, 1) as usize],
            rta_view::FILTER => [rta_tap::PRE_FILTER, rta_tap::POST_FILTER][self.filter_tap.load(Relaxed).clamp(0, 1) as usize],
            _ => [rta_tap::IN, rta_tap::POST_EQ, rta_tap::POST_FADER][self.home.tap.load(Relaxed).clamp(0, 2) as usize],
        }
    }

    /// Back to the console's defaults (settings and listening; the analysed
    /// strip and what the UI shows stay).
    pub fn reset(&self) {
        let d = AnalysisControls::default();
        self.rta_freeze.store(false, Relaxed);
        self.listen_mode.store(listen_mode::OFF, Relaxed);
        self.listen_kind.store(-1, Relaxed);
        self.listen_id.store(-1, Relaxed);
        self.listen_band.store(0, Relaxed);
        self.band_solo_trim_db.store(d.band_solo_trim_db.load());
        self.home.reset_to(&d.home);
        self.eq.reset_to(&d.eq);
        self.filter_tap.store(d.filter_tap.load(Relaxed), Relaxed);
    }
}

// ---- lock-free sample ring --------------------------------------------------------------

/// A ring of interleaved frames with one producer and any number of readers.
/// The producer never waits: it overwrites the oldest frames, and a reader
/// detects frames overwritten while it was reading (seqlock style) and drops
/// them. Samples are stored as atomics, so a torn read is impossible.
pub struct SampleRing {
    data: Box<[AtomicU32]>,
    channels: usize,
    mask: u64,
    /// Frames written (published with Release after the data).
    written: AtomicU64,
    /// Frames claimed (announced before the data is overwritten).
    claimed: AtomicU64,
}

impl SampleRing {
    /// `frames` is rounded up to a power of two.
    pub fn new(frames: usize, channels: usize) -> Self {
        let frames = frames.next_power_of_two();
        let channels = channels.max(1);
        Self {
            data: (0..frames * channels).map(|_| AtomicU32::new(0)).collect(),
            channels,
            mask: frames as u64 - 1,
            written: AtomicU64::new(0),
            claimed: AtomicU64::new(0),
        }
    }

    pub fn capacity(&self) -> u64 {
        self.mask + 1
    }

    pub fn written(&self) -> u64 {
        self.written.load(Acquire)
    }

    /// Producer only (one at a time): appends `frames` frames, sample `ch` of
    /// frame `i` being `sample(i, ch)`.
    pub fn push(&self, frames: usize, mut sample: impl FnMut(usize, usize) -> f32) {
        let w = self.written.load(Relaxed);
        self.claimed.store(w + frames as u64, Relaxed);
        fence(Release);
        for i in 0..frames {
            let base = ((w + i as u64) & self.mask) as usize * self.channels;
            for ch in 0..self.channels {
                self.data[base + ch].store(sample(i, ch).to_bits(), Relaxed);
            }
        }
        self.written.store(w + frames as u64, Release);
    }

    /// Reads frames from position `from` on into `out` (interleaved, as many
    /// as fit). Returns the position of the first frame read and the number
    /// read; frames already overwritten are skipped.
    pub fn read(&self, from: u64, out: &mut [f32]) -> (u64, usize) {
        let ch = self.channels;
        let w = self.written.load(Acquire);
        let start = from.max(w.saturating_sub(self.capacity()));
        let frames = (w.saturating_sub(start) as usize).min(out.len() / ch);
        for i in 0..frames {
            let base = ((start + i as u64) & self.mask) as usize * ch;
            for c in 0..ch {
                out[i * ch + c] = f32::from_bits(self.data[base + c].load(Relaxed));
            }
        }
        fence(Acquire);
        // Anything the producer claimed since may have replaced the oldest frames.
        let valid = self.claimed.load(Relaxed).saturating_sub(self.capacity());
        let lost = (valid.saturating_sub(start) as usize).min(frames);
        if lost > 0 {
            out.copy_within(lost * ch..frames * ch, 0);
        }
        (start + lost as u64, frames - lost)
    }

    /// The newest frames that fit into `out`; returns how many.
    pub fn latest(&self, out: &mut [f32]) -> usize {
        let want = (out.len() / self.channels) as u64;
        self.read(self.written().saturating_sub(want), out).1
    }
}

// ---- RTA ------------------------------------------------------------------------------------

/// One FFT resolution: the signal decimated by `decimation`, `n` points, a
/// periodic Hann window.
struct Tier {
    decimation: usize,
    fft: Fft,
    window: Vec<f32>,
    history: Vec<f32>,
    pos: usize,
    phase: usize,
    lowpass: [Biquad; 4],
    /// Bin spacing and sample rate (Hz).
    df: f32,
    fs: f32,
    /// |X|^2 -> power of the tone in a band of bins (sums to A^2 for a sine of amplitude A).
    norm_sum: f32,
    /// |X|^2 -> A^2 of a tone on that bin.
    norm_peak: f32,
    re: Vec<f32>,
    im: Vec<f32>,
    power: Vec<f32>,
}

impl Tier {
    fn new(sr: f32, decimation: usize, n: usize) -> Self {
        let window: Vec<f32> =
            (0..n).map(|i| (0.5 - 0.5 * (2.0 * std::f64::consts::PI * i as f64 / n as f64).cos()) as f32).collect();
        let sum: f32 = window.iter().sum();
        let sum2: f32 = window.iter().map(|w| w * w).sum();
        let fs = sr / decimation as f32;
        let mut lowpass: [Biquad; 4] = Default::default();
        if decimation > 1 {
            // 8th-order Butterworth well below the new Nyquist.
            for (f, q) in lowpass.iter_mut().zip([0.5098, 0.6013, 0.9000, 2.5629]) {
                f.set_coefficients(BiquadCoeffs::low_pass(sr as f64, fs as f64 * 0.2, q));
            }
        }
        Self {
            decimation,
            fft: Fft::new(n),
            window,
            history: vec![0.0; n],
            pos: 0,
            phase: 0,
            lowpass,
            df: fs / n as f32,
            fs,
            norm_sum: 4.0 / (n as f32 * sum2),
            norm_peak: 4.0 / (sum * sum),
            re: vec![0.0; n],
            im: vec![0.0; n],
            power: vec![0.0; n / 2 + 1],
        }
    }

    #[inline]
    fn push(&mut self, x: f32) {
        let x = if self.decimation > 1 {
            let mut y = x;
            for f in &mut self.lowpass {
                y = f.tick(y, 0);
            }
            self.phase += 1;
            if self.phase < self.decimation {
                return;
            }
            self.phase = 0;
            y
        } else {
            x
        };
        self.history[self.pos] = x;
        self.pos = (self.pos + 1) % self.history.len();
    }

    fn analyse(&mut self) {
        let n = self.history.len();
        for i in 0..n {
            self.re[i] = self.history[(self.pos + i) % n] * self.window[i];
            self.im[i] = 0.0;
        }
        self.fft.forward(&mut self.re, &mut self.im);
        for (k, p) in self.power.iter_mut().enumerate() {
            *p = self.re[k] * self.re[k] + self.im[k] * self.im[k];
        }
    }
}

/// Which bins make up a band.
#[derive(Clone, Copy, Debug, Default)]
struct BandBins {
    tier: usize,
    /// Bins `first..=last` (whose centres lie in the band), or none
    /// (`last < first`): then the spectrum is interpolated at `centre`.
    first: usize,
    last: usize,
    centre: f32,
}

/// The RTA computation (no threads, no allocation after construction):
/// feed samples with [`push`](Self::push); every hop it analyses the latest
/// window and updates the bands.
pub struct RtaCore {
    sr: f32,
    tiers: Vec<Tier>,
    bins: [BandBins; RTA_BANDS],
    hop: usize,
    since_hop: usize,
    detector_state: [f32; RTA_BANDS],
    display: [f32; RTA_BANDS],
    hold: [f32; RTA_BANDS],
    hold_age: [f32; RTA_BANDS],
    /// RTA DECAY (0..2) and DETECTOR (0..2).
    pub decay: usize,
    pub detector: usize,
    current_detector: usize,
    frames: u64,
}

impl RtaCore {
    const N: usize = 4096;

    pub fn new(sample_rate: f32) -> Self {
        let sr = sample_rate.max(8000.0);
        // Full rate for the high bands; about 6 kHz for the low ones, where a
        // semitone is only a few Hz wide.
        let decimation = ((sr / 6000.0).round() as usize).max(2);
        let tiers = vec![Tier::new(sr, 1, Self::N), Tier::new(sr, decimation, Self::N)];
        let mut bins = [BandBins::default(); RTA_BANDS];
        for (b, bin) in bins.iter_mut().enumerate() {
            let fc = rta_band_hz(b);
            let (lo, hi) = (fc * 2f32.powf(-1.0 / 24.0), fc * 2f32.powf(1.0 / 24.0));
            // The coarsest resolution with at least two bins in the band.
            let tier = if hi - lo >= 2.0 * tiers[0].df || hi >= 0.45 * tiers[1].fs { 0 } else { 1 };
            let t = &tiers[tier];
            let top = t.power.len() - 2;
            *bin = BandBins {
                tier,
                first: ((lo / t.df).ceil() as usize).min(top),
                last: ((hi / t.df).ceil() as usize).saturating_sub(1).min(top),
                centre: (fc / t.df).min(top as f32),
            };
        }
        Self {
            sr,
            tiers,
            bins,
            hop: Self::N / 4,
            since_hop: 0,
            detector_state: [0.0; RTA_BANDS],
            display: [RTA_FLOOR_DB; RTA_BANDS],
            hold: [RTA_FLOOR_DB; RTA_BANDS],
            hold_age: [0.0; RTA_BANDS],
            decay: 2,
            detector: 0,
            current_detector: 0,
            frames: 0,
        }
    }

    pub fn sample_rate(&self) -> f32 {
        self.sr
    }

    /// Samples per analysis frame.
    pub fn hop(&self) -> usize {
        self.hop
    }

    /// Frames analysed so far.
    pub fn frames(&self) -> u64 {
        self.frames
    }

    pub fn push(&mut self, samples: &[f32]) {
        for &x in samples {
            let x = if x.is_finite() { x } else { 0.0 };
            for t in &mut self.tiers {
                t.push(x);
            }
            self.since_hop += 1;
            if self.since_hop >= self.hop {
                self.since_hop = 0;
                self.frame();
            }
        }
    }

    /// Band levels in dB (a sine of amplitude 1 centred in a band reads 0 dB).
    pub fn levels(&self) -> &[f32; RTA_BANDS] {
        &self.display
    }

    /// Peak-hold levels in dB.
    pub fn peaks(&self) -> &[f32; RTA_BANDS] {
        &self.hold
    }

    /// The power of band `b` in the last analysis (amplitude^2).
    ///
    /// The larger of the band's summed power (right for noise, and for a tone
    /// in a band many bins wide) and its strongest bin read as a tone (right
    /// for a tone in a band only a bin or two wide).
    fn band_power(&self, b: usize) -> f32 {
        let m = self.bins[b];
        let t = &self.tiers[m.tier];
        if m.last >= m.first {
            let bins = &t.power[m.first..=m.last];
            let sum = bins.iter().sum::<f32>() * t.norm_sum;
            let peak = bins.iter().fold(0.0f32, |a, &p| a.max(p)) * t.norm_peak;
            sum.max(peak)
        } else {
            let k = m.centre.floor() as usize;
            let frac = m.centre - k as f32;
            ((1.0 - frac) * t.power[k] + frac * t.power[k + 1]) * t.norm_peak
        }
    }

    fn frame(&mut self) {
        for t in &mut self.tiers {
            t.analyse();
        }
        self.frames += 1;
        let dt = self.hop as f32 / self.sr;
        if self.detector != self.current_detector {
            self.current_detector = self.detector;
            self.detector_state = [0.0; RTA_BANDS];
        }
        let fall = RTA_DECAY_DB_PER_S[self.decay.min(2)] * dt;
        let rms = (-dt / RMS_SECONDS).exp();
        let avg = (-dt / AVG_SECONDS).exp();
        for b in 0..RTA_BANDS {
            let p = self.band_power(b).max(0.0);
            let s = &mut self.detector_state[b];
            let power = match self.current_detector {
                1 => {
                    *s = rms * *s + (1.0 - rms) * p;
                    *s
                }
                2 => {
                    *s = avg * *s + (1.0 - avg) * p.sqrt();
                    *s * *s
                }
                _ => p,
            };
            let level = (10.0 * (power + 1e-15).log10()).max(RTA_FLOOR_DB);
            let shown = level.max(self.display[b] - fall);
            self.display[b] = shown;
            if shown >= self.hold[b] {
                self.hold[b] = shown;
                self.hold_age[b] = 0.0;
            } else {
                self.hold_age[b] += dt;
                if self.hold_age[b] > HOLD_SECONDS {
                    self.hold[b] = (self.hold[b] - fall).max(shown);
                }
            }
        }
    }
}

// ---- the analyser (owned by the engine) -------------------------------------------------

/// What the audio side, the analysis thread and the UI share.
pub struct AnalysisShared {
    pub controls: AnalysisControls,
    rta_ring: SampleRing,
    scope_ring: SampleRing,
    levels: [AtomicF32; RTA_BANDS],
    peaks: [AtomicF32; RTA_BANDS],
    /// RTA frames published (the UI can tell new data).
    published: AtomicU32,
    sample_rate: AtomicF32,
    scope_decimation: AtomicU32,
    /// Scope decimation phase (producer only).
    scope_phase: AtomicU32,
    stop: AtomicBool,
    /// When the UI last read the bands (ms since `epoch`): the thread idles
    /// once nobody reads them, even if the UI left `rta_on` set.
    epoch: Instant,
    last_read_ms: AtomicU64,
}

impl AnalysisShared {
    fn new() -> Self {
        Self {
            controls: AnalysisControls::default(),
            rta_ring: SampleRing::new(1 << 16, 1),
            scope_ring: SampleRing::new(1 << 13, 2),
            levels: std::array::from_fn(|_| AtomicF32::new(RTA_FLOOR_DB)),
            peaks: std::array::from_fn(|_| AtomicF32::new(RTA_FLOOR_DB)),
            published: AtomicU32::new(0),
            sample_rate: AtomicF32::new(48000.0),
            scope_decimation: AtomicU32::new(2),
            scope_phase: AtomicU32::new(0),
            stop: AtomicBool::new(false),
            epoch: Instant::now(),
            last_read_ms: AtomicU64::new(0),
        }
    }

    fn touch(&self) {
        self.last_read_ms.store(self.epoch.elapsed().as_millis() as u64, Relaxed);
    }

    /// Nobody read the bands for a while.
    fn unread(&self) -> bool {
        self.epoch.elapsed().as_millis() as u64 > self.last_read_ms.load(Relaxed) + UNREAD_MS
    }

    fn publish(&self, core: &RtaCore) {
        for (dst, &v) in self.levels.iter().zip(core.levels()) {
            dst.store(v);
        }
        for (dst, &v) in self.peaks.iter().zip(core.peaks()) {
            dst.store(v);
        }
        self.published.fetch_add(1, Relaxed);
    }

    fn publish_floor(&self) {
        for v in self.levels.iter().chain(&self.peaks) {
            v.store(RTA_FLOOR_DB);
        }
        self.published.fetch_add(1, Relaxed);
    }
}

/// A LISTEN source: a strip, a mode and a band. Null strip = none.
#[derive(Clone, Copy, Debug, PartialEq)]
struct ListenTarget {
    strip: *const Strip,
    mode: i32,
    band: i32,
}

impl ListenTarget {
    const NONE: ListenTarget = ListenTarget { strip: std::ptr::null(), mode: 0, band: 0 };

    fn is_none(&self) -> bool {
        self.strip.is_null()
    }
}

/// LISTEN state of the audio thread.
struct ListenAudio {
    /// The source playing (or fading out).
    effective: ListenTarget,
    gain: f32,
    target_gain: f32,
    /// LISTEN replaces the solo bus this block.
    overriding: bool,
}

/// The strips the audio side analyses this block, resolved once per chunk.
#[derive(Clone, Copy)]
pub(crate) struct Probes {
    rta: *const Strip,
    rta_tap: usize,
    scope: *const Strip,
    listen: *const Strip,
    listen_mode: i32,
    listen_band: i32,
    shared: *const AnalysisShared,
}

// SAFETY: the pointers are only compared, except `shared`, which outlives
// every block (it is owned by the engine's analyser).
unsafe impl Send for Probes {}
unsafe impl Sync for Probes {}

/// What one strip provides this block.
#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct StripProbe {
    /// Push the RTA signal at this tap (`rta_tap`).
    pub rta: Option<usize>,
    /// Push the scope signal (after the INPUT block).
    pub scope: bool,
    /// Compute the LISTEN signal: (`listen_mode`, band).
    pub listen: Option<(i32, i32)>,
}

impl Probes {
    pub(crate) const NONE: Probes = Probes {
        rta: std::ptr::null(),
        rta_tap: 0,
        scope: std::ptr::null(),
        listen: std::ptr::null(),
        listen_mode: 0,
        listen_band: 0,
        shared: std::ptr::null(),
    };

    #[inline]
    pub(crate) fn of(&self, s: &Strip) -> StripProbe {
        let me: *const Strip = s;
        StripProbe {
            rta: (self.rta == me).then_some(self.rta_tap),
            scope: self.scope == me,
            listen: (self.listen == me).then_some((self.listen_mode, self.listen_band)),
        }
    }

    /// Pushes a block of a strip (1 or 2 channels) into the RTA ring, as mono.
    pub(crate) fn push_rta(&self, chans: [&[f32]; 2], nc: usize, n: usize) {
        // SAFETY: see the Sync impl.
        let Some(shared) = (unsafe { self.shared.as_ref() }) else { return };
        if nc > 1 {
            shared.rta_ring.push(n, |i, _| (chans[0][i] + chans[1][i]) * 0.5);
        } else {
            shared.rta_ring.push(n, |i, _| chans[0][i]);
        }
    }

    /// Pushes every n-th L/R pair of a block into the scope ring.
    pub(crate) fn push_scope(&self, chans: [&[f32]; 2], nc: usize, n: usize) {
        // SAFETY: see the Sync impl.
        let Some(shared) = (unsafe { self.shared.as_ref() }) else { return };
        let step = shared.scope_decimation.load(Relaxed).max(1) as usize;
        let phase = shared.scope_phase.load(Relaxed) as usize % step;
        let first = (step - phase) % step;
        let count = if n > first { (n - first).div_ceil(step) } else { 0 };
        let right = if nc > 1 { 1 } else { 0 };
        shared.scope_ring.push(count, |i, ch| chans[if ch == 0 { 0 } else { right }][first + i * step]);
        shared.scope_phase.store(((phase + n) % step) as u32, Relaxed);
    }
}

/// The engine's analysis and LISTEN state.
pub struct Analyser {
    shared: Arc<AnalysisShared>,
    thread: Mutex<Option<JoinHandle<()>>>,
    audio: UnsafeCell<ListenAudio>,
}

// SAFETY: `audio` is only touched by the audio thread (begin_block, the
// LISTEN calls); everything else is atomic or behind the mutex.
unsafe impl Sync for Analyser {}
unsafe impl Send for Analyser {}

impl Default for Analyser {
    fn default() -> Self {
        Self::new()
    }
}

impl Analyser {
    pub fn new() -> Self {
        Self {
            shared: Arc::new(AnalysisShared::new()),
            thread: Mutex::new(None),
            audio: UnsafeCell::new(ListenAudio { effective: ListenTarget::NONE, gain: 0.0, target_gain: 0.0, overriding: false }),
        }
    }

    pub fn controls(&self) -> &AnalysisControls {
        &self.shared.controls
    }

    /// Message thread: the console's defaults (a reset engine).
    pub fn reset(&self) {
        self.shared.controls.reset();
    }

    /// Device stopped: the rates the rings are filled at.
    pub fn prepare(&self, sample_rate: f64) {
        self.shared.sample_rate.store(sample_rate as f32);
        self.shared.scope_decimation.store(((sample_rate / SCOPE_RATE).round() as u32).max(1), Relaxed);
    }

    /// Message thread: starts the analysis thread if it isn't running.
    pub fn ensure_thread(&self) {
        self.shared.touch();
        let mut t = self.thread.lock().unwrap_or_else(|e| e.into_inner());
        if t.is_some() {
            return;
        }
        let shared = self.shared.clone();
        *t = std::thread::Builder::new().name("LiveDAW RTA".into()).spawn(move || run_analysis(&shared)).ok();
    }

    /// Stops the analysis thread (it restarts on the next `ensure_thread`).
    pub fn shutdown(&self) {
        let handle = self.thread.lock().unwrap_or_else(|e| e.into_inner()).take();
        if let Some(h) = handle {
            self.shared.stop.store(true, SeqCst);
            h.thread().unpark();
            let _ = h.join();
            self.shared.stop.store(false, SeqCst);
        }
    }

    /// The latest RTA bands (dB) and peak holds; returns the number of bands
    /// written and the publication counter.
    pub fn read_rta(&self, levels: &mut [f32], peaks: &mut [f32]) -> (usize, u32) {
        self.shared.touch();
        let n = levels.len().min(RTA_BANDS);
        for (dst, src) in levels.iter_mut().zip(&self.shared.levels) {
            *dst = src.load();
        }
        for (dst, src) in peaks.iter_mut().zip(&self.shared.peaks) {
            *dst = src.load();
        }
        (n, self.shared.published.load(Relaxed))
    }

    /// The newest L/R pairs of the scope (interleaved); returns how many.
    pub fn read_scope(&self, out: &mut [f32]) -> usize {
        self.shared.scope_ring.latest(out)
    }

    /// Scope pairs per second.
    pub fn scope_rate(&self) -> f32 {
        self.shared.sample_rate.load() / self.shared.scope_decimation.load(Relaxed).max(1) as f32
    }

    /// Audio thread, once per chunk before the strips run: resolves the
    /// analysed and the listened strip in the current topology and advances
    /// LISTEN's fades.
    ///
    /// # Safety
    /// Audio thread only.
    pub(crate) unsafe fn begin_block<'a>(&self, strips: impl Iterator<Item = &'a Arc<Strip>>) -> Probes {
        let c = &self.shared.controls;
        let a = &mut *self.audio.get();
        let mut p = Probes { shared: Arc::as_ptr(&self.shared), ..Probes::NONE };
        let rta_on = c.rta_on.load(Relaxed);
        let scope_on = c.scope_on.load(Relaxed);
        let mode = c.listen_mode.load(Relaxed);
        if !rta_on && !scope_on && mode == listen_mode::OFF && a.effective.is_none() {
            a.overriding = false;
            a.gain = 0.0;
            a.target_gain = 0.0;
            return p;
        }

        let (kind, id) = (c.strip_kind.load(Relaxed), c.strip_id.load(Relaxed));
        let (listen_kind, listen_id) = (c.listen_kind.load(Relaxed), c.listen_id.load(Relaxed));
        let (mut target, mut listen, mut effective_alive) = (std::ptr::null(), std::ptr::null(), false);
        for s in strips {
            let ptr = Arc::as_ptr(s);
            let k = s.kind() as i32;
            if k == kind && s.id() == id {
                target = ptr;
            }
            if mode != listen_mode::OFF && k == listen_kind && s.id() == listen_id {
                listen = ptr;
            }
            effective_alive |= ptr == a.effective.strip;
        }
        if rta_on {
            p.rta = target;
            p.rta_tap = c.engine_tap();
        }
        if scope_on {
            p.scope = target;
        }

        // A removed strip may be freed soon: forget it at once.
        if !effective_alive {
            a.effective = ListenTarget::NONE;
            a.gain = 0.0;
        }
        let desired = if listen.is_null() {
            ListenTarget::NONE
        } else {
            ListenTarget { strip: listen, mode, band: c.listen_band.load(Relaxed) }
        };
        if desired != a.effective && a.gain == 0.0 {
            a.effective = desired; // switch while silent
        }
        a.target_gain = if a.effective == desired && !desired.is_none() {
            if mode == listen_mode::BAND {
                db_to_gain(c.band_solo_trim_db.load().clamp(0.0, 24.0))
            } else {
                1.0
            }
        } else {
            0.0 // fade the old source out first
        };
        a.overriding = !desired.is_none();
        p.listen = a.effective.strip;
        p.listen_mode = a.effective.mode;
        p.listen_band = a.effective.band;
        p
    }

    /// Audio thread, monitor stage: LISTEN replaces the solo bus this block.
    pub(crate) fn listen_overrides_solo(&self) -> bool {
        // SAFETY: audio thread only (see begin_block).
        unsafe { (*self.audio.get()).overriding }
    }

    /// Audio thread, monitor stage (after every strip ran): adds the LISTEN
    /// signal to the monitor pair.
    ///
    /// # Safety
    /// Audio thread only, after `begin_block` in the same chunk.
    pub(crate) unsafe fn add_listen(&self, l: &mut [f32], r: &mut [f32], n: usize) {
        let a = &mut *self.audio.get();
        if !a.effective.is_none() && (a.gain != 0.0 || a.target_gain != 0.0) {
            // The strip is in this block's topology (checked in begin_block).
            let d = (*a.effective.strip).dsp();
            add_with_gain_ramp(&mut l[..n], d.listen(0, n), a.gain, a.target_gain);
            add_with_gain_ramp(&mut r[..n], d.listen(1, n), a.gain, a.target_gain);
        }
        a.gain = a.target_gain;
    }
}

impl Drop for Analyser {
    fn drop(&mut self) {
        self.shutdown();
    }
}

/// The analysis thread: computes the RTA while one is shown.
fn run_analysis(shared: &AnalysisShared) {
    let mut core: Option<RtaCore> = None;
    let mut key = (0, 0, 0, 0);
    let mut read_pos = 0u64;
    let mut buf = vec![0.0f32; 4096];
    let mut last_data = Instant::now();
    let mut silence_sent = Instant::now();
    let c = &shared.controls;
    while !shared.stop.load(SeqCst) {
        if !c.rta_on.load(Relaxed) || shared.unread() {
            if core.take().is_some() {
                shared.publish_floor();
            }
            std::thread::park_timeout(Duration::from_millis(50));
            continue;
        }
        let sr = shared.sample_rate.load();
        let now_key = (c.strip_kind.load(Relaxed), c.strip_id.load(Relaxed), c.engine_tap() as i32, sr.to_bits() as i32);
        if core.is_none() || now_key != key {
            // Another strip, tap or rate: start over from the newest samples.
            core = Some(RtaCore::new(sr));
            key = now_key;
            read_pos = shared.rta_ring.written();
            shared.publish_floor();
            last_data = Instant::now();
        }
        let Some(rta) = core.as_mut() else { continue };
        let settings = c.view_settings();
        rta.decay = settings.decay.load(Relaxed).clamp(0, 2) as usize;
        rta.detector = settings.detector.load(Relaxed).clamp(0, 2) as usize;

        let mut got = false;
        loop {
            let (start, frames) = shared.rta_ring.read(read_pos, &mut buf);
            if frames == 0 {
                break;
            }
            read_pos = start + frames as u64;
            rta.push(&buf[..frames]);
            got = true;
        }
        let now = Instant::now();
        if got {
            last_data = now;
            silence_sent = now;
        } else if now.duration_since(last_data) > Duration::from_millis(250) {
            // No audio (device stopped, strip gone): let the bars fall.
            let samples = ((now.duration_since(silence_sent).as_secs_f32() * rta.sample_rate()) as usize).min(buf.len());
            buf[..samples].fill(0.0);
            rta.push(&buf[..samples]);
            silence_sent = now;
        }
        if !c.rta_freeze.load(Relaxed) {
            shared.publish(rta);
        }
        std::thread::park_timeout(Duration::from_millis(12));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sine(sr: f32, f: f32, amp: f32, n: usize) -> Vec<f32> {
        (0..n).map(|i| amp * (2.0 * std::f32::consts::PI * f * i as f32 / sr).sin()).collect()
    }

    fn band_of(f: f32) -> usize {
        (0..RTA_BANDS).min_by(|&a, &b| (rta_band_hz(a) / f).ln().abs().total_cmp(&(rta_band_hz(b) / f).ln().abs())).unwrap()
    }

    #[test]
    fn bands_are_the_piano_keys() {
        assert!((rta_band_hz(0) - 20.60).abs() < 0.01);
        assert!((rta_band_hz(band_of(440.0)) - 440.0).abs() < 1e-3);
        assert!((rta_band_hz(RTA_BANDS - 1) - 19912.1).abs() < 1.0);
    }

    #[test]
    fn sines_read_their_level_in_their_band() {
        let sr = 48000.0;
        for (f, amp) in [(1000.0, 0.5), (110.0, 0.25), (41.2, 1.0), (8372.0, 0.1), (261.6, 0.5)] {
            let f = rta_band_hz(band_of(f)); // centred in its band
            let mut rta = RtaCore::new(sr);
            rta.detector = 0;
            rta.push(&sine(sr, f, amp, 48000 * 2));
            let b = band_of(f);
            let want = 20.0 * amp.log10();
            let got = rta.levels()[b];
            assert!((got - want).abs() < 1.5, "{f} Hz: {got} dB, want {want}");
            // Far from the tone, nothing.
            for other in [b.saturating_sub(24), (b + 24).min(RTA_BANDS - 1)] {
                if other.abs_diff(b) >= 24 {
                    assert!(rta.levels()[other] < want - 50.0, "{f} Hz leaks into band {other}: {}", rta.levels()[other]);
                }
            }
            // The neighbours a few semitones away are well down too.
            assert!(rta.levels()[b + 3] < want - 20.0, "{f} Hz: band +3 {}", rta.levels()[b + 3]);
        }
    }

    #[test]
    fn other_sample_rates() {
        for sr in [44100.0f32, 96000.0] {
            let f = rta_band_hz(band_of(500.0));
            let mut rta = RtaCore::new(sr);
            rta.push(&sine(sr, f, 0.5, sr as usize * 2));
            assert!((rta.levels()[band_of(f)] + 6.02).abs() < 1.5, "{sr}: {}", rta.levels()[band_of(f)]);
        }
    }

    #[test]
    fn decay_and_peak_hold() {
        let sr = 48000.0;
        let f = rta_band_hz(band_of(2000.0));
        let b = band_of(f);
        let mut rta = RtaCore::new(sr);
        rta.decay = 2; // FAST, 60 dB/s
        rta.push(&sine(sr, f, 1.0, 48000));
        let top = rta.levels()[b];
        assert!(top > -2.0);
        // 0.5 s of silence: the bar falls at the decay rate, the hold stays.
        rta.push(&vec![0.0; 24000]);
        let fallen = rta.levels()[b];
        assert!((top - fallen - 30.0).abs() < 6.0, "fell {} dB", top - fallen);
        assert!((rta.peaks()[b] - top).abs() < 0.5);
        // After the hold time the hold falls too, never below the bar.
        rta.push(&vec![0.0; 72000]);
        assert!(rta.peaks()[b] < top - 20.0 && rta.peaks()[b] >= rta.levels()[b]);

        // SLOW falls slower.
        let mut slow = RtaCore::new(sr);
        slow.decay = 0;
        slow.push(&sine(sr, f, 1.0, 48000));
        slow.push(&vec![0.0; 24000]);
        assert!(slow.levels()[b] > fallen + 15.0);
    }

    #[test]
    fn rms_and_avg_detectors_are_slower() {
        let sr = 48000.0;
        let f = rta_band_hz(band_of(1000.0));
        let b = band_of(f);
        let level_after = |detector: usize, seconds: f32| {
            let mut rta = RtaCore::new(sr);
            rta.detector = detector;
            rta.decay = 2;
            rta.push(&sine(sr, f, 1.0, (sr * seconds) as usize));
            rta.levels()[b]
        };
        let (peak, rms, avg) = (level_after(0, 0.15), level_after(1, 0.15), level_after(2, 0.15));
        assert!(peak > rms && rms > avg, "{peak} {rms} {avg}");
        // Settled, all read the tone.
        for d in 0..3 {
            assert!(level_after(d, 6.0).abs() < 1.5, "detector {d}");
        }
    }

    #[test]
    fn ring_keeps_the_newest_frames() {
        let ring = SampleRing::new(8, 2);
        ring.push(5, |i, ch| (i * 10 + ch) as f32);
        let mut out = [0.0; 16];
        assert_eq!(ring.read(0, &mut out), (0, 5));
        assert_eq!(&out[..4], &[0.0, 1.0, 10.0, 11.0]);
        // Overrun: the oldest frames are gone.
        ring.push(6, |i, ch| (100 + i * 10 + ch) as f32);
        let (start, n) = ring.read(0, &mut out);
        assert_eq!((start, n), (3, 8));
        assert_eq!(&out[..2], &[30.0, 31.0]);
        assert_eq!(&out[14..16], &[150.0, 151.0]);
        let mut last = [0.0; 4];
        assert_eq!(ring.latest(&mut last), 2);
        assert_eq!(last, [140.0, 141.0, 150.0, 151.0]);
        // Reading on from a position.
        assert_eq!(ring.read(11, &mut out).1, 0);
    }

    #[test]
    fn the_thread_publishes_bands() {
        let a = Analyser::new();
        a.prepare(48000.0);
        let c = a.controls();
        c.rta_on.store(true, Relaxed);
        a.ensure_thread();
        let p = Probes { shared: Arc::as_ptr(&a.shared), ..Probes::NONE };
        let f = rta_band_hz(band_of(3000.0));
        let block = sine(48000.0, f, 0.5, 48000);
        // Wait for the thread to pick up the target before feeding it.
        std::thread::sleep(Duration::from_millis(100));
        let mut levels = [0.0; RTA_BANDS];
        let mut peaks = [0.0; RTA_BANDS];
        let deadline = Instant::now() + Duration::from_secs(10);
        loop {
            for chunk in block.chunks(512) {
                p.push_rta([chunk, chunk], 1, chunk.len());
            }
            std::thread::sleep(Duration::from_millis(30));
            a.read_rta(&mut levels, &mut peaks);
            if levels[band_of(f)] > -9.0 || Instant::now() > deadline {
                break;
            }
        }
        assert!((levels[band_of(f)] + 6.0).abs() < 2.0, "{}", levels[band_of(f)]);
        // FREEZE holds the bands.
        c.rta_freeze.store(true, Relaxed);
        std::thread::sleep(Duration::from_millis(60));
        let (_, frozen) = a.read_rta(&mut levels, &mut peaks);
        std::thread::sleep(Duration::from_millis(60));
        assert_eq!(a.read_rta(&mut levels, &mut peaks).1, frozen);
        a.shutdown();
    }
}
