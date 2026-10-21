//! Sidecar's web remote: browsers on the local network see the app's window
//! and control it.
//!
//! * `http`: the embedded pages and the routing (`/` the screen mirror, `/ws`
//!   its WebSocket);
//! * `ws`: the WebSocket handshake and framing;
//! * `auth`: the optional PIN, with wrong guesses rate-limited;
//! * `screen`: the mirrored screen as tiles (change detection, the latest
//!   image of each tile, what each browser still lacks);
//! * `input`: the pointer, wheel and key messages from the page, queued for
//!   the host;
//! * `server`: the threads tying it together.
//!
//! The host (the JUCE app) snapshots its window and encodes the changed
//! tiles; this crate only moves bytes. Only std is used.

pub mod auth;
pub mod http;
pub mod input;
pub mod screen;
pub mod server;
pub mod ws;

pub use server::{Config, Status, WebServer};
