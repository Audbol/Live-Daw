//! Sidecar realtime mixing engine, laid out like a Behringer WING.
//!
//! The engine owns all strips and their parameters. Parameters and meters are
//! lock-free atomics shared with the UI; the audio path never allocates, locks
//! or frees. See `engine.rs` for the per-block signal flow and threading rules.

pub mod analyser;
pub mod atomic;
pub mod denormals;
pub mod engine;
pub mod fft;
pub mod fx;
pub mod garbage;
pub mod listen;
pub mod monitor;
pub mod params;
pub mod pool;
pub mod sends;
pub mod solo;
pub mod sources;
pub mod strip;
pub mod talk;
pub mod tapsend;

pub use analyser::{Analyser, AnalysisControls};
pub use engine::Engine;
pub use fx::{FxRack, InsertCallbacks};
pub use livedaw_recorder as recorder;
pub use livedaw_recorder::{Recorder, NUM_PLAYBACK_CHANNELS, PLAYBACK_SOURCE_BASE};
pub use livedaw_recorder::{UsbDeck, USB_CHANNELS, USB_PLAYER_SOURCE_BASE};
pub use params::{EngineControls, StripKind, StripMeters, StripParams};
pub use sends::{SendParams, MAX_BUSES, SEND_MODES};
pub use strip::Strip;
