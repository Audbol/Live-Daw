//! Plugin delay compensation (PDC). Plugins report a latency (lookahead,
//! linear-phase filters...): their output comes that many samples late. So
//! that parallel paths stay in time, every destination that sums several
//! sources (buses, mains, matrices) reads each source delayed by the
//! difference between the latest source and that one; a summing strip's own
//! signal then carries that aligned latency into the next stage.
//!
//! The plan (who is aligned to what) is worked out on the message thread
//! (`Engine::update_pdc`, from `collect`) and published through atomics: per
//! strip the latency of each signal tap, and for summing strips the latency
//! their sources are aligned to. The audio thread only computes
//! `target - latency` per send.
//!
//! The delay is read from a history ring of the source's tap, so one ring
//! serves every destination at its own offset. Rings are allocated on the
//! message thread, only for the taps that some destination reads delayed, and
//! hold [`MAX_PDC`] samples of history: a compensation above that is capped
//! (and reported).

use crate::garbage::Garbage;
use std::cell::UnsafeCell;
use std::sync::atomic::{AtomicBool, AtomicPtr, AtomicU32, Ordering::*};

/// The longest compensation (samples). Paths needing more stay late by the rest.
pub const MAX_PDC: usize = 8192;

/// The strip signals destinations sum, each with its own latency and ring.
pub mod pdc_tap {
    /// The TAP point (TAP-mode / PRE sends).
    pub const TAP: usize = 0;
    /// Pre-fader after INS 1 (matrix direct inputs, PRE).
    pub const PFL: usize = 1;
    /// Post-fader, after INS 2, before pan (POST bus sends).
    pub const POST: usize = 2;
    /// The output after pan (main, matrix and GROUP sends).
    pub const OUT: usize = 3;
    pub const COUNT: usize = 4;
}

/// A stereo history ring. Every sample is stored twice (at `i` and `i +
/// size`), so any window of up to `size` samples is one contiguous slice.
pub(crate) struct CompRing {
    data: UnsafeCell<[Box<[f32]>; 2]>,
    mask: usize,
    /// Where the next sample goes.
    pos: UnsafeCell<usize>,
}

// SAFETY: one writer task at a time (the strip's owner, or the FX slot's
// claimer) and readers only in later stages; see the thread rules on `Strip`.
unsafe impl Sync for CompRing {}
unsafe impl Send for CompRing {}

impl CompRing {
    /// Allocates (message thread): room for `MAX_PDC` samples of delay with
    /// blocks of up to `max_block`.
    pub(crate) fn new(max_block: usize) -> Self {
        let size = (MAX_PDC + max_block.max(1)).next_power_of_two();
        Self {
            data: UnsafeCell::new([vec![0.0; 2 * size].into_boxed_slice(), vec![0.0; 2 * size].into_boxed_slice()]),
            mask: size - 1,
            pos: UnsafeCell::new(0),
        }
    }

    fn size(&self) -> usize {
        self.mask + 1
    }

    /// Writer only: appends a block (`l` and `r` of equal length <= the block size).
    pub(crate) unsafe fn write(&self, l: &[f32], r: &[f32]) {
        let data = &mut *self.data.get();
        let pos = &mut *self.pos.get();
        let size = self.size();
        for (buf, src) in data.iter_mut().zip([l, r]) {
            let mut w = *pos;
            for &x in src {
                buf[w] = x;
                buf[w + size] = x;
                w = (w + 1) & self.mask;
            }
        }
        *pos = (*pos + l.len()) & self.mask;
    }

    /// The last block of `n` samples written, `delay` samples ago (`delay` <= MAX_PDC).
    pub(crate) unsafe fn read(&self, ch: usize, delay: usize, n: usize) -> &[f32] {
        let data = &*self.data.get();
        let start = (*self.pos.get()).wrapping_sub(n + delay.min(MAX_PDC)) & self.mask;
        &data[ch.min(1)][start..start + n]
    }
}

/// Wrapper so a ring that is no longer needed can go to the garbage collector.
struct RetiredRing(#[allow(dead_code)] Box<CompRing>);

/// A strip's compensation state.
pub struct StripPdc {
    /// Latency (samples) of each tap, `pdc_tap` order, from the plan.
    latency: [AtomicU32; pdc_tap::COUNT],
    /// Summing strips: the latency their sources are aligned to.
    target: AtomicU32,
    rings: [AtomicPtr<CompRing>; pdc_tap::COUNT],
}

impl Default for StripPdc {
    fn default() -> Self {
        Self {
            latency: Default::default(),
            target: AtomicU32::new(0),
            rings: std::array::from_fn(|_| AtomicPtr::new(std::ptr::null_mut())),
        }
    }
}

impl StripPdc {
    /// The latency of `tap` (samples).
    pub fn latency(&self, tap: usize) -> u32 {
        self.latency[tap].load(Relaxed)
    }

    /// Summing strips: the latency every source is aligned to.
    pub fn target(&self) -> u32 {
        self.target.load(Relaxed)
    }

    pub(crate) fn set(&self, latency: [u32; pdc_tap::COUNT], target: u32) {
        for (a, v) in self.latency.iter().zip(latency) {
            a.store(v, Relaxed);
        }
        self.target.store(target, Relaxed);
    }

    /// The tap has a history ring.
    pub fn has_ring(&self, tap: usize) -> bool {
        !self.rings[tap].load(Relaxed).is_null()
    }

    /// Message thread: makes sure `tap` has a ring for blocks of `max_block`.
    pub(crate) fn ensure_ring(&self, tap: usize, max_block: usize) {
        if self.rings[tap].load(Acquire).is_null() {
            let ring = Box::into_raw(Box::new(CompRing::new(max_block)));
            // Release publishes the zeroed ring to the audio thread.
            if self.rings[tap].compare_exchange(std::ptr::null_mut(), ring, AcqRel, Acquire).is_err() {
                // SAFETY: never published; allocated just above.
                drop(unsafe { Box::from_raw(ring) });
            }
        }
    }

    /// Message thread: retires every ring (no compensation is needed any more).
    pub(crate) fn release_rings(&self, garbage: &Garbage) {
        for r in &self.rings {
            let old = r.swap(std::ptr::null_mut(), AcqRel);
            if !old.is_null() {
                // SAFETY: created by Box::into_raw in ensure_ring; now unpublished.
                garbage.retire(RetiredRing(unsafe { Box::from_raw(old) }));
            }
        }
    }

    /// Device stopped: frees the rings (they are sized for the block size;
    /// the next plan allocates them again).
    pub(crate) fn drop_rings(&self) {
        for r in &self.rings {
            let old = r.swap(std::ptr::null_mut(), AcqRel);
            if !old.is_null() {
                // SAFETY: created by Box::into_raw; the audio thread is stopped.
                drop(unsafe { Box::from_raw(old) });
            }
        }
    }

    /// Owning task only: appends this block of `tap` (left, right) to its ring, if any.
    #[inline]
    pub(crate) unsafe fn write(&self, tap: usize, l: &[f32], r: &[f32]) {
        if let Some(ring) = self.rings[tap].load(Acquire).as_ref() {
            ring.write(l, r);
        }
    }

    /// `tap`'s signal as a destination aligned to `target` reads it: None
    /// when it needs no delay (or its ring isn't there yet), else the delayed block.
    #[inline]
    pub(crate) unsafe fn delayed(&self, tap: usize, target: u32, ch: usize, n: usize) -> Option<&[f32]> {
        let delay = target.saturating_sub(self.latency[tap].load(Relaxed)) as usize;
        if delay == 0 {
            return None;
        }
        self.rings[tap].load(Acquire).as_ref().map(|ring| ring.read(ch, delay.min(MAX_PDC), n))
    }
}

impl Drop for StripPdc {
    fn drop(&mut self) {
        self.drop_rings();
    }
}

/// Console-wide compensation settings and status.
pub struct PdcState {
    /// SETUP > PLUGINS > DELAY COMPENSATION (default on).
    pub enabled: AtomicBool,
    /// The longest path latency the plan aligns to (samples).
    pub total: AtomicU32,
    /// A path needed more than `MAX_PDC` samples of compensation.
    pub limited: AtomicBool,
}

impl Default for PdcState {
    fn default() -> Self {
        Self { enabled: AtomicBool::new(true), total: AtomicU32::new(0), limited: AtomicBool::new(false) }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ring_reads_contiguous_delayed_windows() {
        let ring = CompRing::new(64);
        let block: Vec<f32> = (0..64).map(|i| i as f32).collect();
        let size = ring.size();
        // Fill past a wrap-around, then read blocks at several delays.
        let mut written = 0usize;
        while written < size + 100 {
            let b: Vec<f32> = block.iter().map(|x| x + written as f32).collect();
            unsafe { ring.write(&b, &b) };
            written += 64;
        }
        for delay in [1usize, 63, 64, 1000, MAX_PDC] {
            let got = unsafe { ring.read(1, delay, 64) };
            let first = (written - 64 - delay) as f32;
            assert_eq!(got[0], first, "delay {delay}");
            assert_eq!(got[63], first + 63.0, "delay {delay}");
        }
        assert_eq!(unsafe { ring.read(0, 0, 64) }[63], (written - 1) as f32);
    }

    #[test]
    fn strip_reads_only_what_needs_a_delay() {
        let pdc = StripPdc::default();
        pdc.set([0, 0, 100, 100], 0);
        pdc.ensure_ring(pdc_tap::TAP, 32);
        let x = [1.0f32; 32];
        unsafe {
            pdc.write(pdc_tap::TAP, &x, &x);
            assert!(pdc.delayed(pdc_tap::TAP, 0, 0, 32).is_none());
            assert!(pdc.delayed(pdc_tap::POST, 300, 0, 32).is_none()); // no ring
            assert_eq!(pdc.delayed(pdc_tap::TAP, 16, 0, 32).unwrap()[..16], [0.0; 16]);
            assert_eq!(pdc.delayed(pdc_tap::TAP, 16, 0, 32).unwrap()[16..], [1.0; 16]);
        }
        let garbage = Garbage::default();
        pdc.release_rings(&garbage);
        assert!(!pdc.has_ring(pdc_tap::TAP));
        assert_eq!(garbage.num_pending(), 1);
    }
}
