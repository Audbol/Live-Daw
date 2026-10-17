//! Per-strip send storage that grows in pages as buses are created.

use crate::atomic::AtomicF32;
use std::cell::UnsafeCell;
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicPtr, Ordering};

/// Upper bound on bus slots. Pages are allocated on demand, so the ceiling costs
/// only the page-pointer table (2 KB per strip); in practice CPU is the limit.
pub const MAX_BUSES: usize = 16384;
pub const PAGE_SIZE: usize = 64;
const NUM_PAGES: usize = MAX_BUSES.div_ceil(PAGE_SIZE);

/// Bus send modes: TAP (the WING's name for input channels; pre-fader for
/// buses), post-fader, or GROUP (post-fader and post-pan, like routing the
/// strip to a group: no send level).
pub const SEND_MODES: [&str; 3] = ["PRE", "POST", "GRP"];

/// Send modes (indices into [`SEND_MODES`]).
pub mod send_mode {
    pub const PRE: i32 = 0;
    pub const POST: i32 = 1;
    pub const GROUP: i32 = 2;
}

#[repr(C)]
#[derive(Debug)]
pub struct SendParams {
    pub enabled: AtomicBool,
    /// Pan follows the strip's pan (else `pan` is used).
    pub pan_link: AtomicBool,
    /// IGNORE CHANNEL MUTE (input and aux channels, PRE/TAP mode only): the send
    /// stays on while the channel is muted.
    pub ignore_mute: AtomicBool,
    /// With `pan_link`: follow the strip's pan mirrored (LINK REVERSED).
    pub pan_reversed: AtomicBool,
    /// Index into [`SEND_MODES`].
    pub mode: AtomicI32,
    pub level_db: AtomicF32,
    pub pan: AtomicF32,
    /// Gain ramp state (left, right), owned by the audio task that sums the destination bus.
    pub(crate) last_gain: UnsafeCell<[f32; 2]>,
}

// SAFETY: `last_gain` is only touched by the single audio task that sums the bus
// this send feeds; everything else is atomic.
unsafe impl Sync for SendParams {}

impl Default for SendParams {
    fn default() -> Self {
        Self {
            enabled: AtomicBool::new(false),
            pan_link: AtomicBool::new(true),
            ignore_mute: AtomicBool::new(false),
            pan_reversed: AtomicBool::new(false),
            mode: AtomicI32::new(send_mode::PRE),
            level_db: AtomicF32::new(0.0),
            pan: AtomicF32::new(0.0),
            last_gain: UnsafeCell::new([0.0; 2]),
        }
    }
}

struct Page {
    sends: [SendParams; PAGE_SIZE],
}

/// Pages are only added (by the message thread) and freed with the strip, so the
/// audio thread reads them lock-free; a slot whose page doesn't exist has no send.
pub struct SendBank {
    pages: Box<[AtomicPtr<Page>]>,
}

impl Default for SendBank {
    fn default() -> Self {
        Self { pages: (0..NUM_PAGES).map(|_| AtomicPtr::new(std::ptr::null_mut())).collect() }
    }
}

impl SendBank {
    /// Any thread: the send for `slot`, or None if it was never configured.
    #[inline]
    pub fn find(&self, slot: usize) -> Option<&SendParams> {
        if slot >= MAX_BUSES {
            return None;
        }
        let page = self.pages[slot / PAGE_SIZE].load(Ordering::Acquire);
        // SAFETY: pages are never freed while the bank is alive.
        unsafe { page.as_ref() }.map(|p| &p.sends[slot % PAGE_SIZE])
    }

    /// Any thread but the audio thread (it allocates): the send for `slot`,
    /// allocating its page if needed. Two threads creating the same page race
    /// through a compare-exchange; the loser frees its copy.
    pub fn get_or_create(&self, slot: usize) -> Option<&SendParams> {
        if slot >= MAX_BUSES {
            return None;
        }
        let ptr = &self.pages[slot / PAGE_SIZE];
        if ptr.load(Ordering::Acquire).is_null() {
            let page = Box::into_raw(Box::new(Page { sends: std::array::from_fn(|_| SendParams::default()) }));
            // Release publishes the initialised page to the audio thread.
            if ptr.compare_exchange(std::ptr::null_mut(), page, Ordering::AcqRel, Ordering::Acquire).is_err() {
                // SAFETY: never published; allocated just above.
                drop(unsafe { Box::from_raw(page) });
            }
        }
        self.find(slot)
    }
}

impl Drop for SendBank {
    fn drop(&mut self) {
        for p in self.pages.iter() {
            let page = p.swap(std::ptr::null_mut(), Ordering::AcqRel);
            if !page.is_null() {
                // SAFETY: allocated by Box::into_raw in get_or_create.
                drop(unsafe { Box::from_raw(page) });
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::Ordering::Relaxed;

    #[test]
    fn pages_allocate_on_demand() {
        let bank = SendBank::default();
        assert!(bank.find(5000).is_none());
        bank.get_or_create(5000).unwrap().enabled.store(true, Relaxed);
        assert!(bank.find(5000).unwrap().enabled.load(Relaxed));
        assert!(bank.find(4999).is_some()); // same page
        assert!(bank.find(5100).is_none());
        assert!(bank.get_or_create(MAX_BUSES).is_none());
    }
}
