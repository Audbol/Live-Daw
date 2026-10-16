//! Atomic helpers. All parameter and meter atomics use relaxed ordering: each
//! value is independent and only needs to be torn-free.

use std::sync::atomic::{AtomicU32, Ordering::Relaxed};

/// An `f32` stored atomically. `#[repr(transparent)]` over `AtomicU32`, so it has
/// the layout of a 4-byte float (the C++ side mirrors it as `std::atomic<uint32_t>`).
#[repr(transparent)]
#[derive(Debug)]
pub struct AtomicF32(AtomicU32);

impl AtomicF32 {
    pub const fn new(v: f32) -> Self {
        Self(AtomicU32::new(v.to_bits()))
    }

    #[inline]
    pub fn load(&self) -> f32 {
        f32::from_bits(self.0.load(Relaxed))
    }

    #[inline]
    pub fn store(&self, v: f32) {
        self.0.store(v.to_bits(), Relaxed);
    }

    #[inline]
    pub fn exchange(&self, v: f32) -> f32 {
        f32::from_bits(self.0.swap(v.to_bits(), Relaxed))
    }

    /// "Max since last read" update used for peak meters.
    #[inline]
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

impl Default for AtomicF32 {
    fn default() -> Self {
        Self::new(0.0)
    }
}
