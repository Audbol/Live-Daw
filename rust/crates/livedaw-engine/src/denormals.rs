//! Flush-to-zero / denormals-are-zero for the current thread, so filter tails
//! decaying towards zero never hit the slow denormal path.

pub struct ScopedNoDenormals {
    #[allow(dead_code)]
    previous: u64,
}

impl ScopedNoDenormals {
    #[inline]
    #[allow(deprecated, clippy::new_without_default)]
    pub fn new() -> Self {
        #[cfg(target_arch = "x86_64")]
        {
            use std::arch::x86_64::{_mm_getcsr, _mm_setcsr};
            // SAFETY: reading/writing MXCSR only changes this thread's FP mode.
            unsafe {
                let previous = _mm_getcsr();
                _mm_setcsr(previous | 0x8040); // FTZ | DAZ
                return Self { previous: previous as u64 };
            }
        }
        #[cfg(target_arch = "aarch64")]
        {
            // SAFETY: FPCR only affects this thread's floating-point behaviour.
            unsafe {
                let previous: u64;
                std::arch::asm!("mrs {}, fpcr", out(reg) previous);
                std::arch::asm!("msr fpcr, {}", in(reg) previous | (1 << 24)); // FZ
                return Self { previous };
            }
        }
        #[allow(unreachable_code)]
        Self { previous: 0 }
    }
}

impl Drop for ScopedNoDenormals {
    #[inline]
    #[allow(deprecated)]
    fn drop(&mut self) {
        #[cfg(target_arch = "x86_64")]
        // SAFETY: restores the value read in new().
        unsafe {
            std::arch::x86_64::_mm_setcsr(self.previous as u32)
        }
        #[cfg(target_arch = "aarch64")]
        // SAFETY: restores the value read in new().
        unsafe {
            std::arch::asm!("msr fpcr, {}", in(reg) self.previous)
        }
    }
}
