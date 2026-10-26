//! KEY TAP: a GATE or DYNAMICS slot keyed from another input channel takes
//! the key at one of that channel's tap points (`TAP_POINTS`: IN, FILT,
//! 3, 4, 5, PFL, AFL, POST), as a mono signal (a stereo channel's L + R / 2).
//!
//! Aux channels, buses, mains and matrices key their DYNAMICS from the
//! buses, mains, matrices and aux channels instead, as the WING's
//! `/bus/n/dynsc/src` list (SELF, BUS.1-16, MAIN.1-4, MTX.1-8, AUX.1-8),
//! at one of the taps of `/bus/n/dynsc/tap` ([`OUTPUT_KEY_TAPS`]: BUS, DYN,
//! PFL, AFL, EQ, INS2). `sc_source` holds such a source as
//! [`KEY_STRIP_BASE`] × kind + index ([`KeySource`]; an older bus keyed from
//! an input channel keeps the channel's index and taps). A key from a strip
//! of an earlier summing stage ([`stage_of`]) takes its tap of this block;
//! one from the same or a later stage (a bus keyed from a main, an aux from
//! an aux) that of the previous block.
//!
//! The source channel writes each tap some key reads into a buffer of its
//! own while it runs. Buffers are allocated on the message thread, only for
//! the (channel, tap) pairs some key uses (`Engine::update_keys`, from
//! `collect`), and published through an `AtomicPtr` like the PDC rings; a
//! key whose buffer isn't there yet reads the source's input.
//!
//! Processing order: input and aux channels run in parallel in stage 1, so a
//! key from a channel of the same stage may not have run yet. Each block the
//! audio thread sorts stage 1 into *waves* by key dependency
//! ([`KeySchedule`]): a strip runs in a later wave than the channels its keys
//! come from, and the waves run one after the other. A key reads the source's
//! tap of this block when the source ran in an earlier wave (or stage), else
//! the tap of the previous block: cycles (A keys B, B keys A, a channel keyed
//! by its own tap) and chains deeper than [`MAX_KEY_WAVES`] get a key one
//! block late, never a half-written one. Every buffer is double-buffered by
//! block parity, so a source writing this block's half never touches the
//! half a reader in the same wave takes.

use crate::garbage::Garbage;
use crate::params::{StripKind, StripParams};
use std::cell::UnsafeCell;
use std::sync::atomic::{AtomicPtr, Ordering::*};

/// The tap buffers of a strip: the input channel taps (`TAP_POINTS`), then
/// the post-fader EQ of buses, mains and matrices ([`TAP_EQ_POST`]) and
/// INS 2 ([`TAP_INS2`]), which only the keys of `OUTPUT_KEY_TAPS` take.
pub const NUM_KEY_TAPS: usize = 10;
/// Buses, mains and matrices after their post-fader EQ (aux: the EQ is tap "4").
pub const TAP_EQ_POST: usize = 8;
/// After INS 2, before the post-fader delay (aux, without INS 2: after the fader).
pub const TAP_INS2: usize = 9;

/// Aux, bus, main and matrix key sources in `sc_source`: `KEY_STRIP_BASE *
/// kind + index` (kind as `StripKind`, index from 0). Smaller values are
/// input channels.
pub const KEY_STRIP_BASE: i32 = 0x10000;

/// The taps an aux, bus, main or matrix key takes from its source (WING
/// `dynsc/tap`, in list order).
pub const OUTPUT_KEY_TAPS: [&str; 6] = ["BUS", "DYN", "PFL", "AFL", "EQ", "INS2"];

/// What a slot's `sc_source` names.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum KeySource {
    /// The slot keys itself.
    SelfKey,
    /// Input channel `n` (from 0).
    Channel(usize),
    /// An aux, bus, main or matrix (index from 0).
    Strip(StripKind, usize),
}

impl KeySource {
    pub fn from_code(code: i32) -> Self {
        if code < 0 {
            return Self::SelfKey;
        }
        if code < KEY_STRIP_BASE {
            return Self::Channel(code as usize);
        }
        match StripKind::from_i32(code / KEY_STRIP_BASE) {
            Some(kind) if kind != StripKind::Channel => Self::Strip(kind, (code % KEY_STRIP_BASE) as usize),
            _ => Self::SelfKey,
        }
    }

    pub fn code(self) -> i32 {
        match self {
            Self::SelfKey => -1,
            Self::Channel(n) => n as i32,
            Self::Strip(kind, i) => kind as i32 * KEY_STRIP_BASE + i as i32,
        }
    }

    /// The WING's name ("SELF", "CH.3", "BUS.12", "MAIN.1", "MTX.2", "AUX.4").
    pub fn wing_name(self) -> String {
        match self {
            Self::SelfKey => "SELF".into(),
            Self::Channel(n) => format!("CH.{}", n + 1),
            Self::Strip(kind, i) => format!("{}.{}", strip_prefix(kind), i + 1),
        }
    }

    /// Parses a WING name (see `wing_name`).
    pub fn from_wing_name(text: &str) -> Option<Self> {
        if text == "SELF" {
            return Some(Self::SelfKey);
        }
        let (prefix, n) = text.split_once('.')?;
        let i = n.parse::<usize>().ok()?.checked_sub(1)?;
        if prefix == "CH" {
            return Some(Self::Channel(i));
        }
        let kind = [StripKind::Aux, StripKind::Bus, StripKind::Main, StripKind::Matrix]
            .into_iter()
            .find(|&k| strip_prefix(k) == prefix)?;
        Some(Self::Strip(kind, i))
    }
}

fn strip_prefix(kind: StripKind) -> &'static str {
    match kind {
        StripKind::Channel => "CH",
        StripKind::Aux => "AUX",
        StripKind::Bus => "BUS",
        StripKind::Main => "MAIN",
        StripKind::Matrix => "MTX",
    }
}

/// The tap buffer a key takes: an input channel's tap point (`sc_tap` as
/// `TAP_POINTS`) or, from an aux, bus, main or matrix, the buffer of
/// `OUTPUT_KEY_TAPS[sc_tap]` on a strip of that kind.
pub fn key_buffer(source: KeySource, sc_tap: i32) -> usize {
    match source {
        KeySource::Strip(kind, _) => match sc_tap.clamp(0, OUTPUT_KEY_TAPS.len() as i32 - 1) {
            0 => 0,                           // BUS: the strip's input (IN)
            1 => 2,                           // DYN: after the first block (DYNAMICS on all of them)
            2 => 5,                           // PFL
            3 => 6,                           // AFL
            4 if kind == StripKind::Aux => 3, // EQ: aux run DYNAMICS, EQ, INS 1
            4 => TAP_EQ_POST,
            _ => TAP_INS2,
        },
        _ => sc_tap.clamp(0, 7) as usize,
    }
}

/// The stage a strip runs in (`engine.rs`: 1 input and aux channels, 3
/// buses 9+, 4 buses 1-8, 5 mains, 6 matrices). A key from a strip of an
/// earlier stage is of this block.
pub(crate) fn stage_of(kind: StripKind, id: i32) -> u8 {
    match kind {
        StripKind::Channel | StripKind::Aux => 1,
        StripKind::Bus if id.max(0) as usize >= crate::params::BUS_TO_BUS_TARGETS => 3,
        StripKind::Bus => 4,
        StripKind::Main => 5,
        StripKind::Matrix => 6,
    }
}

/// The most stage-1 waves per block (each is one fork/join of the pool).
pub const MAX_KEY_WAVES: u8 = 4;

/// One half of a tap buffer: a block and how long it was.
struct Half {
    data: Box<[f32]>,
    len: usize,
}

/// A mono tap buffer, one half per block parity.
pub(crate) struct KeyTapBuf {
    halves: [UnsafeCell<Half>; 2],
}

// SAFETY: in a block, only the source's task writes the half of the block's
// parity; readers in the same stage take the other half, readers in later
// waves or stages this one after the fork/join (see the module notes).
unsafe impl Sync for KeyTapBuf {}
unsafe impl Send for KeyTapBuf {}

impl KeyTapBuf {
    fn new(max_block: usize) -> Self {
        let half = || UnsafeCell::new(Half { data: vec![0.0; max_block.max(1)].into_boxed_slice(), len: 0 });
        Self { halves: [half(), half()] }
    }
}

/// Wrapper so a buffer no longer needed can go to the garbage collector.
struct RetiredTap(#[allow(dead_code)] Box<KeyTapBuf>);

/// A channel's key tap buffers (the taps some key reads).
pub struct StripKeyTaps {
    bufs: [AtomicPtr<KeyTapBuf>; NUM_KEY_TAPS],
}

impl Default for StripKeyTaps {
    fn default() -> Self {
        Self { bufs: std::array::from_fn(|_| AtomicPtr::new(std::ptr::null_mut())) }
    }
}

impl StripKeyTaps {
    /// `tap` has a buffer.
    pub fn has(&self, tap: usize) -> bool {
        self.bufs.get(tap).is_some_and(|b| !b.load(Relaxed).is_null())
    }

    /// Message thread: keeps buffers (for blocks of `max_block`) for the taps
    /// in `mask` and retires the others.
    pub(crate) fn plan(&self, mask: u32, max_block: usize, garbage: &Garbage) {
        for (t, slot) in self.bufs.iter().enumerate() {
            if mask & (1 << t) != 0 {
                if slot.load(Acquire).is_null() {
                    let buf = Box::into_raw(Box::new(KeyTapBuf::new(max_block)));
                    // Release publishes the zeroed buffer to the audio thread.
                    if slot.compare_exchange(std::ptr::null_mut(), buf, AcqRel, Acquire).is_err() {
                        // SAFETY: never published; allocated just above.
                        drop(unsafe { Box::from_raw(buf) });
                    }
                }
            } else {
                let old = slot.swap(std::ptr::null_mut(), AcqRel);
                if !old.is_null() {
                    // SAFETY: created by Box::into_raw above; now unpublished.
                    garbage.retire(RetiredTap(unsafe { Box::from_raw(old) }));
                }
            }
        }
    }

    /// Device stopped: frees the buffers (sized for the block size; the next
    /// plan allocates them again).
    pub(crate) fn drop_all(&self) {
        for slot in &self.bufs {
            let old = slot.swap(std::ptr::null_mut(), AcqRel);
            if !old.is_null() {
                // SAFETY: created by Box::into_raw; the audio thread is stopped.
                drop(unsafe { Box::from_raw(old) });
            }
        }
    }

    /// Owning task only: stores this block of `tap` (one or two channels,
    /// mixed to mono, times `gain`), if a key reads it.
    #[inline]
    pub(crate) unsafe fn write(&self, tap: usize, block: u64, chans: [&[f32]; 2], nc: usize, gain: f32) {
        let Some(buf) = self.bufs.get(tap).and_then(|b| b.load(Acquire).as_ref()) else { return };
        let half = &mut *buf.halves[(block & 1) as usize].get();
        let n = chans[0].len().min(half.data.len());
        let dst = &mut half.data[..n];
        if nc > 1 {
            let g = gain * 0.5;
            for ((d, l), r) in dst.iter_mut().zip(chans[0]).zip(chans[1]) {
                *d = (l + r) * g;
            }
        } else {
            for (d, x) in dst.iter_mut().zip(chans[0]) {
                *d = x * gain;
            }
        }
        half.len = n;
    }

    /// Owning task only: silence on every tap (an idle or faulted channel).
    pub(crate) unsafe fn clear(&self, block: u64, n: usize) {
        for b in &self.bufs {
            if let Some(buf) = b.load(Acquire).as_ref() {
                let half = &mut *buf.halves[(block & 1) as usize].get();
                let n = n.min(half.data.len());
                half.data[..n].fill(0.0);
                half.len = n;
            }
        }
    }

    /// Reads `tap` into `dst`: this block's signal with `current`, else the
    /// previous block's (a shorter block is padded with silence). False when
    /// the tap has no buffer.
    pub(crate) unsafe fn read(&self, tap: usize, block: u64, current: bool, dst: &mut [f32]) -> bool {
        let Some(buf) = self.bufs.get(tap).and_then(|b| b.load(Acquire).as_ref()) else { return false };
        let parity = if current { block & 1 } else { block.wrapping_sub(1) & 1 };
        let half = &*buf.halves[parity as usize].get();
        let n = dst.len().min(half.len);
        dst[..n].copy_from_slice(&half.data[..n]);
        dst[n..].fill(0.0);
        true
    }
}

impl Drop for StripKeyTaps {
    fn drop(&mut self) {
        self.drop_all();
    }
}

/// The input channels a strip's GATE and DYNAMICS keys come from (-1 =
/// none, itself or no input channel): the gate runs on input channels only.
pub(crate) fn key_sources(p: &StripParams, has_gate: bool) -> [i32; 2] {
    let src = |s: &crate::params::DynSlotParams| match KeySource::from_code(s.sc_source.load(Relaxed)) {
        KeySource::Channel(n) => n as i32,
        _ => -1,
    };
    [if has_gate { src(&p.gate) } else { -1 }, src(&p.dynamics)]
}

/// A slot's key source and the tap buffer its key takes.
pub(crate) fn key_tap(s: &crate::params::DynSlotParams) -> (KeySource, usize) {
    let source = KeySource::from_code(s.sc_source.load(Relaxed));
    (source, key_buffer(source, s.sc_tap.load(Relaxed)))
}

/// Stage 1's order for one block: the wave of each strip (input channels,
/// then aux) and the strips sorted by wave. Audio-thread scratch, sized when
/// the topology is published.
pub(crate) struct KeySchedule {
    waves: Vec<u8>,
    order: Vec<u32>,
    /// Where each wave starts in `order` (wave `w`: `starts[w]..starts[w + 1]`).
    starts: [usize; MAX_KEY_WAVES as usize + 1],
    num_waves: usize,
}

impl Default for KeySchedule {
    fn default() -> Self {
        Self::new(0)
    }
}

impl KeySchedule {
    pub(crate) fn new(num_strips: usize) -> Self {
        Self {
            waves: vec![0; num_strips],
            order: Vec::with_capacity(num_strips),
            starts: [0; MAX_KEY_WAVES as usize + 1],
            num_waves: 1,
        }
    }

    /// Works out the waves. `sources(i)` gives strip `i`'s key sources (input
    /// channel indices; channels are strips `0..num_channels`). A strip runs
    /// one wave after its latest source, up to the last wave, where cycles and
    /// deep chains end up (they read the previous block). Never allocates.
    pub(crate) fn compute(&mut self, num_channels: usize, sources: impl Fn(usize) -> [i32; 2]) {
        let n = self.waves.len();
        self.waves.fill(0);
        let last = MAX_KEY_WAVES - 1;
        let mut any = false;
        // Longest path by relaxation; a cycle climbs to the last wave and stops there.
        for _ in 0..MAX_KEY_WAVES {
            let mut changed = false;
            for i in 0..n {
                for src in sources(i) {
                    let Ok(s) = usize::try_from(src) else { continue };
                    if s == i || s >= num_channels.min(n) {
                        continue;
                    }
                    let w = (self.waves[s] + 1).min(last);
                    if w > self.waves[i] {
                        self.waves[i] = w;
                        changed = true;
                    }
                }
            }
            any |= changed;
            if !changed {
                break;
            }
        }
        self.order.clear();
        if !any {
            self.num_waves = 1;
            self.starts[1] = n;
            return;
        }
        // Counting sort by wave (stable, so the order inside a wave stays the console's).
        let mut count = [0usize; MAX_KEY_WAVES as usize];
        for &w in &self.waves {
            count[w as usize] += 1;
        }
        self.starts[0] = 0;
        for (w, c) in count.iter().enumerate() {
            self.starts[w + 1] = self.starts[w] + c;
        }
        self.num_waves = count.iter().rposition(|&c| c > 0).map_or(1, |w| w + 1);
        for w in 0..self.num_waves as u8 {
            for (i, &sw) in self.waves.iter().enumerate() {
                if sw == w && self.order.len() < self.order.capacity() {
                    self.order.push(i as u32);
                }
            }
        }
    }

    pub(crate) fn num_waves(&self) -> usize {
        self.num_waves
    }

    /// The strips of wave `w` (stage-1 indices). With a single wave: all, in order.
    pub(crate) fn wave(&self, w: usize) -> WaveStrips<'_> {
        if self.order.is_empty() {
            WaveStrips::All(self.waves.len())
        } else {
            WaveStrips::Some(&self.order[self.starts[w]..self.starts[w + 1]])
        }
    }

    /// The wave of each stage-1 strip.
    pub(crate) fn waves(&self) -> &[u8] {
        &self.waves
    }
}

/// The strips of one wave.
pub(crate) enum WaveStrips<'a> {
    All(usize),
    Some(&'a [u32]),
}

impl WaveStrips<'_> {
    pub(crate) fn len(&self) -> usize {
        match self {
            Self::All(n) => *n,
            Self::Some(s) => s.len(),
        }
    }

    pub(crate) fn get(&self, j: usize) -> usize {
        match self {
            Self::All(_) => j,
            Self::Some(s) => s[j] as usize,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn schedule(num_channels: usize, num_strips: usize, deps: &[(usize, i32)]) -> KeySchedule {
        let mut s = KeySchedule::new(num_strips);
        s.compute(num_channels, |i| {
            let mut out = [-1; 2];
            for (k, &(_, src)) in deps.iter().filter(|(d, _)| *d == i).take(2).enumerate() {
                out[k] = src;
            }
            out
        });
        s
    }

    #[test]
    fn key_sources_round_trip_through_codes_and_wing_names() {
        use StripKind::*;
        for src in [
            KeySource::SelfKey,
            KeySource::Channel(39),
            KeySource::Strip(Bus, 15),
            KeySource::Strip(Main, 0),
            KeySource::Strip(Matrix, 7),
            KeySource::Strip(Aux, 3),
        ] {
            assert_eq!(KeySource::from_code(src.code()), src);
            assert_eq!(KeySource::from_wing_name(&src.wing_name()), Some(src));
        }
        assert_eq!(KeySource::Strip(Bus, 11).wing_name(), "BUS.12");
        assert_eq!(KeySource::from_wing_name("MTX.2"), Some(KeySource::Strip(Matrix, 1)));
        assert_eq!(KeySource::from_wing_name("AUX.0"), None);
        assert_eq!(KeySource::from_code(5), KeySource::Channel(5), "an older bus key from a channel");
        assert_eq!(KeySource::from_code(KEY_STRIP_BASE * 9), KeySource::SelfKey);
        // BUS, DYN, PFL, AFL, EQ, INS2; an aux's EQ is its second block.
        let taps = |kind| (0..6).map(|t| key_buffer(KeySource::Strip(kind, 0), t)).collect::<Vec<_>>();
        assert_eq!(taps(Bus), [0, 2, 5, 6, TAP_EQ_POST, TAP_INS2]);
        assert_eq!(taps(Aux), [0, 2, 5, 6, 3, TAP_INS2]);
        assert_eq!(key_buffer(KeySource::Channel(0), 7), 7);
        assert!(
            stage_of(Aux, 0) < stage_of(Bus, 8) && stage_of(Bus, 8) < stage_of(Bus, 7) && stage_of(Bus, 7) < stage_of(Main, 0)
        );
        assert!(stage_of(Main, 3) < stage_of(Matrix, 0));
    }

    #[test]
    fn no_keys_is_one_wave_in_console_order() {
        let s = schedule(4, 6, &[(2, -1), (3, 3)]); // none, and a self key
        assert_eq!(s.num_waves(), 1);
        assert_eq!(s.wave(0).len(), 6);
        assert_eq!(s.wave(0).get(5), 5);
    }

    #[test]
    fn keyed_strips_run_after_their_sources() {
        // 0 keys from 2, 2 from 3; aux 4 (strip 4) keys from 0.
        let s = schedule(4, 6, &[(0, 2), (2, 3), (4, 0)]);
        assert_eq!(s.waves(), &[2, 0, 1, 0, 3, 0]);
        assert_eq!(s.num_waves(), 4);
        let w0: Vec<usize> = (0..s.wave(0).len()).map(|j| s.wave(0).get(j)).collect();
        assert_eq!(w0, [1, 3, 5]);
        assert_eq!(s.wave(3).get(0), 4);
    }

    #[test]
    fn cycles_and_bad_indices_terminate_in_the_last_wave() {
        let s = schedule(3, 3, &[(0, 1), (1, 0), (2, 99), (2, -5)]);
        let last = MAX_KEY_WAVES - 1;
        assert_eq!(s.waves(), &[last, last, 0]);
        // Every strip is in exactly one wave.
        let total: usize = (0..s.num_waves()).map(|w| s.wave(w).len()).sum();
        assert_eq!(total, 3);
        // An aux strip can't be a key source.
        let s = schedule(1, 2, &[(0, 1)]);
        assert_eq!(s.num_waves(), 1);
    }

    #[test]
    fn taps_are_double_buffered_by_block() {
        let taps = StripKeyTaps::default();
        let garbage = Garbage::default();
        taps.plan(1 << 7, 4, &garbage);
        assert!(taps.has(7) && !taps.has(0));
        let mut dst = [9.0f32; 4];
        unsafe {
            assert!(!taps.read(0, 1, true, &mut dst), "no buffer");
            taps.write(7, 1, [&[1.0; 4], &[3.0; 4]], 2, 1.0);
            taps.write(7, 2, [&[2.5; 2], &[0.0; 2]], 1, 2.0);
            assert!(taps.read(7, 2, true, &mut dst));
            assert_eq!(dst, [5.0, 5.0, 0.0, 0.0], "a shorter block is padded");
            taps.read(7, 2, false, &mut dst);
            assert_eq!(dst, [2.0; 4], "stereo is mixed to mono");
            taps.clear(3, 4);
            taps.read(7, 3, true, &mut dst);
            assert_eq!(dst, [0.0; 4]);
        }
        taps.plan(0, 4, &garbage);
        assert!(!taps.has(7));
        assert_eq!(garbage.num_pending(), 1);
    }
}
