//! Realtime-safe DSP building blocks for Live DAW.
//!
//! Nothing in this crate allocates, locks or panics on the audio path.

pub mod biquad;
pub mod comp;
pub mod eq;
pub mod gate;
pub mod util;

pub use biquad::{Biquad, BiquadCoeffs};
pub use comp::{Compressor, CompressorSettings};
pub use eq::{BandType, EqBand, EqSettings, ParametricEq, NUM_BANDS};
pub use gate::{GateSettings, NoiseGate};
