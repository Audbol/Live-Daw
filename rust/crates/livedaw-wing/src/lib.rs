//! Behringer WING interoperability for Sidecar.
//!
//! * [`schema`]: the WING parameter tree (names, ids, types, ranges), generated
//!   from libwing's parameter sweep (Apache-2.0).
//! * [`map`]: the tree mapped onto the engine, both ways, with unit
//!   conversions; nodes without an equivalent live in a shadow store.
//! * Remote servers that answer like a WING: discovery (UDP 2222), OSC (UDP
//!   2223) and the Native protocol (TCP 2222, meters over UDP).
//! * WING snapshot (.snap) and show (.show) files.
//!
//! Clean-room: built from the public protocol document (Patrick-Gilles
//! Maillot's "WING Remote Protocols"), libwing and the Bitfocus Companion
//! module; no Behringer firmware or WING Edit code is used.

pub mod json;
pub mod map;
pub mod meters;
pub mod native;
pub mod osc;
pub mod schema;
pub mod server;
pub mod snap;
pub mod surface;
pub mod sysex;
pub mod value;

pub use map::{Console, Identity, SetError, WingState};
pub use schema::{schema, Node};
pub use server::{Config, EngineHandle, EnginePtr, Status, WingServer};
pub use value::Value;
