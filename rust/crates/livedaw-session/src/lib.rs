//! Sidecar sessions: per-section console state, Show > Scene > Snippet with
//! fades, WING-style fader layers, crash-safe show files and LIBRARY files.
//!
//! Everything here runs on the message thread and talks to the realtime engine
//! only through its shared atomics and structure API. Plugin instances (the FX
//! slots' processors) belong to the host application and are reached through
//! [`console::PluginBridge`].

pub mod console;
pub mod custom;
pub mod globals;
pub mod layers;
pub mod legacy;
pub mod library;
pub mod presets;
pub mod refs;
pub mod scope;
pub mod show;
pub mod xml;

pub use console::{section, ApplyResult, CapturedInsert, InsertMode, NoPlugins, PluginBridge, RestoreError};
pub use custom::{CcControl, CcFunction, CustomControls};
pub use layers::{FaderRef, Layer, LayerSet};
pub use refs::StripRef;
pub use scope::{GlobalSafe, RecordedChanges, Scope};
pub use show::{Scene, Show, Snippet};
