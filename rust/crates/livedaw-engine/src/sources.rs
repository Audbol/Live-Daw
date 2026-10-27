//! Sources (ROUTING > SOURCES, manual pp. 39-40): every hardware input port,
//! every recorder playback channel, the four USB PLAYER channels (the
//! USB deck's player, input `USB_PLAYER_SOURCE_BASE + n`) and the two
//! oscillators (the OSC group, input `OSC_SOURCE_BASE + n`) are sources with a name, colour and
//! icon, a gain, mute, polarity, +48 V (stored only: there is no preamp to
//! switch) and a pairing: MONO, or STEREO / MID-SIDE with its neighbour
//! (ports 1-2, 3-4, ...; the pair shares the first port's settings).
//!
//! The oscillators (`/io/in/OSC/1..2/osc`): each generates a sine, pink or
//! white noise ([`livedaw_dsp::generator`]) at its LEVEL and FREQ; its source
//! MUTE switches it off, as on the WING (the OSC sources have no gain,
//! polarity or +48 V there). A STEREO / MID-SIDE pair runs on OSC 1's
//! settings: the sine in phase on both sides, the noises uncorrelated.
//!
//! Channels patched to a source get its processing first: once per block,
//! before the channels run, the engine processes each source whose settings
//! change its signal into a buffer of its own and hands that buffer to the
//! channels (and to the recorder's input streams) instead of the port's.
//! Sources at their defaults cost nothing: their port is passed through.
//!
//! A MID/SIDE pair decodes to left = M + S, right = M - S (M on the first port).
//!
//! Source tags (SOURCES > TAGS): a source can belong to mute groups; it is
//! muted while one of them is engaged (the WING's `/io/in/../tags` `#M1`...).
//!
//! The USER SIGNAL group (`USR`, input `USR_SOURCE_BASE + n`, manual 2.6
//! "Internal Sources"): USR 1-24 are **User Signals**, mono copies of an
//! input channel, aux, bus, main or matrix (PRE: its tap point, POST: after
//! its processing; L, R or L+R of a stereo one); USR 25-56 are **User
//! Patches**, mono copies of an external source (a port, a playback channel,
//! the USB player). Both are sources like the others (MUTE, polarity, tags,
//! and adjacent pairs as STEREO or MID/SIDE; no gain, as on the WING). User
//! patches are processed with the other sources, before the channels; user
//! signals once every strip has run: outputs take them in the same block,
//! channels one block late (through a `ReturnRing`).

use crate::atomic::AtomicF32;
use crate::params::StripKind;
use crate::routing::{ReturnRing, NUM_STRIP_SOURCES};
use livedaw_dsp::generator::{generator_mode, SignalGenerator, GENERATOR_MODES};
use livedaw_dsp::generator::{GENERATOR_FREQ_MAX_HZ, GENERATOR_FREQ_MIN_HZ, GENERATOR_LEVEL_MAX_DB, GENERATOR_LEVEL_MIN_DB};
use livedaw_dsp::util::{apply_gain_ramp, db_to_gain, peak_level};
use livedaw_recorder::{playback_channel, usb_player_channel, NUM_PLAYBACK_CHANNELS, PLAYBACK_SOURCE_BASE};
use livedaw_recorder::{USB_CHANNELS, USB_PLAYER_SOURCE_BASE};
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicU32, Ordering::Relaxed};
use std::sync::Mutex;

/// Hardware input ports with a source (12 groups of 48); ports beyond pass through.
pub const NUM_HW_SOURCES: usize = 576;
/// The oscillators (the OSC source group): OSC n is input `OSC_SOURCE_BASE + n - 1`.
pub const NUM_OSCILLATORS: usize = 2;
pub const OSC_SOURCE_BASE: i32 = 0x30000;
/// The USER SIGNAL group (`USR`): 24 user signals, then 32 user patches.
/// USR n is input `USR_SOURCE_BASE + n - 1`.
pub const NUM_USER_SIGNALS: usize = 24;
pub const NUM_USER_PATCHES: usize = 32;
pub const NUM_USR_SOURCES: usize = NUM_USER_SIGNALS + NUM_USER_PATCHES;
pub const USR_SOURCE_BASE: i32 = 0x50000;
/// Sources: the hardware ports, the playback channels, the USB PLAYER, the
/// oscillators, then the USR group.
pub const NUM_SOURCES: usize = NUM_HW_SOURCES + NUM_PLAYBACK_CHANNELS + USB_CHANNELS + NUM_OSCILLATORS + NUM_USR_SOURCES;
/// Table index of USB PLAYER 1.
pub const USB_PLAYER_SOURCE_INDEX: usize = NUM_HW_SOURCES + NUM_PLAYBACK_CHANNELS;
/// Table index of OSC 1.
pub const OSC_SOURCE_INDEX: usize = USB_PLAYER_SOURCE_INDEX + USB_CHANNELS;
/// Table index of USR 1 (the first user signal); USR 25 (the first user
/// patch) is `USR_SOURCE_INDEX + NUM_USER_SIGNALS`.
pub const USR_SOURCE_INDEX: usize = OSC_SOURCE_INDEX + NUM_OSCILLATORS;

/// The oscillator an input index refers to, if any.
pub fn osc_channel(input: i32) -> Option<usize> {
    let n = input.checked_sub(OSC_SOURCE_BASE)?;
    usize::try_from(n).ok().filter(|&n| n < NUM_OSCILLATORS)
}

/// The USR source (0-based: 0-23 user signals, 24-55 user patches) an
/// input index refers to, if any.
pub fn usr_channel(input: i32) -> Option<usize> {
    let n = input.checked_sub(USR_SOURCE_BASE)?;
    usize::try_from(n).ok().filter(|&n| n < NUM_USR_SOURCES)
}

/// Where each kind of internal signal sits in the pointer list the channels
/// read besides the hardware ports (`SourceDsp::playback_ptrs`).
pub(crate) mod internal {
    use super::*;
    pub const PLAYBACK: usize = 0;
    pub const USB: usize = PLAYBACK + NUM_PLAYBACK_CHANNELS;
    pub const OSC: usize = USB + USB_CHANNELS;
    pub const USR: usize = OSC + NUM_OSCILLATORS;
    pub const STRIP: usize = USR + NUM_USR_SOURCES;
    pub const COUNT: usize = STRIP + NUM_STRIP_SOURCES;
}

/// An external source a user patch can copy (a hardware port, a playback
/// channel or the USB player; not an oscillator, a user signal or a strip).
pub fn is_external_source(input: i32) -> bool {
    usize::try_from(input).is_ok_and(|p| p < NUM_HW_SOURCES)
        || playback_channel(input).is_some()
        || usb_player_channel(input).is_some()
}

/// A user signal's SOURCE group (`/io/in/USR/n/user/grp`, USR 1-24).
pub const USER_SIGNAL_GROUPS: [&str; 6] = ["OFF", "CH", "AUX", "BUS", "MAIN", "MTX"];
/// A user signal's side of a stereo strip (`user/lr`): L+R (the two sides
/// mixed, each at half), L or R.
pub const USER_SIGNAL_SIDES: [&str; 3] = ["L+R", "L", "R"];
/// Where a user signal takes its strip (`user/tap`): PRE (the tap point:
/// pre-fader, an input channel's TAP point) or POST (after the processing).
pub const USER_SIGNAL_TAPS: [&str; 2] = ["PRE", "POST"];

pub mod user_side {
    pub const BOTH: i32 = 0;
    pub const LEFT: i32 = 1;
    pub const RIGHT: i32 = 2;
}

/// One user signal (USR 1-24), shared with the UI (`ld_engine_user_signals`).
#[repr(C)]
#[derive(Debug)]
pub struct UserSignalParams {
    /// Index into [`USER_SIGNAL_GROUPS`] (0 = OFF).
    pub group: AtomicI32,
    /// The strip's number, 1-based (the WING's `user/in`; a bus by its number).
    pub number: AtomicI32,
    /// Index into [`USER_SIGNAL_SIDES`].
    pub side: AtomicI32,
    /// POST (else PRE).
    pub post: AtomicBool,
}

impl Default for UserSignalParams {
    fn default() -> Self {
        Self { group: AtomicI32::new(0), number: AtomicI32::new(1), side: AtomicI32::new(0), post: AtomicBool::new(false) }
    }
}

impl UserSignalParams {
    pub fn reset(&self) {
        self.group.store(0, Relaxed);
        self.number.store(1, Relaxed);
        self.side.store(user_side::BOTH, Relaxed);
        self.post.store(false, Relaxed);
    }

    pub fn is_default(&self) -> bool {
        self.group.load(Relaxed) == 0
            && self.number.load(Relaxed) == 1
            && self.side.load(Relaxed) == user_side::BOTH
            && !self.post.load(Relaxed)
    }

    /// The strip it copies: (kind, 1-based number), None when OFF.
    pub fn strip(&self) -> Option<(StripKind, i32)> {
        let kind = match self.group.load(Relaxed) {
            1 => StripKind::Channel,
            2 => StripKind::Aux,
            3 => StripKind::Bus,
            4 => StripKind::Main,
            5 => StripKind::Matrix,
            _ => return None,
        };
        Some((kind, self.number.load(Relaxed).max(1)))
    }

    /// Copies strip (kind, 1-based number); None = OFF.
    pub fn set_strip(&self, strip: Option<(StripKind, i32)>) {
        let group = match strip.map(|(k, _)| k) {
            None => 0,
            Some(StripKind::Channel) => 1,
            Some(StripKind::Aux) => 2,
            Some(StripKind::Bus) => 3,
            Some(StripKind::Main) => 4,
            Some(StripKind::Matrix) => 5,
        };
        self.group.store(group, Relaxed);
        if let Some((_, n)) = strip {
            self.number.store(n.max(1), Relaxed);
        }
    }
}

/// One user patch (USR 25-56), shared with the UI (`ld_engine_user_patches`).
#[repr(C)]
#[derive(Debug)]
pub struct UserPatchParams {
    /// The external source it copies (an input index, see
    /// [`is_external_source`]), -1 = OFF.
    pub source: AtomicI32,
}

impl Default for UserPatchParams {
    fn default() -> Self {
        Self { source: AtomicI32::new(-1) }
    }
}

/// The oscillators' defaults: a 1 kHz sine at -6 dBFS, as on a factory-set WING.
pub const OSC_DEFAULT_LEVEL_DB: f32 = -6.0;
/// The oscillators' level before Sidecar took the WING's (shows saved then
/// left an oscillator at this level out).
pub const OSC_LEGACY_LEVEL_DB: f32 = -20.0;
pub const OSC_DEFAULT_FREQ_HZ: f32 = 1000.0;

/// One oscillator's settings, shared with the UI (`ld_engine_oscillators`).
/// Its on / off is its source's MUTE.
#[repr(C)]
#[derive(Debug)]
pub struct OscParams {
    /// Index into [`GENERATOR_MODES`] (SINE, PINK, WHITE).
    pub mode: AtomicI32,
    /// LEVEL, dBFS (the sine's peak; the noises have the sine's RMS).
    pub level_db: AtomicF32,
    pub freq_hz: AtomicF32,
}

impl Default for OscParams {
    fn default() -> Self {
        Self {
            mode: AtomicI32::new(generator_mode::SINE),
            level_db: AtomicF32::new(OSC_DEFAULT_LEVEL_DB),
            freq_hz: AtomicF32::new(OSC_DEFAULT_FREQ_HZ),
        }
    }
}

impl OscParams {
    pub fn reset(&self) {
        self.mode.store(generator_mode::SINE, Relaxed);
        self.level_db.store(OSC_DEFAULT_LEVEL_DB);
        self.freq_hz.store(OSC_DEFAULT_FREQ_HZ);
    }

    pub fn is_default(&self) -> bool {
        self.mode.load(Relaxed) == generator_mode::SINE
            && self.level_db.load() == OSC_DEFAULT_LEVEL_DB
            && self.freq_hz.load() == OSC_DEFAULT_FREQ_HZ
    }

    /// Mode, level and frequency, in range.
    fn get(&self) -> (i32, f32, f32) {
        (
            self.mode.load(Relaxed).clamp(0, GENERATOR_MODES.len() as i32 - 1),
            self.level_db.load().clamp(GENERATOR_LEVEL_MIN_DB, GENERATOR_LEVEL_MAX_DB),
            self.freq_hz.load().clamp(GENERATOR_FREQ_MIN_HZ, GENERATOR_FREQ_MAX_HZ),
        )
    }
}

/// Source pairing.
pub const SOURCE_MODES: [&str; 3] = ["MONO", "STEREO", "MID/SIDE"];

pub mod source_mode {
    pub const MONO: i32 = 0;
    pub const STEREO: i32 = 1;
    pub const MID_SIDE: i32 = 2;
}

/// A source group's GAIN: its range and step (dB), as the WING describes
/// its group's `/io/in/<grp>/<n>/g`. Sidecar's gain is digital (it can't
/// reach the interface's preamps), but it takes the WING's range and steps.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct GainRange {
    pub min: f32,
    pub max: f32,
    pub step: f32,
}

impl GainRange {
    /// LOCAL IN (`LCL`, the full-size WING's own preamps): -2.5 .. +45 dB in
    /// 2.5 dB steps ("lin [-2.5 .. 45.0 dB], 20 steps").
    pub const LOCAL: Self = Self { min: -2.5, max: 45.0, step: 2.5 };
    /// AES50 A / B / C (stage box preamps): -3 .. +45.5 dB in 0.5 dB steps
    /// ("lin [-3.0 .. 45.5 dB], 98 steps").
    pub const AES50: Self = Self { min: -3.0, max: 45.5, step: 0.5 };
    /// Groups without a gain on the WING (WLIVE PLAY, the USB player, the
    /// oscillators): fixed at 0 dB.
    pub const NONE: Self = Self { min: 0.0, max: 0.0, step: 0.0 };

    /// The group has a GAIN.
    pub fn has_gain(&self) -> bool {
        self.max > self.min
    }

    /// The number of values (the WING's "steps").
    pub fn steps(&self) -> u32 {
        if self.has_gain() {
            ((self.max - self.min) / self.step).round() as u32 + 1
        } else {
            1
        }
    }

    /// `db` in range, on the nearest step (0 dB for a group without a gain).
    pub fn clamp(&self, db: f32) -> f32 {
        if !self.has_gain() || !db.is_finite() {
            return 0.0f32.clamp(self.min, self.max);
        }
        let k = ((db.clamp(self.min, self.max) - self.min) / self.step).round();
        (self.min + k * self.step).clamp(self.min, self.max)
    }
}

/// The hardware ports of LOCAL IN (the WING's LCL group; the remote's AES50 A).
pub const NUM_LOCAL_SOURCES: usize = 48;

/// The GAIN range of source `index`: LOCAL IN's ports take the local
/// preamps', the other hardware ports (AES50 B / C and beyond) the AES50
/// stage boxes', the rest has none.
pub fn source_gain_range(index: usize) -> GainRange {
    if index < NUM_LOCAL_SOURCES {
        GainRange::LOCAL
    } else if index < NUM_HW_SOURCES {
        GainRange::AES50
    } else {
        GainRange::NONE
    }
}

/// Default source colour (the WING's factory Dark Blue).
pub const DEFAULT_SOURCE_COLOUR: i32 = crate::params::DEFAULT_COLOUR;

/// A source's factory customization: name, colour, icon and pairing.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SourceFactory {
    pub name: &'static str,
    pub colour: i32,
    pub icon: i32,
    pub mode: i32,
}

impl SourceFactory {
    /// Unnamed, Dark Blue, no icon, mono: every source but the ones below.
    pub const PLAIN: Self = Self { name: "", colour: DEFAULT_SOURCE_COLOUR, icon: 0, mode: source_mode::MONO };
    /// The USB player's first pair on a factory-set WING (`/io/in/PLAY/1-2`):
    /// "2TR", colour 8, icon 608, stereo.
    pub const PLAYER_2TR: Self = Self { name: "2TR", colour: 8, icon: 608, mode: source_mode::STEREO };
}

/// The factory customization of source `index`. (The local inputs keep
/// icon 0: they are AES50 A's ports here, whose factory icon is 0; the
/// WING gives its LCL group icon 1.)
pub fn source_factory(index: usize) -> SourceFactory {
    match index.checked_sub(USB_PLAYER_SOURCE_INDEX) {
        Some(0 | 1) => SourceFactory::PLAYER_2TR,
        _ => SourceFactory::PLAIN,
    }
}

/// One source's settings, shared with the UI (`ld_engine_sources`).
#[repr(C)]
#[derive(Debug)]
pub struct SourceParams {
    pub gain_db: AtomicF32,
    pub mute: AtomicBool,
    pub invert: AtomicBool,
    /// +48 V phantom power (stored with the show; nothing to switch here).
    pub phantom: AtomicBool,
    /// Index into [`SOURCE_MODES`]; both sources of a pair hold the same mode.
    pub mode: AtomicI32,
    /// WING colour 1..18 and icon number (0 = none).
    pub colour: AtomicI32,
    pub icon: AtomicI32,
    /// Peak since the last read (after the source's processing).
    pub peak: AtomicF32,
    /// ABI 8, SOURCES > TAGS: bit n = mute group n+1 mutes the source.
    pub mute_groups: AtomicU32,
}

impl Default for SourceParams {
    fn default() -> Self {
        Self {
            gain_db: AtomicF32::new(0.0),
            mute: AtomicBool::new(false),
            invert: AtomicBool::new(false),
            phantom: AtomicBool::new(false),
            mode: AtomicI32::new(source_mode::MONO),
            colour: AtomicI32::new(DEFAULT_SOURCE_COLOUR),
            icon: AtomicI32::new(0),
            peak: AtomicF32::new(0.0),
            mute_groups: AtomicU32::new(0),
        }
    }
}

impl SourceParams {
    /// INITIALIZE: back to the defaults `f` (the pairing is left to the table).
    fn reset(&self, f: &SourceFactory) {
        self.gain_db.store(0.0);
        self.mute.store(false, Relaxed);
        self.invert.store(false, Relaxed);
        self.phantom.store(false, Relaxed);
        self.colour.store(f.colour, Relaxed);
        self.icon.store(f.icon, Relaxed);
        self.mute_groups.store(0, Relaxed);
    }

    /// True if the settings are the defaults `f` (shows store the others).
    pub fn is_default_for(&self, f: &SourceFactory) -> bool {
        self.gain_db.load() == 0.0
            && !self.mute.load(Relaxed)
            && !self.invert.load(Relaxed)
            && !self.phantom.load(Relaxed)
            && self.mode.load(Relaxed) == f.mode
            && self.colour.load(Relaxed) == f.colour
            && self.icon.load(Relaxed) == f.icon
            && self.mute_groups.load(Relaxed) == 0
    }

    /// True if the settings are a plain source's defaults ([`SourceFactory::PLAIN`]).
    pub fn is_default(&self) -> bool {
        self.is_default_for(&SourceFactory::PLAIN)
    }

    /// Muted: its own MUTE, or one of its mute groups is engaged.
    pub fn is_muted(&self, mute_groups: u32) -> bool {
        self.mute.load(Relaxed) || self.mute_groups.load(Relaxed) & mute_groups != 0
    }

    /// The gain the source applies (mute, mute groups, gain in `range`, polarity).
    fn gain(&self, range: GainRange, mute_groups: u32) -> f32 {
        if self.is_muted(mute_groups) {
            return 0.0;
        }
        let g = db_to_gain(range.clamp(self.gain_db.load()));
        if self.invert.load(Relaxed) {
            -g
        } else {
            g
        }
    }
}

/// The table index of input port `port` (a hardware input or a playback
/// channel), None for ports without a source.
pub fn source_index(port: i32) -> Option<usize> {
    if let Some(n) = usr_channel(port) {
        return Some(USR_SOURCE_INDEX + n);
    }
    if let Some(n) = osc_channel(port) {
        return Some(OSC_SOURCE_INDEX + n);
    }
    if let Some(n) = usb_player_channel(port) {
        return Some(USB_PLAYER_SOURCE_INDEX + n);
    }
    match playback_channel(port) {
        Some(n) => Some(NUM_HW_SOURCES + n),
        None => usize::try_from(port).ok().filter(|&p| p < NUM_HW_SOURCES),
    }
}

/// The input port of table index `index` (inverse of [`source_index`]).
pub fn source_port(index: usize) -> Option<i32> {
    if index < NUM_HW_SOURCES {
        Some(index as i32)
    } else if index < USB_PLAYER_SOURCE_INDEX {
        Some(PLAYBACK_SOURCE_BASE + (index - NUM_HW_SOURCES) as i32)
    } else if index < OSC_SOURCE_INDEX {
        Some(USB_PLAYER_SOURCE_BASE + (index - USB_PLAYER_SOURCE_INDEX) as i32)
    } else if index < USR_SOURCE_INDEX {
        Some(OSC_SOURCE_BASE + (index - OSC_SOURCE_INDEX) as i32)
    } else if index < NUM_SOURCES {
        Some(USR_SOURCE_BASE + (index - USR_SOURCE_INDEX) as i32)
    } else {
        None
    }
}

/// The first source of `index`'s pair.
pub fn pair_first(index: usize) -> usize {
    index & !1
}

/// Every source's settings and names.
pub struct SourceTable {
    params: Box<[SourceParams]>,
    /// Names (message thread; empty = the port's name).
    names: Mutex<Vec<String>>,
    osc: [OscParams; NUM_OSCILLATORS],
    /// USR 1-24 and USR 25-56: what each copies.
    user_signals: [UserSignalParams; NUM_USER_SIGNALS],
    user_patches: [UserPatchParams; NUM_USER_PATCHES],
}

impl Default for SourceTable {
    fn default() -> Self {
        let t = Self {
            params: (0..NUM_SOURCES).map(|_| SourceParams::default()).collect(),
            names: Mutex::new(vec![String::new(); NUM_SOURCES]),
            osc: Default::default(),
            user_signals: Default::default(),
            user_patches: Default::default(),
        };
        t.reset();
        t
    }
}

impl SourceTable {
    pub fn params(&self) -> &[SourceParams] {
        &self.params
    }

    pub fn get(&self, index: usize) -> Option<&SourceParams> {
        self.params.get(index)
    }

    /// Sets source `index`'s GAIN, in its group's range and on its steps
    /// ([`source_gain_range`]).
    pub fn set_gain(&self, index: usize, db: f32) {
        if let Some(p) = self.params.get(index) {
            p.gain_db.store(source_gain_range(index).clamp(db));
        }
    }

    /// Puts every source's GAIN in its group's range (a show or snapshot
    /// saved with the wider range Sidecar had before).
    pub fn clamp_gains(&self) {
        for (i, p) in self.params.iter().enumerate() {
            let db = p.gain_db.load();
            let clamped = source_gain_range(i).clamp(db);
            if clamped != db {
                p.gain_db.store(clamped);
            }
        }
    }

    /// Oscillator `n` (0-based).
    pub fn osc(&self, n: usize) -> Option<&OscParams> {
        self.osc.get(n)
    }

    pub fn oscillators(&self) -> &[OscParams] {
        &self.osc
    }

    /// USR 1-24: what each user signal copies.
    pub fn user_signals(&self) -> &[UserSignalParams] {
        &self.user_signals
    }

    /// USR 25-56: the source each user patch copies.
    pub fn user_patches(&self) -> &[UserPatchParams] {
        &self.user_patches
    }

    /// Patches user patch `n` (0-based) to `source`: an external source, or
    /// OFF (-1) for anything else.
    pub fn set_user_patch(&self, n: usize, source: i32) {
        if let Some(p) = self.user_patches.get(n) {
            p.source.store(if is_external_source(source) { source } else { -1 }, Relaxed);
        }
    }

    /// The routing of the USR group back to OFF (a new console).
    pub fn reset_user_routing(&self) {
        self.user_signals.iter().for_each(UserSignalParams::reset);
        self.user_patches.iter().for_each(|p| p.source.store(-1, Relaxed));
    }

    pub fn name(&self, index: usize) -> String {
        self.names.lock().unwrap_or_else(|e| e.into_inner()).get(index).cloned().unwrap_or_default()
    }

    pub fn set_name(&self, index: usize, name: &str) {
        if let Some(n) = self.names.lock().unwrap_or_else(|e| e.into_inner()).get_mut(index) {
            *n = name.to_owned();
        }
    }

    /// Pairs `index` with its neighbour (STEREO, MID/SIDE) or splits the pair
    /// (MONO): both sources of the pair get the mode, and a new pair takes the
    /// first source's settings.
    pub fn set_mode(&self, index: usize, mode: i32) {
        let first = pair_first(index);
        let (Some(a), Some(b)) = (self.params.get(first), self.params.get(first + 1)) else { return };
        let mode = mode.clamp(0, SOURCE_MODES.len() as i32 - 1);
        if mode != source_mode::MONO {
            b.gain_db.store(a.gain_db.load());
            b.mute.store(a.mute.load(Relaxed), Relaxed);
            b.invert.store(a.invert.load(Relaxed), Relaxed);
            b.phantom.store(a.phantom.load(Relaxed), Relaxed);
            b.mute_groups.store(a.mute_groups.load(Relaxed), Relaxed);
        }
        a.mode.store(mode, Relaxed);
        b.mode.store(mode, Relaxed);
    }

    /// True if source `index` is at its factory settings with its factory
    /// name (what a show leaves out).
    pub fn is_factory(&self, index: usize) -> bool {
        let f = source_factory(index);
        self.params.get(index).is_some_and(|p| p.is_default_for(&f)) && self.name(index) == f.name
    }

    /// INITIALIZE: the source's settings (its pair's, if paired) and name back
    /// to the defaults; the pairing stays.
    pub fn initialize(&self, index: usize) {
        let Some(p) = self.params.get(index) else { return };
        let paired = p.mode.load(Relaxed) != source_mode::MONO;
        let first = pair_first(index);
        let indices = if paired { first..first + 2 } else { index..index + 1 };
        for i in indices {
            let f = source_factory(i);
            self.params[i].reset(&f);
            self.set_name(i, f.name);
            if let Some(o) = i.checked_sub(OSC_SOURCE_INDEX).and_then(|n| self.osc.get(n)) {
                o.reset();
            }
        }
    }

    /// Source `index` back to its factory settings, pairing and name (a scene
    /// that holds the sources and leaves this one out).
    pub fn reset_source(&self, index: usize) {
        let Some(p) = self.params.get(index) else { return };
        let f = source_factory(index);
        p.reset(&f);
        p.mode.store(f.mode, Relaxed);
        self.set_name(index, f.name);
    }

    /// Everything back to the defaults (a new console).
    pub fn reset(&self) {
        let mut names = self.names.lock().unwrap_or_else(|e| e.into_inner());
        for (i, (p, name)) in self.params.iter().zip(names.iter_mut()).enumerate() {
            let f = source_factory(i);
            p.reset(&f);
            p.mode.store(f.mode, Relaxed);
            name.clear();
            name.push_str(f.name);
        }
        self.osc.iter().for_each(OscParams::reset);
        drop(names);
        self.reset_user_routing();
    }

    /// The defaults before Sidecar took the WING's for the USB player and
    /// the oscillators: what a show saved then left out had these values.
    pub fn apply_legacy_defaults(&self) {
        let plain = SourceFactory::PLAIN;
        for i in 0..NUM_SOURCES {
            if source_factory(i) != plain {
                self.params[i].reset(&plain);
                self.params[i].mode.store(plain.mode, Relaxed);
                self.set_name(i, plain.name);
            }
        }
        for o in &self.osc {
            o.level_db.store(OSC_LEGACY_LEVEL_DB);
        }
    }
}

const IDENTITY: [f32; 4] = [1.0, 0.0, 0.0, 1.0];
const MID_SIDE: [f32; 4] = [1.0, 1.0, 1.0, -1.0];

/// Audio-thread state of the sources: processed buffers, ramps and the
/// pointers the channels read.
#[derive(Default)]
pub(crate) struct SourceDsp {
    /// One buffer per source the device has (hardware ports, then playback).
    buffers: Vec<Vec<f32>>,
    last_gain: Vec<f32>,
    /// Per pair (indexed by the pair's first source / 2).
    last_matrix: Vec<[f32; 4]>,
    num_hw: usize,
    /// What the channels read: hardware inputs by port, and the internal
    /// signals in the layout of [`internal`] (playback channels, the USB
    /// PLAYER, the oscillators, the USR group, the strip sources; see
    /// `source_ptr`).
    hw_out: Vec<*const f32>,
    playback_out: Vec<*const f32>,
    /// The oscillators and the signal they generate this block.
    generators: Vec<SignalGenerator>,
    generated: Vec<Vec<f32>>,
    /// The first buffer of the USR group.
    usr_slot: usize,
    /// User signals: this block's copies of their strips (before their
    /// source processing), the processed signals one block late for the
    /// channels, and which signals are routed (their ring is written).
    user_raw: Vec<Vec<f32>>,
    user_ring: ReturnRing,
    user_live: [bool; NUM_USER_SIGNALS],
}

impl SourceDsp {
    /// Allocates for a device (message thread, device stopped).
    pub fn prepare(&mut self, num_hw_inputs: usize, block_size: usize, sample_rate: f64) {
        let num_hw = num_hw_inputs.min(NUM_HW_SOURCES);
        let usr_slot = num_hw.next_multiple_of(2) + NUM_PLAYBACK_CHANNELS + USB_CHANNELS + NUM_OSCILLATORS;
        let count = usr_slot + NUM_USR_SOURCES;
        self.num_hw = num_hw;
        self.usr_slot = usr_slot;
        self.buffers = vec![vec![0.0; block_size]; count];
        self.last_gain = vec![1.0; count];
        self.last_matrix = vec![IDENTITY; count / 2];
        self.hw_out = vec![std::ptr::null(); num_hw_inputs];
        self.playback_out = vec![std::ptr::null(); internal::COUNT];
        self.generators = (0..NUM_OSCILLATORS as u32).map(|n| SignalGenerator::new(sample_rate, 0x9e37_79b9 ^ (n + 1))).collect();
        self.generated = vec![vec![0.0; block_size]; NUM_OSCILLATORS];
        self.user_raw = vec![vec![0.0; block_size]; NUM_USER_SIGNALS];
        self.user_ring = ReturnRing::new(NUM_USER_SIGNALS, block_size);
        self.user_live = [false; NUM_USER_SIGNALS];
    }

    /// Prepared for blocks of `n` samples and this many hardware inputs.
    fn ready(&self, num_hw_inputs: usize, n: usize) -> bool {
        self.hw_out.len() == num_hw_inputs && n <= self.buffers.first().map_or(0, Vec::len)
    }

    /// The signal of strip source `slot` (`routing::strip_source_slot`)
    /// the channels read this block (null: silent).
    pub fn set_strip_return(&mut self, slot: usize, ptr: *const f32) {
        if let Some(p) = self.playback_out.get_mut(internal::STRIP + slot) {
            *p = ptr;
        }
    }

    /// The buffer user signal `i` (0-based) copies its strip into this block
    /// (None when not routed or not prepared for `n`).
    pub fn user_raw_mut(&mut self, i: usize, n: usize) -> Option<&mut [f32]> {
        if !self.user_live.get(i).copied().unwrap_or(false) {
            return None;
        }
        self.user_raw.get_mut(i).filter(|b| b.len() >= n).map(|b| &mut b[..n])
    }

    /// The user signals' source processing, once every strip has run: from
    /// now on in this block the outputs read them as they are, and the
    /// processed signals go into the ring the channels read next block.
    ///
    /// # Safety
    /// Audio thread, after `process` in the same block.
    pub unsafe fn process_user_signals(&mut self, table: &SourceTable, pos: usize, n: usize, mute_groups: u32) {
        if self.user_raw.is_empty() || n > self.user_raw[0].len() {
            return;
        }
        for pair in 0..NUM_USER_SIGNALS / 2 {
            let i = pair * 2;
            let raw: [*const f32; 2] =
                std::array::from_fn(|ch| if self.user_live[i + ch] { self.user_raw[i + ch].as_ptr() } else { std::ptr::null() });
            let out = self.process_pair(table, USR_SOURCE_INDEX + i, self.usr_slot + i, raw, n, mute_groups);
            for (ch, &p) in out.iter().enumerate() {
                if self.user_live[i + ch] && !p.is_null() {
                    self.user_ring.write(i + ch, pos, std::slice::from_raw_parts(p, n));
                }
                self.playback_out[internal::USR + i + ch] = p;
            }
        }
    }

    pub fn hw_ptrs(&self) -> &[*const f32] {
        &self.hw_out
    }

    pub fn playback_ptrs(&self) -> &[*const f32] {
        &self.playback_out
    }

    /// Processes this block's sources from the raw port buffers (`usb`: the
    /// USB deck's player channels; `mute_groups`: the engaged mute groups;
    /// `pos`: the return clock, where the user signals are read one block late).
    ///
    /// # Safety
    /// Audio thread. Non-null pointers must be valid for `n` samples.
    #[allow(clippy::too_many_arguments)]
    pub unsafe fn process(
        &mut self,
        table: &SourceTable,
        hw: &[*const f32],
        playback: &[*const f32],
        usb: &[*const f32; USB_CHANNELS],
        n: usize,
        mute_groups: u32,
        pos: usize,
    ) {
        if !self.ready(hw.len(), n) {
            // Not prepared for this layout: pass the ports through (internal signals silent).
            self.hw_out.clear();
            self.playback_out.clear();
            self.hw_out.extend_from_slice(&hw[..hw.len().min(self.hw_out.capacity())]);
            let room = self.playback_out.capacity();
            self.playback_out.extend_from_slice(&playback[..playback.len().min(room)]);
            let room = self.playback_out.capacity() - self.playback_out.len();
            self.playback_out.extend_from_slice(&usb[..USB_CHANNELS.min(room)]);
            let room = self.playback_out.capacity() - self.playback_out.len();
            self.playback_out.extend(std::iter::repeat_n(std::ptr::null(), room));
            return;
        }
        // Ports beyond the source table pass through.
        for (out, &raw) in self.hw_out.iter_mut().zip(hw).skip(self.num_hw) {
            *out = raw;
        }
        let hw_pairs = self.num_hw.div_ceil(2);
        for pair in 0..hw_pairs {
            let first = pair * 2;
            let raw =
                [hw.get(first).copied().unwrap_or(std::ptr::null()), hw.get(first + 1).copied().unwrap_or(std::ptr::null())];
            let [l, r] = self.process_pair(table, first, first, raw, n, mute_groups);
            self.hw_out[first] = l;
            if first + 1 < self.num_hw {
                self.hw_out[first + 1] = r;
            }
        }
        let base = self.num_hw.next_multiple_of(2);
        for pair in 0..NUM_PLAYBACK_CHANNELS / 2 {
            let ch = pair * 2;
            let raw = [
                playback.get(ch).copied().unwrap_or(std::ptr::null()),
                playback.get(ch + 1).copied().unwrap_or(std::ptr::null()),
            ];
            let [l, r] = self.process_pair(table, NUM_HW_SOURCES + ch, base + ch, raw, n, mute_groups);
            self.playback_out[internal::PLAYBACK + ch] = l;
            self.playback_out[internal::PLAYBACK + ch + 1] = r;
        }
        // The USB PLAYER, after the playback channels.
        let base = base + NUM_PLAYBACK_CHANNELS;
        for pair in 0..USB_CHANNELS / 2 {
            let ch = pair * 2;
            let [l, r] =
                self.process_pair(table, USB_PLAYER_SOURCE_INDEX + ch, base + ch, [usb[ch], usb[ch + 1]], n, mute_groups);
            self.playback_out[internal::USB + ch] = l;
            self.playback_out[internal::USB + ch + 1] = r;
        }
        // The oscillators, after the USB PLAYER.
        let raw = self.generate(table, n);
        let [l, r] = self.process_pair(table, OSC_SOURCE_INDEX, base + USB_CHANNELS, raw, n, mute_groups);
        self.playback_out[internal::OSC] = l;
        self.playback_out[internal::OSC + 1] = r;

        // USR 25-56, the user patches: copies of the processed external sources.
        for pair in 0..NUM_USER_PATCHES / 2 {
            let i = NUM_USER_SIGNALS + pair * 2;
            let raw: [*const f32; 2] = std::array::from_fn(|ch| {
                let src = table.user_patches[i - NUM_USER_SIGNALS + ch].source.load(Relaxed);
                self.external_ptr(src).unwrap_or(std::ptr::null())
            });
            let [l, r] = self.process_pair(table, USR_SOURCE_INDEX + i, self.usr_slot + i, raw, n, mute_groups);
            self.playback_out[internal::USR + i] = l;
            self.playback_out[internal::USR + i + 1] = r;
        }
        // USR 1-24, the user signals: the channels read last block's.
        for i in 0..NUM_USER_SIGNALS {
            let routed = table.user_signals[i].strip().is_some();
            if routed && !self.user_live[i] {
                self.user_ring.clear(i); // newly routed: no stale signal
            }
            self.user_live[i] = routed;
            self.playback_out[internal::USR + i] =
                if routed { self.user_ring.read(i, pos, n).map_or(std::ptr::null(), <[f32]>::as_ptr) } else { std::ptr::null() };
        }
    }

    /// The processed buffer of an external source (a user patch's copy).
    fn external_ptr(&self, input: i32) -> Option<*const f32> {
        if !is_external_source(input) {
            return None;
        }
        let p = if let Some(n) = playback_channel(input) {
            self.playback_out.get(internal::PLAYBACK + n)
        } else if let Some(n) = usb_player_channel(input) {
            self.playback_out.get(internal::USB + n)
        } else {
            self.hw_out.get(usize::try_from(input).ok()?)
        };
        p.copied().filter(|p| !p.is_null())
    }
    /// Runs the oscillators into `generated` (a pair on OSC 1's settings, its
    /// sine in phase) and returns their buffers.
    fn generate(&mut self, table: &SourceTable, n: usize) -> [*const f32; 2] {
        let ([g1, g2, ..], [b1, b2, ..]) = (&mut self.generators[..], &mut self.generated[..]) else {
            return [std::ptr::null(); 2];
        };
        let paired = table.params[OSC_SOURCE_INDEX].mode.load(Relaxed) != source_mode::MONO;
        let (mode, level, freq) = table.osc[0].get();
        if paired {
            g2.set_phase(g1.phase());
        }
        g1.process(&mut b1[..n], mode, freq, level);
        let (mode, level, freq) = if paired { (mode, level, freq) } else { table.osc[1].get() };
        g2.process(&mut b2[..n], mode, freq, level);
        [b1.as_ptr(), b2.as_ptr()]
    }

    /// One pair: `index` is the table index of its first source, `slot` the
    /// index of its first buffer. Returns the pointers the channels read.
    unsafe fn process_pair(
        &mut self,
        table: &SourceTable,
        index: usize,
        slot: usize,
        raw: [*const f32; 2],
        n: usize,
        mute_groups: u32,
    ) -> [*const f32; 2] {
        let p = &table.params[index..index + 2];
        let mode = p[0].mode.load(Relaxed);
        // A pair shares the first source's settings.
        let range = source_gain_range(index);
        let gains = if mode == source_mode::MONO {
            [p[0].gain(range, mute_groups), p[1].gain(source_gain_range(index + 1), mute_groups)]
        } else {
            [p[0].gain(range, mute_groups); 2]
        };
        let matrix = if mode == source_mode::MID_SIDE { MID_SIDE } else { IDENTITY };
        let last = [self.last_gain[slot], self.last_gain[slot + 1]];
        let last_matrix = self.last_matrix[slot / 2];
        let plain = gains == [1.0, 1.0] && last == [1.0, 1.0] && matrix == IDENTITY && last_matrix == IDENTITY;

        let mut out = raw;
        if !plain && (!raw[0].is_null() || !raw[1].is_null()) {
            let (a, b) = self.buffers.split_at_mut(slot + 1);
            let bufs = [&mut a[slot][..n], &mut b[0][..n]];
            for (ch, buf) in bufs.into_iter().enumerate() {
                if raw[ch].is_null() {
                    buf.fill(0.0);
                } else {
                    buf.copy_from_slice(std::slice::from_raw_parts(raw[ch], n));
                    apply_gain_ramp(buf, last[ch], gains[ch]);
                }
            }
            if matrix != IDENTITY || last_matrix != IDENTITY {
                let (a, b) = self.buffers.split_at_mut(slot + 1);
                crossfade_matrix(&mut a[slot][..n], &mut b[0][..n], last_matrix, matrix);
            }
            // A decoded pair has signal on both sides; otherwise a missing port stays missing.
            let decoded = matrix != IDENTITY || last_matrix != IDENTITY;
            for ch in 0..2 {
                out[ch] = if raw[ch].is_null() && !decoded { std::ptr::null() } else { self.buffers[slot + ch].as_ptr() };
            }
        }
        self.last_gain[slot] = gains[0];
        self.last_gain[slot + 1] = gains[1];
        self.last_matrix[slot / 2] = matrix;
        for ch in 0..2 {
            if !out[ch].is_null() {
                p[ch].peak.fetch_max(peak_level(std::slice::from_raw_parts(out[ch], n)));
            }
        }
        out
    }
}

/// `(l, r) = m * (l, r)` with the matrix crossfading from `last` to `target`.
fn crossfade_matrix(l: &mut [f32], r: &mut [f32], last: [f32; 4], target: [f32; 4]) {
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn indices_and_ports() {
        assert_eq!(source_index(0), Some(0));
        assert_eq!(source_index(NUM_HW_SOURCES as i32), None);
        assert_eq!(source_index(PLAYBACK_SOURCE_BASE + 3), Some(NUM_HW_SOURCES + 3));
        assert_eq!(source_index(-1), None);
        assert_eq!(source_index(USB_PLAYER_SOURCE_BASE + 1), Some(USB_PLAYER_SOURCE_INDEX + 1));
        assert_eq!(source_index(USB_PLAYER_SOURCE_BASE + USB_CHANNELS as i32), None);
        for i in [0, 5, NUM_HW_SOURCES - 1, NUM_HW_SOURCES, USB_PLAYER_SOURCE_INDEX - 1, USB_PLAYER_SOURCE_INDEX, NUM_SOURCES - 1]
        {
            assert_eq!(source_index(source_port(i).unwrap()), Some(i));
        }
        assert_eq!(source_port(NUM_SOURCES), None);
        assert_eq!(pair_first(5), 4);
    }

    #[test]
    fn sources_start_as_a_factory_set_wing() {
        // The USB player's first pair: "2TR", colour 8, icon 608, stereo; the
        // rest unnamed, Dark Blue, no icon, mono; the oscillators at -6 dB.
        let t = SourceTable::default();
        let p = USB_PLAYER_SOURCE_INDEX;
        for i in [p, p + 1] {
            assert_eq!(t.name(i), "2TR");
            let s = t.get(i).unwrap();
            assert_eq!((s.colour.load(Relaxed), s.icon.load(Relaxed), s.mode.load(Relaxed)), (8, 608, source_mode::STEREO));
            assert!(t.is_factory(i) && !s.is_default());
        }
        assert!(t.get(p + 2).unwrap().is_default() && t.name(p + 2).is_empty() && t.is_factory(0));
        assert_eq!(t.osc(0).unwrap().level_db.load(), -6.0);
        // INITIALIZE brings a renamed player source back.
        t.set_name(p, "Walk-in");
        t.get(p).unwrap().icon.store(5, Relaxed);
        assert!(!t.is_factory(p));
        t.initialize(p);
        assert!(t.is_factory(p) && t.is_factory(p + 1));
        // What shows saved before these defaults left out.
        t.apply_legacy_defaults();
        assert!(t.get(p).unwrap().is_default() && t.name(p).is_empty());
        assert_eq!(t.osc(1).unwrap().level_db.load(), OSC_LEGACY_LEVEL_DB);
    }

    /// GAIN takes the WING's range and steps of the source's group:
    /// LOCAL IN -2.5 .. +45 dB in 2.5 dB steps, AES50 -3 .. +45.5 dB in 0.5 dB
    /// steps, none for WLIVE PLAY, the USB player and the oscillators.
    #[test]
    fn gain_ranges_follow_the_wing_groups() {
        assert_eq!(source_gain_range(0), GainRange::LOCAL);
        assert_eq!(source_gain_range(NUM_LOCAL_SOURCES - 1), GainRange::LOCAL);
        assert_eq!(source_gain_range(NUM_LOCAL_SOURCES), GainRange::AES50);
        assert_eq!(source_gain_range(NUM_HW_SOURCES - 1), GainRange::AES50);
        for none in [NUM_HW_SOURCES, USB_PLAYER_SOURCE_INDEX, OSC_SOURCE_INDEX + 1] {
            assert!(!source_gain_range(none).has_gain(), "{none}");
        }
        assert!(!source_gain_range(NUM_SOURCES).has_gain());
        assert_eq!((GainRange::LOCAL.steps(), GainRange::AES50.steps(), GainRange::NONE.steps()), (20, 98, 1));

        let l = GainRange::LOCAL;
        assert_eq!(
            [l.clamp(60.0), l.clamp(-12.0), l.clamp(0.0), l.clamp(6.0), l.clamp(1.2), l.clamp(f32::NAN)],
            [45.0, -2.5, 0.0, 5.0, 0.0, 0.0]
        );
        let a = GainRange::AES50;
        assert_eq!([a.clamp(60.0), a.clamp(-12.0), a.clamp(0.0), a.clamp(6.2), a.clamp(-0.74)], [45.5, -3.0, 0.0, 6.0, -0.5]);
        assert_eq!(GainRange::NONE.clamp(12.0), 0.0);

        // Set in range; values from before (-12 .. +60 everywhere) clamp.
        let t = SourceTable::default();
        let gain = |i: usize| t.get(i).unwrap().gain_db.load();
        t.set_gain(0, 60.0);
        t.set_gain(48, -12.0);
        t.set_gain(USB_PLAYER_SOURCE_INDEX, 6.0);
        assert_eq!([gain(0), gain(48), gain(USB_PLAYER_SOURCE_INDEX)], [45.0, -3.0, 0.0]);
        for (i, db) in [(1, 60.0), (49, -12.0), (NUM_HW_SOURCES, 12.0), (2, 25.0)] {
            t.get(i).unwrap().gain_db.store(db);
        }
        t.clamp_gains();
        assert_eq!([gain(1), gain(49), gain(NUM_HW_SOURCES), gain(2)], [45.0, -3.0, 0.0, 25.0]);

        // The audio takes the clamped gain even before that.
        let p = &t.params()[3];
        p.gain_db.store(60.0);
        assert!((p.gain(source_gain_range(3), 0) - db_to_gain(45.0)).abs() < 1e-3);
        assert_eq!(t.params()[NUM_HW_SOURCES + 1].gain(source_gain_range(NUM_HW_SOURCES + 1), 0), 1.0);
    }

    #[test]
    fn pairs_share_settings_and_initialize_keeps_the_pairing() {
        let t = SourceTable::default();
        let p = t.params();
        p[2].gain_db.store(12.0);
        p[2].invert.store(true, Relaxed);
        t.set_mode(3, source_mode::STEREO);
        assert_eq!(p[2].mode.load(Relaxed), source_mode::STEREO);
        assert_eq!(p[3].mode.load(Relaxed), source_mode::STEREO);
        assert_eq!(p[3].gain_db.load(), 12.0);
        assert!(p[3].invert.load(Relaxed));
        t.set_name(2, "OH");
        t.initialize(3);
        assert_eq!(p[2].gain_db.load(), 0.0);
        assert!(!p[2].invert.load(Relaxed));
        assert_eq!(t.name(2), "");
        assert_eq!(p[3].mode.load(Relaxed), source_mode::STEREO);
        t.set_mode(2, source_mode::MONO);
        assert!(p[2].is_default() && p[3].is_default());
    }
}
