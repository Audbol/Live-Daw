//! Routing beyond each strip's own patch (WING Operation Manual 2.6, 5.4 and
//! 6.6):
//!
//! * **Strip sources** (manual 2.6 "Bus / Main / Matrix": "All Buses, Mains
//!   and Matrices can be selected as the Source for all Input and Aux
//!   channels"): input `STRIP_SOURCE_BASE + n` is signal `n` of the WING's
//!   `$BUS` (n 0-31: bus slot * 2 + side), `$MAIN` (32-39) and `$MTX`
//!   (40-55) groups. A channel reads them through a [`ReturnRing`] one
//!   block late: they come from later in the chain.
//! * **The output patch** (manual 5.4 OUTPUTS: "route any Source to any
//!   digital or analog output"; manual 2: sources "can also be sent directly
//!   from any input to any output"): [`OutputPatch`] gives each hardware
//!   output a source (an input index as channels take it), next to the
//!   strips' own outputs (`StripParams::output_left`...).
//! * **Matrix DIRECT IN** (manual 6.4: "an additional signal (AES/EBU,
//!   Phones, Speakers or Solo bus)"): [`MATRIX_DIRECT_INPUTS`]. The monitor
//!   paths run after the matrices, so a matrix reads them one block late.
//! * **Bus to bus** (manual 5.10 "BUS → BUS: sends from Bus 1-16 to BUS
//!   1-16"; 6.6 "Bus to Bus send: 8 samples"): any bus feeds any other bus.
//!   [`BusSchedule`] orders the buses by their sends so a send arrives in
//!   the same block; a send that closes a loop reads its source's previous
//!   block instead (the engine never waits on itself).
//!
//! Every late read goes through a [`ReturnRing`]: a constant delay of one
//! device block (`L` = the block size), whatever the chunk lengths, and
//! never the half a running task is writing.

use crate::params::{StripKind, NUM_MAINS, NUM_MATRICES, WING_BUSES};
use std::cell::UnsafeCell;
use std::sync::atomic::{AtomicI32, Ordering::Relaxed};

// ---- strip sources ----------------------------------------------------------------

/// Input index of the first strip source (`$BUS` 1 = Bus 1 L).
pub const STRIP_SOURCE_BASE: i32 = 0x40000;
/// The WING's `$BUS` (16 stereo buses), `$MAIN` (4) and `$MTX` (8) signals.
pub const NUM_BUS_SOURCES: usize = 2 * WING_BUSES;
pub const NUM_MAIN_SOURCES: usize = 2 * NUM_MAINS;
pub const NUM_MTX_SOURCES: usize = 2 * NUM_MATRICES;
pub const NUM_STRIP_SOURCES: usize = NUM_BUS_SOURCES + NUM_MAIN_SOURCES + NUM_MTX_SOURCES;

/// The strip signal of input `input`: (kind, 0-based number (a bus's slot),
/// side 0 = L / 1 = R).
pub fn strip_source(input: i32) -> Option<(StripKind, usize, usize)> {
    let n = usize::try_from(input.checked_sub(STRIP_SOURCE_BASE)?).ok()?;
    let (kind, n) = if n < NUM_BUS_SOURCES {
        (StripKind::Bus, n)
    } else if n < NUM_BUS_SOURCES + NUM_MAIN_SOURCES {
        (StripKind::Main, n - NUM_BUS_SOURCES)
    } else if n < NUM_STRIP_SOURCES {
        (StripKind::Matrix, n - NUM_BUS_SOURCES - NUM_MAIN_SOURCES)
    } else {
        return None;
    };
    Some((kind, n / 2, n % 2))
}

/// The input index of a strip signal (inverse of [`strip_source`]); -1 when
/// it has none (input and aux channels, buses past the WING's 16).
pub fn strip_source_input(kind: StripKind, number: usize, side: usize) -> i32 {
    let (base, count) = match kind {
        StripKind::Bus => (0, WING_BUSES),
        StripKind::Main => (NUM_BUS_SOURCES, NUM_MAINS),
        StripKind::Matrix => (NUM_BUS_SOURCES + NUM_MAIN_SOURCES, NUM_MATRICES),
        _ => return -1,
    };
    if number >= count || side > 1 {
        return -1;
    }
    STRIP_SOURCE_BASE + (base + 2 * number + side) as i32
}

/// The position of a strip source among the strip sources (0..`NUM_STRIP_SOURCES`).
pub(crate) fn strip_source_slot(input: i32) -> Option<usize> {
    let n = usize::try_from(input.checked_sub(STRIP_SOURCE_BASE)?).ok()?;
    (n < NUM_STRIP_SOURCES).then_some(n)
}

// ---- the output patch ---------------------------------------------------------------

/// Hardware outputs the patch can feed (as many as there are input ports
/// with a source).
pub const NUM_PATCH_OUTPUTS: usize = crate::sources::NUM_HW_SOURCES;

/// ROUTING > OUTPUTS for sources: what each hardware output plays besides
/// the strips routed to it. An input index as channels take it (a hardware
/// port, a playback channel, the USB PLAYER, an oscillator or a USER
/// SIGNAL / USER PATCH), -1 for none. Shared with the UI
/// (`ld_engine_output_patch`).
#[repr(C)]
#[derive(Debug)]
pub struct OutputPatch {
    pub ports: [AtomicI32; NUM_PATCH_OUTPUTS],
}

impl Default for OutputPatch {
    fn default() -> Self {
        Self { ports: std::array::from_fn(|_| AtomicI32::new(-1)) }
    }
}

impl OutputPatch {
    /// The source on output `port` (-1 none).
    pub fn get(&self, port: i32) -> i32 {
        usize::try_from(port).ok().and_then(|p| self.ports.get(p)).map_or(-1, |s| s.load(Relaxed))
    }

    pub fn set(&self, port: i32, source: i32) {
        if let Some(s) = usize::try_from(port).ok().and_then(|p| self.ports.get(p)) {
            s.store(source.max(-1), Relaxed);
        }
    }

    pub fn reset(&self) {
        self.ports.iter().for_each(|p| p.store(-1, Relaxed));
    }

    /// The patched outputs: (port, source).
    pub fn patched(&self) -> impl Iterator<Item = (usize, i32)> + '_ {
        self.ports.iter().enumerate().map(|(i, p)| (i, p.load(Relaxed))).filter(|(_, s)| *s >= 0)
    }
}

// ---- matrix DIRECT IN ---------------------------------------------------------------

/// A matrix's DIRECT IN (`/mtx/n/dir/in`, manual 6.4): OFF, the AES/EBU
/// input (not on Sidecar: silent), the PHONES and SPEAKERS monitor paths
/// and the solo bus. `StripParams::direct_source` is an index into it.
pub const MATRIX_DIRECT_INPUTS: [&str; 5] = ["OFF", "AES", "MON.PH", "MON.SPK", "MON.BUS"];

pub mod matrix_direct {
    pub const OFF: i32 = 0;
    pub const AES: i32 = 1;
    pub const PHONES: i32 = 2;
    pub const SPEAKERS: i32 = 3;
    pub const SOLO_BUS: i32 = 4;
}

/// The monitor return channel pair of a matrix DIRECT IN (0 PHONES, 1
/// SPEAKERS, 2 the solo bus), None for OFF and AES.
pub(crate) fn matrix_direct_pair(choice: i32) -> Option<usize> {
    match choice {
        matrix_direct::PHONES => Some(0),
        matrix_direct::SPEAKERS => Some(1),
        matrix_direct::SOLO_BUS => Some(2),
        _ => None,
    }
}

// ---- bus to bus ---------------------------------------------------------------------

/// Bus `from` (slot) can send to bus `to`: any other bus (manual 5.10, BUS →
/// BUS "from Bus 1-16 to BUS 1-16"; a bus never feeds itself).
pub fn bus_feeds_bus(from: usize, to: usize) -> bool {
    from != to
}

/// The order of the bus stage for one block. Each bus runs in a *wave* after
/// the buses whose sends feed it, so those sends arrive in the same block; a
/// loop (A to B and B to A, or longer) is broken at the lowest-numbered bus
/// left, and the sends that close it read their source's previous block
/// through its [`ReturnRing`]. Without bus-to-bus sends the stage is one
/// wave. Audio-thread scratch, sized when the topology is published; the
/// message thread uses the same rules for delay compensation.
pub(crate) struct BusSchedule {
    n: usize,
    words: usize,
    /// Row `b`: bit `a` set when bus `a` feeds bus `b` (indices into the bus list).
    feeds: Vec<u64>,
    placed: Vec<u64>,
    waves: Vec<u16>,
    order: Vec<u32>,
    /// Wave `w` is `order[starts[w]..starts[w + 1]]`.
    starts: Vec<usize>,
    num_waves: usize,
}

impl Default for BusSchedule {
    fn default() -> Self {
        Self::new(0)
    }
}

impl BusSchedule {
    pub(crate) fn new(n: usize) -> Self {
        let words = n.div_ceil(64).max(1);
        Self {
            n,
            words,
            feeds: vec![0; n * words],
            placed: vec![0; words],
            waves: vec![0; n],
            order: Vec::with_capacity(n),
            starts: vec![0; n + 2],
            num_waves: 1,
        }
    }

    fn bit(words: &[u64], i: usize) -> bool {
        words[i / 64] & (1 << (i % 64)) != 0
    }

    /// Works out the waves; `feeds(a, b)` says bus `a` sends to bus `b` this
    /// block (indices into the bus list). Never allocates.
    pub(crate) fn compute(&mut self, feeds: impl Fn(usize, usize) -> bool) {
        let (n, words) = (self.n, self.words);
        self.feeds.fill(0);
        let mut any = false;
        for b in 0..n {
            for a in 0..n {
                if a != b && feeds(a, b) {
                    self.feeds[b * words + a / 64] |= 1 << (a % 64);
                    any = true;
                }
            }
        }
        self.order.clear();
        self.waves.fill(0);
        if !any {
            self.order.extend(0..n as u32);
            self.starts[0] = 0;
            self.starts[1] = n;
            self.num_waves = 1;
            return;
        }
        self.placed.fill(0);
        let mut w = 0;
        while self.order.len() < n {
            let start = self.order.len();
            self.starts[w] = start;
            for b in 0..n {
                if Self::bit(&self.placed, b) {
                    continue;
                }
                let row = &self.feeds[b * words..(b + 1) * words];
                if row.iter().zip(&self.placed).all(|(f, p)| f & !p == 0) {
                    self.order.push(b as u32);
                }
            }
            if self.order.len() == start {
                // A loop: the lowest-numbered bus left runs now, its loop sends late.
                let b = (0..n).find(|&b| !Self::bit(&self.placed, b)).unwrap_or(0);
                self.order.push(b as u32);
            }
            for &b in &self.order[start..] {
                self.placed[b as usize / 64] |= 1 << (b % 64);
                self.waves[b as usize] = w as u16;
            }
            w += 1;
        }
        self.starts[w] = n;
        self.num_waves = w;
    }

    pub(crate) fn num_waves(&self) -> usize {
        self.num_waves
    }

    /// The buses of wave `w` (indices into the bus list).
    pub(crate) fn wave(&self, w: usize) -> &[u32] {
        &self.order[self.starts[w]..self.starts[w + 1]]
    }

    /// The send from bus `a` to bus `b` reads `a`'s previous block (it closes a loop).
    pub(crate) fn is_late(&self, a: usize, b: usize) -> bool {
        self.waves[a] >= self.waves[b]
    }
}

// ---- return rings -------------------------------------------------------------------

/// Signals read one block late: a ring of `size` = 2 blocks per channel,
/// every sample stored twice (at `i` and `i + size`) so a block is one
/// contiguous slice. All rings advance together on the engine's
/// [`ReturnClock`]: a block is written at the clock's position and read
/// `size / 2` (one device block) behind it, so the write and read windows
/// never overlap and a reader may run while the writer runs.
pub(crate) struct ReturnRing {
    data: Box<[UnsafeCell<f32>]>,
    channels: usize,
    size: usize,
}

// SAFETY: the writer writes the clock's window, readers read the window one
// block behind; the two are disjoint (see `ReturnClock`).
unsafe impl Sync for ReturnRing {}
unsafe impl Send for ReturnRing {}

impl Default for ReturnRing {
    fn default() -> Self {
        Self::new(0, 1)
    }
}

impl ReturnRing {
    /// Allocates (message thread) `channels` rings for blocks of up to `max_block`.
    pub(crate) fn new(channels: usize, max_block: usize) -> Self {
        let size = 2 * max_block.max(1);
        Self { data: (0..channels * 2 * size).map(|_| UnsafeCell::new(0.0)).collect(), channels, size }
    }

    fn base(&self, ch: usize) -> *mut f32 {
        // `UnsafeCell<f32>` has the layout of `f32`.
        unsafe { (self.data.as_ptr() as *mut f32).add(ch * 2 * self.size) }
    }

    /// Silence on channel `ch` (one writer, no reader of this channel in the block).
    pub(crate) unsafe fn clear(&self, ch: usize) {
        if ch < self.channels {
            std::slice::from_raw_parts_mut(self.base(ch), 2 * self.size).fill(0.0);
        }
    }

    /// Writes a block at the clock's position `pos`.
    pub(crate) unsafe fn write(&self, ch: usize, pos: usize, src: &[f32]) {
        if ch >= self.channels || src.len() > self.size / 2 {
            return;
        }
        let base = self.base(ch);
        let mut w = pos % self.size;
        for &x in src {
            *base.add(w) = x;
            *base.add(w + self.size) = x;
            w += 1;
            if w == self.size {
                w = 0;
            }
        }
    }

    /// The block one device block behind position `pos`, `n` samples long.
    pub(crate) unsafe fn read(&self, ch: usize, pos: usize, n: usize) -> Option<&[f32]> {
        if ch >= self.channels || n > self.size / 2 {
            return None;
        }
        let start = (pos % self.size + self.size / 2) % self.size;
        Some(std::slice::from_raw_parts(self.base(ch).add(start), n))
    }
}

/// The position all return rings write at (audio thread).
#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct ReturnClock {
    pub pos: usize,
    pub size: usize,
}

impl ReturnClock {
    pub(crate) fn new(max_block: usize) -> Self {
        Self { pos: 0, size: 2 * max_block.max(1) }
    }

    /// After a chunk of `n` samples.
    pub(crate) fn advance(&mut self, n: usize) {
        self.pos = (self.pos + n) % self.size.max(1);
    }
}

/// The signals of a bus, main or matrix other strips read late.
pub(crate) mod return_tap {
    /// The output after pan (as a source, a late POST / GROUP send).
    pub const OUT: usize = 0;
    /// The TAP point (a late PRE send).
    pub const TAP: usize = 1;
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn strip_sources_round_trip() {
        assert_eq!(strip_source(STRIP_SOURCE_BASE), Some((StripKind::Bus, 0, 0)));
        assert_eq!(strip_source(STRIP_SOURCE_BASE + 31), Some((StripKind::Bus, 15, 1)));
        assert_eq!(strip_source(STRIP_SOURCE_BASE + 32), Some((StripKind::Main, 0, 0)));
        assert_eq!(strip_source(STRIP_SOURCE_BASE + 55), Some((StripKind::Matrix, 7, 1)));
        assert_eq!(strip_source(STRIP_SOURCE_BASE + 56), None);
        assert_eq!(strip_source(3), None);
        for n in 0..NUM_STRIP_SOURCES as i32 {
            let (k, i, s) = strip_source(STRIP_SOURCE_BASE + n).unwrap();
            assert_eq!(strip_source_input(k, i, s), STRIP_SOURCE_BASE + n);
        }
        assert_eq!(strip_source_input(StripKind::Bus, 16, 0), -1);
        assert_eq!(strip_source_input(StripKind::Channel, 0, 0), -1);
    }

    fn waves(s: &BusSchedule) -> Vec<Vec<u32>> {
        (0..s.num_waves()).map(|w| s.wave(w).to_vec()).collect()
    }

    #[test]
    fn buses_without_bus_sends_are_one_wave() {
        let mut s = BusSchedule::new(16);
        s.compute(|_, _| false);
        assert_eq!(waves(&s), vec![(0..16).collect::<Vec<u32>>()]);
    }

    #[test]
    fn buses_run_after_the_buses_feeding_them() {
        // 9 -> 1, 1 -> 3, 4 -> 3 (0-based 8 -> 0, 0 -> 2, 3 -> 2).
        let edges = [(8, 0), (0, 2), (3, 2)];
        let mut s = BusSchedule::new(16);
        s.compute(|a, b| edges.contains(&(a, b)));
        let w = waves(&s);
        assert_eq!(w.len(), 3);
        assert!(w[0].contains(&8) && w[0].contains(&3) && !w[0].contains(&0));
        assert_eq!(w[1], vec![0]);
        assert_eq!(w[2], vec![2]);
        assert!(edges.iter().all(|&(a, b)| !s.is_late(a, b)));
        assert_eq!(w.iter().map(Vec::len).sum::<usize>(), 16);
    }

    #[test]
    fn a_loop_is_broken_at_its_lowest_bus() {
        // 2 -> 5 -> 7 -> 2, and 7 -> 9 outside the loop.
        let edges = [(2, 5), (5, 7), (7, 2), (7, 9)];
        let mut s = BusSchedule::new(10);
        s.compute(|a, b| edges.contains(&(a, b)));
        assert!(s.is_late(7, 2), "the send closing the loop is late");
        assert!(!s.is_late(2, 5) && !s.is_late(5, 7) && !s.is_late(7, 9));
        let total: usize = (0..s.num_waves()).map(|w| s.wave(w).len()).sum();
        assert_eq!(total, 10);
        // Two buses feeding each other: one of the two sends is late.
        let mut s = BusSchedule::new(2);
        s.compute(|a, b| a != b);
        assert!(s.is_late(1, 0) != s.is_late(0, 1));
    }

    #[test]
    fn rings_read_one_block_behind_whatever_the_chunks() {
        let ring = ReturnRing::new(1, 4);
        let mut clock = ReturnClock::new(4);
        let mut written = Vec::new();
        let mut read = Vec::new();
        let mut t = 0.0;
        for n in [4, 4, 3, 1, 4, 2, 4, 4] {
            let block: Vec<f32> = (0..n)
                .map(|_| {
                    t += 1.0;
                    t
                })
                .collect();
            unsafe {
                read.extend_from_slice(ring.read(0, clock.pos, n).unwrap());
                ring.write(0, clock.pos, &block);
            }
            written.extend(block);
            clock.advance(n);
        }
        // A constant delay of 4 samples (one block), zeros before.
        assert_eq!(&read[..4], &[0.0; 4]);
        assert_eq!(&read[4..], &written[..written.len() - 4]);
    }
}
