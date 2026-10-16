//! Live DAW sessions: per-section console state, Show > Scene > Snippet with
//! fades, WING-style fader layers, and crash-safe show files.
//!
//! Everything here runs on the message thread and talks to the realtime engine
//! only through its shared atomics and structure API. Plugin instances (the FX
//! slots' processors) belong to the host application and are reached through
//! [`console::PluginBridge`].

pub mod console;
pub mod layers;
pub mod legacy;
pub mod refs;
pub mod show;
pub mod xml;

pub use console::{section, ApplyResult, CapturedInsert, InsertMode, NoPlugins, PluginBridge, RestoreError};
pub use layers::{FaderRef, Layer, LayerSet};
pub use refs::StripRef;
pub use show::{Scene, Show, Snippet};
