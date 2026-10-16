//! Live DAW realtime mixing engine, laid out like a Behringer WING.
//!
//! The engine owns all strips and their parameters. Parameters and meters are
//! lock-free atomics shared with the UI; the audio path never allocates, locks
//! or frees. See `engine.rs` for the per-block signal flow and threading rules.

pub mod atomic;
pub mod denormals;
pub mod engine;
pub mod fx;
pub mod garbage;
pub mod params;
pub mod pool;
pub mod sends;
pub mod strip;

pub use engine::Engine;
pub use fx::{FxRack, InsertCallbacks};
pub use params::{EngineControls, StripKind, StripMeters, StripParams};
pub use sends::{SendParams, MAX_BUSES, SEND_MODES};
pub use strip::Strip;
