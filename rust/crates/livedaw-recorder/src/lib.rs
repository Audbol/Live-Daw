//! The multitrack recorder and player of Live DAW.
//!
//! Three threads share a [`Recorder`]:
//!
//! * **Audio thread** ([`audio`]): owns the transport (Stopped / Starting /
//!   Playing / Recording), the position and the 64 mono playback buffers. Per
//!   chunk it drains the commands of the message thread, reads the playback
//!   ring into the playback buffers and copies the record sources into the
//!   record ring. It never blocks, allocates or frees.
//! * **Reader thread** ([`reader`]): renders the playlist ahead of the play
//!   position into one of two playback rings. A locate (or play) puts the
//!   transport in Starting: the reader primes a ring from the position, then
//!   rolling begins. A playlist or loop change while rolling is rendered into
//!   the other ring from a point a little ahead; the audio thread crossfades
//!   to it there, so playback never stops.
//! * **Writer thread** ([`writer`]): drains the record ring into 32-bit float
//!   WAV files, computes the peaks for drawing and finalizes the takes.
//!
//! The message thread drives everything through the methods of [`Recorder`].
//!
//! [`usb`] is the separate 2/4-track USB recorder / player (the WING's USB
//! stick), built on the same rings, WAV files and disk-thread design.

pub mod audio;
pub mod playlist;
mod reader;
pub mod ring;
pub mod usb;
pub mod wav;
mod writer;

pub use playlist::{Clip, FadeShape, LoopRange, PlaybackTrack, Playlist, PreparedPlaylist};
pub use usb::{usb_player_channel, UsbDeck, USB_CHANNELS, USB_PLAYER_SOURCE_BASE};

use playlist::PreparedPlaylist as Prepared;
use ring::{Ring, SpscQueue};
use std::cell::UnsafeCell;
use std::collections::VecDeque;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicI64, AtomicPtr, AtomicU32, AtomicU64, AtomicUsize, Ordering::*};
use std::sync::{Arc, Mutex, MutexGuard};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

/// Mono playback channels (input sources `PLAYBACK_SOURCE_BASE + n`).
pub const NUM_PLAYBACK_CHANNELS: usize = 64;
pub const MAX_RECORD_TRACKS: usize = 64;
/// Input index of playback channel 0 (strip input and ALT input fields).
pub const PLAYBACK_SOURCE_BASE: i32 = 0x10000;
/// Resolution of the recording peaks.
pub const SAMPLES_PER_PEAK: usize = 256;

/// Seconds of audio each playback ring holds ahead of the play position.
pub const PLAYBACK_BUFFER_SECONDS: f64 = 2.0;
/// Seconds of audio the record ring holds for the writer.
pub const RECORD_BUFFER_SECONDS: f64 = 4.0;
/// Rolling starts once this much is buffered.
pub const PRIME_SECONDS: f64 = 0.25;
/// Fade-in/out of playback on start, stop and locate (samples).
pub const DECLICK_SAMPLES: usize = 256;
/// Crossfade to new content after a playlist or loop change (samples).
pub const CROSSFADE_SAMPLES: usize = 512;
/// The writer rewrites the WAV headers this often while recording.
pub const HEADER_INTERVAL: Duration = Duration::from_secs(2);

const DEFAULT_SAMPLE_RATE: f64 = 48000.0;
const DEFAULT_BLOCK: usize = 512;

/// The playback channel an input index refers to, if any.
pub fn playback_channel(input: i32) -> Option<usize> {
    let n = input.checked_sub(PLAYBACK_SOURCE_BASE)?;
    usize::try_from(n).ok().filter(|&n| n < NUM_PLAYBACK_CHANNELS)
}

#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub enum TransportState {
    #[default]
    Stopped = 0,
    /// Filling the playback buffers before rolling.
    Starting = 1,
    Playing = 2,
    Recording = 3,
}

impl TransportState {
    fn from_i32(v: i32) -> Self {
        match v {
            1 => Self::Starting,
            2 => Self::Playing,
            3 => Self::Recording,
            _ => Self::Stopped,
        }
    }

    pub fn is_rolling(self) -> bool {
        matches!(self, Self::Playing | Self::Recording)
    }
}

/// Where a record track takes its signal.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub enum SourceKind {
    /// A hardware input port (or a playback channel), before any processing.
    #[default]
    HardwareInput,
    /// A strip at its pre-fader (PFL) tap.
    StripPreFader,
    /// A strip's output after fader and pan.
    StripPostFader,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub struct RecordSource {
    pub kind: SourceKind,
    /// HardwareInput: first port (0-based; `PLAYBACK_SOURCE_BASE + n` = playback channel n).
    pub port: i32,
    /// Strip sources: the strip kind (the engine's `StripKind` as i32) and its
    /// number as the console shows it ("CH 3" = 3, "BUS 12" = 12).
    pub strip_kind: i32,
    pub strip_number: i32,
}

/// One mono stream of a recording: channel `channel` of `source`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub struct RecordStream {
    pub source: RecordSource,
    pub channel: usize,
}

#[derive(Clone, Debug, PartialEq)]
pub struct RecordTrackSpec {
    /// The UI's track index (reported back in its take).
    pub track: i32,
    pub source: RecordSource,
    pub num_channels: usize,
    pub file: PathBuf,
}

/// A finished recording of one track.
#[derive(Clone, Debug, PartialEq)]
pub struct Take {
    pub track: i32,
    pub file: PathBuf,
    /// Timeline position of the first sample.
    pub start: i64,
    pub length: i64,
    pub num_channels: usize,
}

// ---- shared state ------------------------------------------------------------------

/// An f32 in an atomic.
#[derive(Default)]
pub(crate) struct AtomicF32(AtomicU32);

impl AtomicF32 {
    pub fn load(&self) -> f32 {
        f32::from_bits(self.0.load(Relaxed))
    }
    pub fn store(&self, v: f32) {
        self.0.store(v.to_bits(), Relaxed);
    }
    pub fn exchange(&self, v: f32) -> f32 {
        f32::from_bits(self.0.swap(v.to_bits(), Relaxed))
    }
    pub fn fetch_max(&self, v: f32) {
        let mut current = self.0.load(Relaxed);
        while v > f32::from_bits(current) {
            match self.0.compare_exchange_weak(current, v.to_bits(), Relaxed, Relaxed) {
                Ok(_) => break,
                Err(actual) => current = actual,
            }
        }
    }
}

/// Message thread -> audio thread.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub(crate) enum Command {
    #[default]
    None,
    Play,
    Stop,
    Locate(i64),
    /// Record the session in `Shared::pending_session`.
    Record,
}

/// A playback ring and what it was rendered for. The reader thread changes a
/// slot only while the audio thread can't use it (see `reader`).
pub(crate) struct SlotData {
    pub ring: Ring,
    /// Playback channel of each lane.
    pub lane_channel: Vec<usize>,
    /// The loop the ring's content follows (the audio thread wraps with it).
    pub loop_range: LoopRange,
}

pub(crate) struct Slot(UnsafeCell<SlotData>);

// SAFETY: `SlotData` is only mutated by the reader thread while the audio
// thread can't reach the slot; the ring inside is SPSC-safe.
unsafe impl Sync for Slot {}

impl Slot {
    fn new() -> Self {
        Self(UnsafeCell::new(SlotData { ring: Ring::new(0, 1), lane_channel: Vec::new(), loop_range: LoopRange::default() }))
    }

    /// # Safety
    /// No `&mut` to the slot may exist (see the reader's slot rules).
    pub unsafe fn get(&self) -> &SlotData {
        &*self.0.get()
    }

    /// # Safety
    /// The audio thread must not be able to use the slot.
    #[allow(clippy::mut_from_ref)]
    pub unsafe fn get_mut(&self) -> &mut SlotData {
        &mut *self.0.get()
    }
}

/// Frames the record ring could not take (written as silence by the writer).
#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct Gap {
    /// Ring frames committed before the gap.
    pub at: usize,
    pub len: usize,
}

pub(crate) struct SessionTrack {
    pub track: i32,
    pub first_stream: usize,
    pub num_channels: usize,
    pub file: PathBuf,
}

/// One recording: its sources, the ring the audio thread fills and the
/// state the writer and the message thread follow.
pub(crate) struct RecordSession {
    pub tracks: Vec<SessionTrack>,
    pub streams: Vec<RecordStream>,
    pub ring: Ring,
    pub gaps: SpscQueue<Gap>,
    /// Per stream: linear peak since the last read.
    pub levels: Vec<AtomicF32>,
    pub started: AtomicBool,
    pub start_pos: AtomicI64,
    /// Frames recorded so far (published by the audio thread).
    pub frames: AtomicI64,
    /// The audio thread is done with the session; `end_frames` is final.
    pub ended: AtomicBool,
    pub end_frames: AtomicI64,
    /// Per track: min/max pairs per channel of `SAMPLES_PER_PEAK` samples.
    pub peaks: Vec<Mutex<Vec<f32>>>,
}

impl RecordSession {
    /// Called by whoever releases the session for the audio thread.
    pub fn end(&self, frames: i64) {
        self.end_frames.store(frames, Relaxed);
        self.ended.store(true, Release);
    }
}

/// Message thread -> reader: what plays.
#[derive(Clone)]
pub(crate) struct Content {
    pub playlist: Arc<Prepared>,
    pub loop_range: LoopRange,
}

pub(crate) struct Shared {
    pub sample_rate: AtomicU64,
    pub max_block: AtomicUsize,
    pub rf64_threshold: AtomicU64,

    pub commands: SpscQueue<Command>,
    pub commands_processed: AtomicU64,
    pub pending_session: AtomicPtr<RecordSession>,

    // Published by the audio thread.
    pub state: AtomicI32,
    pub position: AtomicI64,
    pub seq: AtomicI64,
    pub epoch: AtomicU64,
    pub prime_pos: AtomicI64,
    pub prime_record: AtomicBool,
    pub prime_epoch: AtomicU64,
    pub slot_in_use: [AtomicBool; 2],
    pub switch_done: AtomicU64,
    pub switch_missed: AtomicU64,
    pub playback_buffered: AtomicUsize,
    pub disk_load: AtomicF32,

    // Published by the reader.
    pub ready_epoch: AtomicU64,
    pub ready_slot: AtomicUsize,
    /// Seqlock: odd while the reader writes a switch request.
    pub pend_token: AtomicU64,
    pub pend_slot: AtomicUsize,
    pub pend_seq: AtomicI64,
    pub pend_epoch: AtomicU64,
    pub reader_seen: AtomicU64,
    pub reader_epoch: AtomicU64,
    pub reader_busy: AtomicBool,
    /// `wait_for_disk` bumps `kick`; each thread copies the value it saw at the
    /// start of a step when the step ends.
    pub kick: AtomicU64,
    pub reader_kick: AtomicU64,
    pub writer_kick: AtomicU64,
    pub slots: [Slot; 2],

    pub content: Mutex<Content>,
    pub content_version: AtomicU64,

    pub new_sessions: Mutex<Vec<writer::WriterJob>>,
    pub finished: Mutex<VecDeque<Take>>,
    pub writer_busy: AtomicBool,

    pub dropouts: AtomicU32,
    pub shutdown: AtomicBool,
}

impl Shared {
    fn new() -> Self {
        Self {
            sample_rate: AtomicU64::new(DEFAULT_SAMPLE_RATE.to_bits()),
            max_block: AtomicUsize::new(DEFAULT_BLOCK),
            rf64_threshold: AtomicU64::new(wav::RIFF_LIMIT),
            commands: SpscQueue::new(256),
            commands_processed: AtomicU64::new(0),
            pending_session: AtomicPtr::new(std::ptr::null_mut()),
            state: AtomicI32::new(0),
            position: AtomicI64::new(0),
            seq: AtomicI64::new(0),
            epoch: AtomicU64::new(0),
            prime_pos: AtomicI64::new(0),
            prime_record: AtomicBool::new(false),
            prime_epoch: AtomicU64::new(0),
            slot_in_use: [AtomicBool::new(false), AtomicBool::new(false)],
            switch_done: AtomicU64::new(0),
            switch_missed: AtomicU64::new(0),
            playback_buffered: AtomicUsize::new(0),
            disk_load: AtomicF32::default(),
            ready_epoch: AtomicU64::new(0),
            ready_slot: AtomicUsize::new(0),
            pend_token: AtomicU64::new(0),
            pend_slot: AtomicUsize::new(0),
            pend_seq: AtomicI64::new(0),
            pend_epoch: AtomicU64::new(0),
            reader_seen: AtomicU64::new(0),
            reader_epoch: AtomicU64::new(0),
            reader_busy: AtomicBool::new(false),
            kick: AtomicU64::new(0),
            reader_kick: AtomicU64::new(0),
            writer_kick: AtomicU64::new(0),
            slots: [Slot::new(), Slot::new()],
            content: Mutex::new(Content { playlist: Arc::new(Prepared::empty()), loop_range: LoopRange::default() }),
            content_version: AtomicU64::new(0),
            new_sessions: Mutex::new(Vec::new()),
            finished: Mutex::new(VecDeque::new()),
            writer_busy: AtomicBool::new(false),
            dropouts: AtomicU32::new(0),
            shutdown: AtomicBool::new(false),
        }
    }

    pub fn sample_rate(&self) -> f64 {
        f64::from_bits(self.sample_rate.load(Relaxed))
    }

    pub fn state(&self) -> TransportState {
        TransportState::from_i32(self.state.load(Acquire))
    }

    pub fn content(&self) -> MutexGuard<'_, Content> {
        self.content.lock().unwrap_or_else(|e| e.into_inner())
    }
}

fn lock<T>(m: &Mutex<T>) -> MutexGuard<'_, T> {
    m.lock().unwrap_or_else(|e| e.into_inner())
}

// ---- the recorder ----------------------------------------------------------------------

struct Control {
    threads: Vec<JoinHandle<()>>,
    current: Option<Arc<RecordSession>>,
    loop_range: LoopRange,
    pushed: u64,
    last_command: Command,
    /// The last locate and the command count that queued it.
    pending_locate: Option<(u64, i64)>,
}

/// The multitrack recorder / player. The engine owns one; the audio thread
/// calls the methods in [`audio`], everything else is for the message thread.
pub struct Recorder {
    shared: Arc<Shared>,
    control: Mutex<Control>,
    audio: UnsafeCell<audio::AudioSide>,
}

// SAFETY: `audio` is only touched by the audio thread (or by `prepare`/drop
// while it can't run); everything else is atomic or behind mutexes.
unsafe impl Sync for Recorder {}
unsafe impl Send for Recorder {}

impl Default for Recorder {
    fn default() -> Self {
        Self::new()
    }
}

impl Recorder {
    pub fn new() -> Self {
        Self {
            shared: Arc::new(Shared::new()),
            control: Mutex::new(Control {
                threads: Vec::new(),
                current: None,
                loop_range: LoopRange::default(),
                pushed: 0,
                last_command: Command::None,
                pending_locate: None,
            }),
            audio: UnsafeCell::new(audio::AudioSide::new(DEFAULT_SAMPLE_RATE, DEFAULT_BLOCK)),
        }
    }

    fn control(&self) -> MutexGuard<'_, Control> {
        lock(&self.control)
    }

    /// Starts the disk threads on first use.
    fn ensure_threads(&self, c: &mut Control) {
        if !c.threads.is_empty() {
            return;
        }
        let s = self.shared.clone();
        let reader = std::thread::Builder::new().name("LiveDAW disk reader".into()).spawn(move || reader::run(s));
        let s = self.shared.clone();
        let writer = std::thread::Builder::new().name("LiveDAW disk writer".into()).spawn(move || writer::run(s));
        c.threads.extend(reader.ok());
        c.threads.extend(writer.ok());
    }

    fn wake_threads(&self, c: &Control) {
        for t in &c.threads {
            t.thread().unpark();
        }
    }

    fn push(&self, c: &mut Control, cmd: Command) -> bool {
        self.ensure_threads(c);
        let ok = self.shared.commands.push(cmd);
        if ok {
            c.pushed += 1;
            c.last_command = cmd;
            if let Command::Locate(p) = cmd {
                c.pending_locate = Some((c.pushed, p.max(0)));
            }
        }
        self.wake_threads(c);
        ok
    }

    /// Prepares for a device configuration and stops the transport (a
    /// recording ends here).
    ///
    /// # Safety
    /// The audio callback must not be running.
    pub unsafe fn prepare(&self, sample_rate: f64, max_block: usize) {
        let max_block = max_block.max(1);
        self.shared.sample_rate.store(sample_rate.to_bits(), Relaxed);
        self.shared.max_block.store(max_block, Relaxed);
        (*self.audio.get()).prepare(&self.shared, sample_rate, max_block);
    }

    // ---- transport ------------------------------------------------------------

    /// The transport state, including commands the audio thread hasn't
    /// picked up yet (play shows Starting at once).
    pub fn state(&self) -> TransportState {
        let c = self.control();
        let s = self.shared.state();
        if self.shared.commands_processed.load(Acquire) >= c.pushed {
            return s;
        }
        match c.last_command {
            Command::Play | Command::Record if s == TransportState::Stopped => TransportState::Starting,
            Command::Stop => TransportState::Stopped,
            _ => s,
        }
    }

    /// The play position on the timeline (samples).
    pub fn position(&self) -> i64 {
        let c = self.control();
        match c.pending_locate {
            Some((n, p)) if self.shared.commands_processed.load(Acquire) < n => p,
            _ => self.shared.position.load(Acquire),
        }
    }

    pub fn sample_rate(&self) -> f64 {
        self.shared.sample_rate()
    }

    pub fn play(&self) {
        let mut c = self.control();
        self.push(&mut c, Command::Play);
    }

    pub fn stop(&self) {
        let mut c = self.control();
        // A recording the audio thread hasn't picked up yet ends here.
        let pending = self.shared.pending_session.swap(std::ptr::null_mut(), AcqRel);
        if let Some(s) = unsafe { pending.as_ref() } {
            s.end(0);
        }
        self.push(&mut c, Command::Stop);
    }

    pub fn locate(&self, position: i64) {
        let mut c = self.control();
        self.push(&mut c, Command::Locate(position.max(0)));
    }

    pub fn set_loop(&self, start: i64, end: i64, enabled: bool) {
        let mut c = self.control();
        c.loop_range = LoopRange::new(start, end, enabled);
        self.shared.content().loop_range = c.loop_range;
        self.shared.content_version.fetch_add(1, AcqRel);
        self.ensure_threads(&mut c);
        self.wake_threads(&c);
    }

    pub fn loop_range(&self) -> LoopRange {
        self.control().loop_range
    }

    // ---- playback ---------------------------------------------------------------

    /// Replaces what plays. While rolling, playback continues from the current
    /// position with the new content after a short refill and crossfade.
    pub fn set_playlist(&self, playlist: &Playlist) {
        let prepared = Arc::new(Prepared::new(playlist));
        let mut c = self.control();
        self.shared.content().playlist = prepared;
        self.shared.content_version.fetch_add(1, AcqRel);
        self.ensure_threads(&mut c);
        self.wake_threads(&c);
    }

    // ---- recording ----------------------------------------------------------------

    /// Starts recording `tracks` from the current position (the engine checks
    /// that the sources exist first). Creates the files now.
    pub fn record(&self, tracks: &[RecordTrackSpec]) -> Result<(), String> {
        if tracks.is_empty() {
            return Err("No tracks are armed".into());
        }
        if tracks.len() > MAX_RECORD_TRACKS {
            return Err(format!("At most {MAX_RECORD_TRACKS} tracks can record"));
        }
        if tracks.iter().any(|t| !(1..=2).contains(&t.num_channels)) {
            return Err("A record track has 1 or 2 channels".into());
        }
        if self.state() == TransportState::Recording {
            return Err("Already recording".into());
        }

        let sample_rate = self.sample_rate();
        let threshold = self.shared.rf64_threshold.load(Relaxed);
        let mut files = Vec::with_capacity(tracks.len());
        for t in tracks {
            match wav::WavWriter::create(&t.file, t.num_channels, sample_rate.round() as u32) {
                Ok(mut w) => {
                    w.set_rf64_threshold(threshold);
                    files.push(w);
                }
                Err(e) => {
                    for w in files.drain(..) {
                        let path = w.path().to_owned();
                        drop(w);
                        let _ = std::fs::remove_file(path);
                    }
                    return Err(format!("Can't create {}: {e}", t.file.display()));
                }
            }
        }

        let mut streams = Vec::new();
        let mut session_tracks = Vec::new();
        for t in tracks {
            session_tracks.push(SessionTrack {
                track: t.track,
                first_stream: streams.len(),
                num_channels: t.num_channels,
                file: t.file.clone(),
            });
            streams.extend((0..t.num_channels).map(|channel| RecordStream { source: t.source, channel }));
        }
        let max_block = self.shared.max_block.load(Relaxed);
        let capacity = ((RECORD_BUFFER_SECONDS * sample_rate) as usize).max(16 * max_block);
        let session = Arc::new(RecordSession {
            ring: Ring::new(streams.len(), capacity),
            gaps: SpscQueue::new(1024),
            levels: streams.iter().map(|_| AtomicF32::default()).collect(),
            peaks: session_tracks.iter().map(|_| Mutex::new(Vec::new())).collect(),
            tracks: session_tracks,
            streams,
            started: AtomicBool::new(false),
            start_pos: AtomicI64::new(0),
            frames: AtomicI64::new(0),
            ended: AtomicBool::new(false),
            end_frames: AtomicI64::new(0),
        });

        let mut c = self.control();
        self.ensure_threads(&mut c);
        // The writer owns the session until it is finalized.
        lock(&self.shared.new_sessions).push(writer::WriterJob::new(session.clone(), files));
        let previous = self.shared.pending_session.swap(Arc::as_ptr(&session) as *mut _, AcqRel);
        if let Some(p) = unsafe { previous.as_ref() } {
            p.end(0); // never picked up
        }
        if !self.push(&mut c, Command::Record) {
            let mine = self.shared.pending_session.swap(std::ptr::null_mut(), AcqRel);
            if let Some(s) = unsafe { mine.as_ref() } {
                s.end(0);
            }
            return Err("The transport is busy".into());
        }
        c.current = Some(session);
        Ok(())
    }

    /// The next finalized take, if any.
    pub fn pop_finished_take(&self) -> Option<Take> {
        lock(&self.shared.finished).pop_front()
    }

    fn current_session(&self) -> Option<Arc<RecordSession>> {
        self.control().current.clone()
    }

    /// Peak (linear) of a record track's channel since the last call.
    pub fn record_level(&self, track: i32, channel: usize) -> f32 {
        let Some(s) = self.current_session() else { return 0.0 };
        let Some(t) = s.tracks.iter().find(|t| t.track == track) else { return 0.0 };
        if channel >= t.num_channels || s.ended.load(Acquire) {
            return 0.0;
        }
        s.levels[t.first_stream + channel].exchange(0.0)
    }

    /// Channels of `track` in the current (or last) recording (0 if it isn't in it).
    pub fn record_track_channels(&self, track: i32) -> usize {
        self.current_session().and_then(|s| s.tracks.iter().find(|t| t.track == track).map(|t| t.num_channels)).unwrap_or(0)
    }

    /// Peaks of the current (or last) recording of `track`, from peak
    /// `first`: min/max pairs per channel, interleaved. Returns the number of
    /// peaks written.
    pub fn recording_peaks(&self, track: i32, first: i64, max_peaks: usize, out: &mut [f32]) -> usize {
        let Some(s) = self.current_session() else { return 0 };
        let Some(i) = s.tracks.iter().position(|t| t.track == track) else { return 0 };
        let per_peak = 2 * s.tracks[i].num_channels;
        let peaks = lock(&s.peaks[i]);
        let total = peaks.len() / per_peak;
        let Ok(first) = usize::try_from(first) else { return 0 };
        let n = total.saturating_sub(first).min(max_peaks).min(out.len() / per_peak);
        if n == 0 {
            return 0;
        }
        out[..n * per_peak].copy_from_slice(&peaks[first * per_peak..(first + n) * per_peak]);
        n
    }

    /// Samples recorded so far in the current recording (0 when not recording).
    pub fn recorded_length(&self) -> i64 {
        match self.current_session() {
            Some(s) if !s.ended.load(Acquire) => s.frames.load(Acquire),
            _ => 0,
        }
    }

    // ---- health -----------------------------------------------------------------------

    /// Ring under/overruns (and write errors) since the recorder was created.
    pub fn dropouts(&self) -> u32 {
        self.shared.dropouts.load(Relaxed)
    }

    /// The fullest record ring / emptiest playback ring, 0..1.
    pub fn disk_load(&self) -> f32 {
        self.shared.disk_load.load()
    }

    /// Frames buffered ahead in the playback ring (0 when not rolling).
    pub fn playback_buffered(&self) -> usize {
        self.shared.playback_buffered.load(Relaxed)
    }

    /// Waits until the disk threads have done everything they can do now:
    /// the reader has seen the latest playlist, primed or topped up its rings
    /// and published any switch; the writer has drained what it was given.
    /// For tests and offline rendering (which run faster than real time).
    pub fn wait_for_disk(&self, timeout: Duration) -> bool {
        let s = &self.shared;
        let kick = s.kick.fetch_add(1, AcqRel) + 1;
        {
            let mut c = self.control();
            self.ensure_threads(&mut c);
            self.wake_threads(&c);
        }
        let deadline = Instant::now() + timeout;
        loop {
            let reader_done = s.reader_kick.load(Acquire) >= kick
                && !s.reader_busy.load(Acquire)
                && s.reader_seen.load(Acquire) == s.content_version.load(Acquire)
                && s.reader_epoch.load(Acquire) == s.epoch.load(Acquire);
            let writer_done =
                s.writer_kick.load(Acquire) >= kick && !s.writer_busy.load(Acquire) && lock(&s.new_sessions).is_empty();
            if reader_done && writer_done {
                return true;
            }
            if Instant::now() >= deadline {
                return false;
            }
            {
                let c = self.control();
                self.wake_threads(&c);
            }
            std::thread::sleep(Duration::from_micros(100));
        }
    }

    /// Test hook: the RIFF size above which new recordings become RF64.
    pub fn set_rf64_threshold(&self, bytes: u64) {
        self.shared.rf64_threshold.store(bytes.min(wav::RIFF_LIMIT), Relaxed);
    }
}

impl Drop for Recorder {
    fn drop(&mut self) {
        // The audio thread is gone: end whatever it held, then let the writer
        // finish every take before the threads exit.
        self.audio.get_mut().shutdown();
        let pending = self.shared.pending_session.swap(std::ptr::null_mut(), AcqRel);
        if let Some(s) = unsafe { pending.as_ref() } {
            s.end(0);
        }
        self.shared.shutdown.store(true, Release);
        let threads = std::mem::take(&mut lock(&self.control).threads);
        for t in &threads {
            t.thread().unpark();
        }
        for t in threads {
            let _ = t.join();
        }
    }
}
