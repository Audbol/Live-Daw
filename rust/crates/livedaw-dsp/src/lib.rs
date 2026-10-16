//! Realtime-safe DSP building blocks for Live DAW.
//!
//! Nothing in this crate allocates, locks or panics on the audio path. The
//! WING's processing (filter, EQ models, gate and dynamics models) lives in
//! [`wing`]; it is built from the biquads, delay lines and helpers here.

pub mod biquad;
pub mod delay;
pub mod util;
pub mod wing;

pub use biquad::{Biquad, BiquadCoeffs};
