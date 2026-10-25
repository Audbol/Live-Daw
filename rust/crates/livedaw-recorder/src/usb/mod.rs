//! The USB recorder / player: the WING's 2/4-track recorder and player of the
//! USB stick (manual 8.2 "4-Track USB Recorder", WING tree `/rec` and
//! `/play`), separate from the multitrack recorder but built on its core
//! (the SPSC rings, the WAV files, a disk thread).
//!
//! * **Recorder** (`/rec`): records the RECORDER output group - four ports,
//!   each tapping a strip's output or a source (see [`RecTap`]) - to a 2- or
//!   4-channel file at the engine's rate: WAV, FLAC or AIFF (16 or 24 bit),
//!   or MP3 (2 channels, 128-320 kbit/s). REC, PAUSE (the file stays open),
//!   STOP (REC then starts a new file) and NEW FILE (the next file starts on
//!   the next sample: nothing is lost).
//! * **Player** (`/play`): plays 1- to 4-channel WAV, FLAC, AIFF and MP3
//!   files (any rate: a file at another rate is resampled and flagged) into
//!   the USB PLAYER source group,
//!   input indices [`USB_PLAYER_SOURCE_BASE`] + 0..3, which channels patch
//!   like any input. PLAY, PAUSE, STOP, previous / next, seek. What plays is
//!   a [`Queue`]: a folder's files or a playlist (SKIP, STOP after, REPEAT);
//!   clips follow each other without a gap.
//!
//! Threads, as in the multitrack recorder:
//! * the **audio thread** ([`audio`]) owns both transports. Per chunk it takes
//!   the commands of the message thread, fills the four player buffers from
//!   the playback ring and copies the four RECORDER taps into the record
//!   ring. It never blocks, allocates or frees.
//! * the **disk thread** ([`disk`]) primes and tops up the playback ring
//!   (deciding what follows a clip shortly before it ends, so late playlist
//!   edits count) and writes the record ring to the WAV file.
//! * the **message thread** calls the methods of [`UsbDeck`].

mod audio;
mod disk;
pub mod queue;
mod resample;

pub use queue::{After, Queue, QueueEntry, QueueKind};

use crate::codec::{create_writer, AudioInfo, AudioWriter, EncodeFormat, FileFormat};
use crate::ring::{Ring, SpscQueue};
use crate::wav::SampleFormat;
use crate::AtomicF32;
use std::cell::UnsafeCell;
use std::collections::VecDeque;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicI64, AtomicPtr, AtomicU32, AtomicU64, AtomicUsize, Ordering::*};
use std::sync::{Arc, Mutex, MutexGuard, OnceLock};
use std::thread::JoinHandle;
use std::time::Duration;

/// Channels of the USB PLAYER source group and the RECORDER output group.
pub const USB_CHANNELS: usize = 4;
/// Input index of USB PLAYER 1 (strip input and ALT input fields, record
/// ports, RECORDER taps): USB PLAYER n is `USB_PLAYER_SOURCE_BASE + n - 1`.
pub const USB_PLAYER_SOURCE_BASE: i32 = 0x20000;

/// Frames of the playback ring (2.7 s at 48 kHz).
pub const PLAY_RING_FRAMES: usize = 1 << 17;
/// Seconds of audio the record ring holds for the disk thread.
pub const REC_BUFFER_SECONDS: f64 = 4.0;
/// The disk thread starts playing once this much is buffered.
pub const PLAY_PRIME_SECONDS: f64 = 0.2;
/// What follows a clip is decided when this much of it is left to play.
pub const DECIDE_AHEAD_SECONDS: f64 = 0.5;
/// Fade-in / fade-out of the player on play, pause, stop and seek (samples).
pub const PLAY_DECLICK_SAMPLES: usize = 256;

/// The USB player channel an input index refers to, if any.
pub fn usb_player_channel(input: i32) -> Option<usize> {
    let n = input.checked_sub(USB_PLAYER_SOURCE_BASE)?;
    usize::try_from(n).ok().filter(|&n| n < USB_CHANNELS)
}

// ---- RECORDER taps --------------------------------------------------------------------

/// Codes of strip taps start here (below: a source's input index).
pub const TAP_STRIP_BASE: i32 = 0x0100_0000;
const TAP_KIND_SPAN: i32 = 0x0010_0000;

/// What a RECORDER port records (`/io/out/REC/n`): nothing, a source (an
/// input port after its source processing: hardware input, playback
/// channel, USB player), or a strip's output - input and aux channels at
/// their direct-out (pre-fader) tap, buses, mains and matrices after fader
/// and pan, as their hardware outputs carry them.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RecTap {
    Off,
    Source(i32),
    /// `kind` is the engine's `StripKind` as i32, `number` as shown (CH 3 =
    /// 3, BUS 12 = 12), `channel` 0 = left, 1 = right.
    Strip {
        kind: i32,
        number: i32,
        channel: usize,
    },
}

impl RecTap {
    /// A single integer (`ld_usb_rec_tap`): -1 off, 0.. a source's input
    /// index, `TAP_STRIP_BASE + kind * 0x100000 + (number - 1) * 2 + channel`.
    pub fn code(self) -> i32 {
        match self {
            Self::Off => -1,
            Self::Source(port) if (0..TAP_STRIP_BASE).contains(&port) => port,
            Self::Source(_) => -1,
            Self::Strip { kind, number, channel } if (0..16).contains(&kind) && (1..=TAP_KIND_SPAN / 2).contains(&number) => {
                TAP_STRIP_BASE + kind * TAP_KIND_SPAN + (number - 1) * 2 + (channel.min(1) as i32)
            }
            Self::Strip { .. } => -1,
        }
    }

    pub fn from_code(code: i32) -> Self {
        if code < 0 {
            Self::Off
        } else if code < TAP_STRIP_BASE {
            Self::Source(code)
        } else {
            let c = code - TAP_STRIP_BASE;
            Self::Strip { kind: c / TAP_KIND_SPAN, number: (c % TAP_KIND_SPAN) / 2 + 1, channel: (c % 2) as usize }
        }
    }
}

// ---- states ----------------------------------------------------------------------------

/// `/play/$actstate`.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub enum PlayerState {
    #[default]
    Stopped = 0,
    Playing = 1,
    Paused = 2,
    /// The clip couldn't be played (missing, not a WAV file).
    Error = 3,
}

impl PlayerState {
    fn from_i32(v: i32) -> Self {
        match v {
            1 => Self::Playing,
            2 => Self::Paused,
            3 => Self::Error,
            _ => Self::Stopped,
        }
    }
}

/// `/rec/$actstate`.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub enum RecState {
    #[default]
    Stopped = 0,
    Recording = 1,
    Paused = 2,
    /// Writing the file failed (the recording goes on into the void).
    Error = 3,
}

impl RecState {
    fn from_i32(v: i32) -> Self {
        match v {
            1 => Self::Recording,
            2 => Self::Paused,
            3 => Self::Error,
            _ => Self::Stopped,
        }
    }
}

/// The recording format (`/rec/channels`, `/rec/resolution`, and the file
/// format of this recorder).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RecFormat {
    /// 2 or 4 (MP3: 2).
    pub channels: usize,
    /// 16 or 24 (WAV, FLAC, AIFF).
    pub bits: u16,
    pub format: FileFormat,
    /// MP3 bit rate (kbit/s).
    pub kbps: u32,
}

impl Default for RecFormat {
    fn default() -> Self {
        Self { channels: 2, bits: 24, format: FileFormat::Wav, kbps: 320 }
    }
}

impl RecFormat {
    /// What the file writer gets.
    pub fn encode_format(self) -> EncodeFormat {
        let sample = if self.bits <= 16 { SampleFormat::Int16 } else { SampleFormat::Int24 };
        EncodeFormat::new(self.format, sample, self.kbps)
    }

    /// The nearest format the recorder writes (MP3 has 2 channels).
    pub fn normalized(self) -> Self {
        let channels = if self.channels <= 2 || self.format == FileFormat::Mp3 { 2 } else { 4 };
        Self { channels, bits: if self.bits <= 16 { 16 } else { 24 }, format: self.format, kbps: self.encode_format().kbps }
    }
}

/// What the player knows about a clip once it has opened it.
#[derive(Clone, Debug, PartialEq)]
pub struct PlayFileInfo {
    pub path: PathBuf,
    pub format: FileFormat,
    /// MP3 bit rate (kbit/s; 0 for the other formats).
    pub kbps: u32,
    pub channels: usize,
    pub sample_rate: u32,
    pub bits: u16,
    pub float: bool,
    /// Length in frames of the engine's rate.
    pub length: i64,
    /// The file's rate differs from the engine's: it is resampled.
    pub rate_mismatch: bool,
}

/// What an audio file of any of the formats holds, for the browser
/// (format, channels, rate, bits, frames).
pub fn audio_file_info(path: &Path) -> Option<AudioInfo> {
    crate::codec::file_info(path).ok()
}

// ---- shared state ------------------------------------------------------------------------

/// Message thread -> audio thread (player).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub(crate) enum PlayCmd {
    #[default]
    None,
    Play,
    Pause,
    Stop,
    /// Cue or play clip `entry` (0: the queue's first) from `pos`.
    Load {
        entry: u64,
        pos: i64,
        play: bool,
    },
    /// Moves the position in the current clip (playing or not).
    Seek(i64),
}

/// Message thread -> audio thread (recorder).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) enum RecCmd {
    #[default]
    None,
    /// Start the pending session, or resume a paused one.
    Rec,
    Pause,
    Stop,
    /// Start the next file of the session's queue.
    NewFile,
}

/// Disk thread -> audio thread: where in the playback ring a clip ends.
#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct Boundary {
    pub epoch: u64,
    /// Ring frames written before the boundary.
    pub at: usize,
    /// The clip that follows (0: the end: stop and cue `cue`).
    pub next: u64,
    pub next_len: i64,
    pub cue: u64,
}

/// Audio thread -> disk thread: an event in the record stream.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) enum RecEventKind {
    #[default]
    NewFile,
    /// Frames the ring could not take (written as silence).
    Gap(usize),
}

#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct RecEvent {
    /// Ring frames committed before the event.
    pub at: usize,
    pub kind: RecEventKind,
}

/// One recording (REC to STOP): its ring and files.
pub(crate) struct RecSession {
    pub channels: usize,
    pub ring: Ring,
    pub events: SpscQueue<RecEvent>,
    /// The first file, then one per NEW FILE (pushed before the command).
    pub files: Mutex<VecDeque<Box<dyn AudioWriter>>>,
    pub ended: AtomicBool,
    /// Ring frames committed when the audio thread let go.
    pub end_at: AtomicUsize,
    pub error: AtomicBool,
}

impl RecSession {
    pub fn end(&self, at: usize) {
        self.end_at.store(at, Relaxed);
        self.ended.store(true, Release);
    }
}

pub(crate) struct DeckShared {
    pub sample_rate: AtomicU64,
    pub max_block: AtomicUsize,

    // ---- player
    pub play_cmds: SpscQueue<PlayCmd>,
    pub play_processed: AtomicU64,
    /// Allocated on first use (message thread), then read by the audio and disk threads.
    pub ring: OnceLock<Ring>,
    pub boundaries: SpscQueue<Boundary>,
    // Published by the audio thread.
    pub play_state: AtomicI32,
    pub play_entry: AtomicU64,
    pub play_pos: AtomicI64,
    pub play_len: AtomicI64,
    pub prime_epoch: AtomicU64,
    pub prime_entry: AtomicU64,
    pub prime_pos: AtomicI64,
    // Published by the disk thread for `prime_epoch`.
    pub ready_epoch: AtomicU64,
    /// 0 ready, 1 nothing to play, 2 the clip can't be read.
    pub ready_status: AtomicI32,
    pub ready_entry: AtomicU64,
    pub ready_len: AtomicI64,
    pub ready_pos: AtomicI64,
    pub queue: Mutex<Queue>,
    pub infos: Mutex<VecDeque<(u64, PlayFileInfo)>>,
    pub play_levels: [AtomicF32; USB_CHANNELS],

    // ---- recorder
    pub rec_cmds: SpscQueue<RecCmd>,
    pub rec_processed: AtomicU64,
    pub pending_session: AtomicPtr<RecSession>,
    pub new_sessions: Mutex<Vec<Arc<RecSession>>>,
    pub rec_state: AtomicI32,
    pub rec_frames: AtomicI64,
    pub rec_file_start: AtomicI64,
    pub rec_levels: [AtomicF32; USB_CHANNELS],
    pub taps: [AtomicI32; USB_CHANNELS],

    pub dropouts: AtomicU32,
    pub disk_busy: AtomicBool,
    /// `wait_for_disk` bumps `kick`; the disk thread copies it at the end of a step.
    pub kick: AtomicU64,
    pub disk_kick: AtomicU64,
    pub shutdown: AtomicBool,
}

impl DeckShared {
    fn new() -> Self {
        // The RECORDER ports start OFF, as on a factory-initialized WING.
        let off = || AtomicI32::new(RecTap::Off.code());
        Self {
            sample_rate: AtomicU64::new(48000f64.to_bits()),
            max_block: AtomicUsize::new(512),
            play_cmds: SpscQueue::new(64),
            play_processed: AtomicU64::new(0),
            ring: OnceLock::new(),
            boundaries: SpscQueue::new(64),
            play_state: AtomicI32::new(0),
            play_entry: AtomicU64::new(0),
            play_pos: AtomicI64::new(0),
            play_len: AtomicI64::new(0),
            prime_epoch: AtomicU64::new(0),
            prime_entry: AtomicU64::new(0),
            prime_pos: AtomicI64::new(0),
            ready_epoch: AtomicU64::new(0),
            ready_status: AtomicI32::new(0),
            ready_entry: AtomicU64::new(0),
            ready_len: AtomicI64::new(0),
            ready_pos: AtomicI64::new(0),
            queue: Mutex::new(Queue::factory()),
            infos: Mutex::new(VecDeque::new()),
            play_levels: Default::default(),
            rec_cmds: SpscQueue::new(64),
            rec_processed: AtomicU64::new(0),
            pending_session: AtomicPtr::new(std::ptr::null_mut()),
            new_sessions: Mutex::new(Vec::new()),
            rec_state: AtomicI32::new(0),
            rec_frames: AtomicI64::new(0),
            rec_file_start: AtomicI64::new(0),
            rec_levels: Default::default(),
            taps: [off(), off(), off(), off()],
            dropouts: AtomicU32::new(0),
            disk_busy: AtomicBool::new(false),
            kick: AtomicU64::new(0),
            disk_kick: AtomicU64::new(0),
            shutdown: AtomicBool::new(false),
        }
    }

    pub fn sample_rate(&self) -> f64 {
        f64::from_bits(self.sample_rate.load(Relaxed))
    }

    pub fn queue(&self) -> MutexGuard<'_, Queue> {
        lock(&self.queue)
    }
}

pub(crate) fn lock<T>(m: &Mutex<T>) -> MutexGuard<'_, T> {
    m.lock().unwrap_or_else(|e| e.into_inner())
}

// ---- the deck ------------------------------------------------------------------------------

struct Control {
    thread: Option<JoinHandle<()>>,
    play_pushed: u64,
    last_play: PlayCmd,
    rec_pushed: u64,
    last_rec: RecCmd,
    session: Option<Arc<RecSession>>,
    rec_file: Option<PathBuf>,
    rec_format: RecFormat,
}

/// The USB recorder / player. The engine owns one; the audio thread calls
/// the methods in [`audio`], everything else is for the message thread.
pub struct UsbDeck {
    shared: Arc<DeckShared>,
    control: Mutex<Control>,
    audio: UnsafeCell<audio::DeckAudio>,
}

// SAFETY: `audio` is only touched by the audio thread (or by `prepare` / drop
// while it can't run); everything else is atomic or behind mutexes.
unsafe impl Sync for UsbDeck {}
unsafe impl Send for UsbDeck {}

impl Default for UsbDeck {
    fn default() -> Self {
        Self::new()
    }
}

impl UsbDeck {
    pub fn new() -> Self {
        Self {
            shared: Arc::new(DeckShared::new()),
            control: Mutex::new(Control {
                thread: None,
                play_pushed: 0,
                last_play: PlayCmd::None,
                rec_pushed: 0,
                last_rec: RecCmd::None,
                session: None,
                rec_file: None,
                rec_format: RecFormat::default(),
            }),
            audio: UnsafeCell::new(audio::DeckAudio::new(512)),
        }
    }

    fn control(&self) -> MutexGuard<'_, Control> {
        lock(&self.control)
    }

    fn ensure_thread(&self, c: &mut Control) {
        if c.thread.is_none() {
            let s = self.shared.clone();
            c.thread = std::thread::Builder::new().name("LiveDAW USB disk".into()).spawn(move || disk::run(s)).ok();
        }
    }

    fn wake(&self, c: &Control) {
        if let Some(t) = &c.thread {
            t.thread().unpark();
        }
    }

    /// Prepares for a device configuration; the player stops and a
    /// recording ends here.
    ///
    /// # Safety
    /// The audio callback must not be running.
    pub unsafe fn prepare(&self, sample_rate: f64, max_block: usize) {
        let max_block = max_block.max(1);
        self.shared.sample_rate.store(sample_rate.to_bits(), Relaxed);
        self.shared.max_block.store(max_block, Relaxed);
        (*self.audio.get()).prepare(&self.shared, max_block);
    }

    pub fn sample_rate(&self) -> f64 {
        self.shared.sample_rate()
    }

    /// Ring under/overruns and write errors since the deck was created.
    pub fn dropouts(&self) -> u32 {
        self.shared.dropouts.load(Relaxed)
    }

    // ---- player ------------------------------------------------------------------

    fn push_play(&self, cmd: PlayCmd) -> bool {
        let mut c = self.control();
        let _ = self.shared.ring.get_or_init(|| Ring::new(USB_CHANNELS, PLAY_RING_FRAMES));
        self.ensure_thread(&mut c);
        let ok = self.shared.play_cmds.push(cmd);
        if ok {
            c.play_pushed += 1;
            c.last_play = cmd;
        }
        self.wake(&c);
        ok
    }

    fn play_pending(&self, c: &Control) -> bool {
        self.shared.play_processed.load(Acquire) < c.play_pushed
    }

    /// PLAY: from the cued position (the queue's first clip if none is cued),
    /// or on from PAUSE.
    pub fn play(&self) {
        self.push_play(PlayCmd::Play);
    }

    /// PAUSE: the position stays (PLAY goes on from there).
    pub fn pause(&self) {
        self.push_play(PlayCmd::Pause);
    }

    /// STOP: back to the start of the clip.
    pub fn stop(&self) {
        self.push_play(PlayCmd::Stop);
    }

    /// Cues (or plays) clip `id` of the queue from `pos` (frames).
    pub fn load(&self, id: u64, pos: i64, play: bool) {
        self.push_play(PlayCmd::Load { entry: id, pos: pos.max(0), play });
    }

    /// Moves the play position in the current clip (frames).
    pub fn seek(&self, pos: i64) {
        self.push_play(PlayCmd::Seek(pos.max(0)));
    }

    fn rolling(&self) -> bool {
        self.player_state() == PlayerState::Playing
    }

    /// `>`: the next clip of the queue (nothing at the end without REPEAT).
    pub fn next(&self) -> bool {
        let current = self.current_entry();
        let target = self.shared.queue().next_of(current);
        match target {
            Some(id) => {
                self.load(id, 0, self.rolling());
                true
            }
            None => false,
        }
    }

    /// `<`: back to the start of the clip, or (in its first two seconds) the
    /// previous clip.
    pub fn prev(&self) -> bool {
        let current = self.current_entry();
        if self.position() as f64 > 2.0 * self.sample_rate() {
            self.load(current, 0, self.rolling());
            return true;
        }
        let target = self.shared.queue().prev_of(current);
        match target {
            Some(id) => {
                self.load(id, 0, self.rolling());
                true
            }
            None => false,
        }
    }

    /// The player's state, including commands the audio thread hasn't taken.
    pub fn player_state(&self) -> PlayerState {
        let c = self.control();
        let s = PlayerState::from_i32(self.shared.play_state.load(Acquire));
        if !self.play_pending(&c) {
            return s;
        }
        match c.last_play {
            PlayCmd::Play => PlayerState::Playing,
            PlayCmd::Pause if s == PlayerState::Playing => PlayerState::Paused,
            PlayCmd::Stop => PlayerState::Stopped,
            PlayCmd::Load { play: true, .. } => PlayerState::Playing,
            _ => s,
        }
    }

    /// The clip playing or cued (its queue id; 0: none).
    pub fn current_entry(&self) -> u64 {
        let c = self.control();
        if self.play_pending(&c) {
            if let PlayCmd::Load { entry, .. } = c.last_play {
                return entry;
            }
        }
        self.shared.play_entry.load(Acquire)
    }

    /// The position in the current clip (frames at the engine's rate).
    pub fn position(&self) -> i64 {
        let c = self.control();
        if self.play_pending(&c) {
            match c.last_play {
                PlayCmd::Load { pos, .. } | PlayCmd::Seek(pos) => return pos,
                PlayCmd::Stop => return 0,
                _ => {}
            }
        }
        self.shared.play_pos.load(Acquire)
    }

    /// The current clip's length (frames at the engine's rate; 0 until opened).
    pub fn length(&self) -> i64 {
        self.shared.play_len.load(Acquire)
    }

    /// What the player knows about clip `id` (after it opened it).
    pub fn file_info(&self, id: u64) -> Option<PlayFileInfo> {
        lock(&self.shared.infos).iter().find(|(i, _)| *i == id).map(|(_, info)| info.clone())
    }

    /// Linear peak of USB PLAYER channel `ch` since the last call.
    pub fn play_level(&self, ch: usize) -> f32 {
        self.shared.play_levels.get(ch).map_or(0.0, |l| l.exchange(0.0))
    }

    /// The queue (folder or playlist). The disk thread reads it when it
    /// decides what follows a clip, so edits count until shortly before the
    /// current clip ends.
    pub fn queue(&self) -> MutexGuard<'_, Queue> {
        self.shared.queue()
    }

    // ---- recorder ----------------------------------------------------------------------

    fn push_rec(&self, c: &mut Control, cmd: RecCmd) -> bool {
        self.ensure_thread(c);
        let ok = self.shared.rec_cmds.push(cmd);
        if ok {
            c.rec_pushed += 1;
            c.last_rec = cmd;
        }
        self.wake(c);
        ok
    }

    /// REC: starts recording to `file` (created now) in `format`, or resumes
    /// a paused recording (then `file` isn't used).
    pub fn record(&self, file: &Path, format: RecFormat) -> Result<(), String> {
        let state = self.rec_state();
        let mut c = self.control();
        match state {
            RecState::Paused => {
                self.push_rec(&mut c, RecCmd::Rec);
                return Ok(());
            }
            RecState::Recording | RecState::Error => return Ok(()),
            RecState::Stopped => {}
        }
        let format = format.normalized();
        let rate = self.sample_rate().round() as u32;
        format.encode_format().check(format.channels, rate)?;
        let writer = create_writer(file, format.channels, rate, format.encode_format())
            .map_err(|e| format!("Can't create {}: {e}", file.display()))?;
        let max_block = self.shared.max_block.load(Relaxed);
        let capacity = ((REC_BUFFER_SECONDS * self.sample_rate()) as usize).max(16 * max_block);
        let session = Arc::new(RecSession {
            channels: format.channels,
            ring: Ring::new(format.channels, capacity),
            events: SpscQueue::new(256),
            files: Mutex::new(VecDeque::from([writer])),
            ended: AtomicBool::new(false),
            end_at: AtomicUsize::new(0),
            error: AtomicBool::new(false),
        });
        self.ensure_thread(&mut c);
        lock(&self.shared.new_sessions).push(session.clone());
        let previous = self.shared.pending_session.swap(Arc::as_ptr(&session) as *mut _, AcqRel);
        // SAFETY: a session stays alive until it is ended (the disk thread holds it).
        if let Some(p) = unsafe { previous.as_ref() } {
            p.end(0);
        }
        if !self.push_rec(&mut c, RecCmd::Rec) {
            let mine = self.shared.pending_session.swap(std::ptr::null_mut(), AcqRel);
            if let Some(s) = unsafe { mine.as_ref() } {
                s.end(0);
            }
            return Err("The recorder is busy".into());
        }
        c.session = Some(session);
        c.rec_file = Some(file.to_owned());
        c.rec_format = format;
        Ok(())
    }

    /// PAUSE: the file stays open; REC (or PAUSE again, at the caller's
    /// choice) resumes.
    pub fn pause_recording(&self) {
        let mut c = self.control();
        self.push_rec(&mut c, RecCmd::Pause);
    }

    /// STOP: the file is finalized; REC then starts a new one.
    pub fn stop_recording(&self) {
        let mut c = self.control();
        // A recording the audio thread hasn't picked up yet ends here.
        let pending = self.shared.pending_session.swap(std::ptr::null_mut(), AcqRel);
        if let Some(s) = unsafe { pending.as_ref() } {
            s.end(0);
        }
        self.push_rec(&mut c, RecCmd::Stop);
    }

    /// NEW FILE: the recording continues in `file` (created now) from the
    /// next sample. Only while recording or paused.
    pub fn new_file(&self, file: &Path) -> Result<(), String> {
        let state = self.rec_state();
        if !matches!(state, RecState::Recording | RecState::Paused) {
            return Err("Not recording".into());
        }
        let mut c = self.control();
        let Some(session) = c.session.clone() else { return Err("Not recording".into()) };
        let rate = self.sample_rate().round() as u32;
        let format = c.rec_format;
        let writer = create_writer(file, format.channels, rate, format.encode_format())
            .map_err(|e| format!("Can't create {}: {e}", file.display()))?;
        lock(&session.files).push_back(writer);
        if !self.push_rec(&mut c, RecCmd::NewFile) {
            lock(&session.files).pop_back();
            let _ = std::fs::remove_file(file);
            return Err("The recorder is busy".into());
        }
        c.rec_file = Some(file.to_owned());
        Ok(())
    }

    /// The recorder's state, including commands the audio thread hasn't taken.
    pub fn rec_state(&self) -> RecState {
        let c = self.control();
        let s = RecState::from_i32(self.shared.rec_state.load(Acquire));
        let s = match &c.session {
            Some(session) if session.error.load(Relaxed) && s != RecState::Stopped => RecState::Error,
            _ => s,
        };
        if self.shared.rec_processed.load(Acquire) >= c.rec_pushed {
            return s;
        }
        match c.last_rec {
            RecCmd::Rec => RecState::Recording,
            RecCmd::Pause if s == RecState::Recording => RecState::Paused,
            RecCmd::Stop => RecState::Stopped,
            _ => s,
        }
    }

    /// The file being recorded (or the last one).
    pub fn rec_file(&self) -> Option<PathBuf> {
        self.control().rec_file.clone()
    }

    /// The format of the current (or last) recording.
    pub fn rec_format(&self) -> RecFormat {
        self.control().rec_format
    }

    /// Frames recorded into the current file (record time).
    pub fn rec_time(&self) -> i64 {
        (self.shared.rec_frames.load(Acquire) - self.shared.rec_file_start.load(Acquire)).max(0)
    }

    /// Frames recorded since REC (all files of the recording).
    pub fn rec_total(&self) -> i64 {
        self.shared.rec_frames.load(Acquire)
    }

    /// Linear peak of RECORDER port `ch` since the last call (also while not recording).
    pub fn rec_level(&self, ch: usize) -> f32 {
        self.shared.rec_levels.get(ch).map_or(0.0, |l| l.exchange(0.0))
    }

    /// What RECORDER port `port` (0-3) records.
    pub fn tap(&self, port: usize) -> RecTap {
        self.shared.taps.get(port).map_or(RecTap::Off, |t| RecTap::from_code(t.load(Relaxed)))
    }

    pub fn set_tap(&self, port: usize, tap: RecTap) {
        if let Some(t) = self.shared.taps.get(port) {
            t.store(tap.code(), Relaxed);
        }
    }

    // ---- offline ----------------------------------------------------------------------

    /// Waits until the disk thread has done what it can do now (tests and
    /// offline rendering, which run faster than real time).
    pub fn wait_for_disk(&self, timeout: Duration) -> bool {
        let s = &self.shared;
        let kick = s.kick.fetch_add(1, AcqRel) + 1;
        {
            let mut c = self.control();
            self.ensure_thread(&mut c);
            self.wake(&c);
        }
        let deadline = std::time::Instant::now() + timeout;
        loop {
            if s.disk_kick.load(Acquire) >= kick && !s.disk_busy.load(Acquire) && lock(&s.new_sessions).is_empty() {
                return true;
            }
            if std::time::Instant::now() >= deadline {
                return false;
            }
            self.wake(&self.control());
            std::thread::sleep(Duration::from_micros(100));
        }
    }
}

impl Drop for UsbDeck {
    fn drop(&mut self) {
        // The audio thread is gone: end the recording, let the disk thread
        // finish the file, then stop it.
        self.audio.get_mut().shutdown();
        let pending = self.shared.pending_session.swap(std::ptr::null_mut(), AcqRel);
        if let Some(s) = unsafe { pending.as_ref() } {
            s.end(0);
        }
        self.shared.shutdown.store(true, Release);
        if let Some(t) = lock(&self.control).thread.take() {
            t.thread().unpark();
            let _ = t.join();
        }
    }
}
