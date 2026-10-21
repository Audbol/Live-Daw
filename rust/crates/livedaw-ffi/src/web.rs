//! C ABI for the web remote (livedaw-web): the HTTP + WebSocket server that
//! mirrors the window to browsers. Mirrored in `include/livedaw_engine.h`.
//!
//! Start, stop and status are message thread only; the screen and input
//! functions may be called from any one host thread at a time (the host
//! encodes tiles on a worker thread).

use super::guard;
use super::session::{str_arg, LdString};
use livedaw_web::input::Event;
use livedaw_web::server::{Config, WebServer};
use std::fmt::Write as _;

pub struct LdWeb {
    server: Option<WebServer>,
}

/// One input event from a browser (see `livedaw_web::input`).
#[repr(C)]
#[derive(Default)]
pub struct LdWebInput {
    /// 0 move, 1 down, 2 up, 3 leave, 4 wheel, 5 key down, 6 key up.
    pub kind: i32,
    /// Position as fractions of the screen (0..1).
    pub x: f32,
    pub y: f32,
    /// Wheel deltas (JUCE units).
    pub dx: f32,
    pub dy: f32,
    /// Buttons held after the event: 1 left, 2 right, 4 middle.
    pub buttons: i32,
    /// A character, or a negative special key code (see the page).
    pub key: i32,
    /// 1 shift, 2 ctrl, 4 alt, 8 meta.
    pub mods: u32,
    pub client: u32,
}

unsafe fn web<'a>(w: *mut LdWeb) -> Option<&'a mut LdWeb> {
    w.as_mut()
}

unsafe fn running<'a>(w: *mut LdWeb) -> Option<&'a WebServer> {
    w.as_ref().and_then(|w| w.server.as_ref())
}

#[no_mangle]
pub extern "C" fn ld_web_create() -> *mut LdWeb {
    guard(std::ptr::null_mut(), || Box::into_raw(Box::new(LdWeb { server: None })))
}

/// Stops the server and frees `w`.
#[no_mangle]
pub unsafe extern "C" fn ld_web_destroy(w: *mut LdWeb) {
    if !w.is_null() {
        guard((), || drop(Box::from_raw(w)));
    }
}

/// (Re)starts the server on `port` (0: any free port) of all interfaces.
/// `pin`: 4-8 digits or empty. Returns false if the port could not be bound
/// (see `ld_web_status`).
#[no_mangle]
pub unsafe extern "C" fn ld_web_start(w: *mut LdWeb, port: i32, pin: *const u8, pin_len: usize, view_only: bool) -> bool {
    let Some(w) = web(w) else { return false };
    let pin = str_arg(pin, pin_len).into_owned();
    guard(false, || {
        if let Some(mut s) = w.server.take() {
            s.stop();
        }
        let config = Config { port: u16::try_from(port).unwrap_or(0), pin, view_only, ..Config::default() };
        let server = WebServer::start(&config);
        let ok = server.addr().is_some();
        w.server = Some(server);
        ok
    })
}

#[no_mangle]
pub unsafe extern "C" fn ld_web_stop(w: *mut LdWeb) {
    if let Some(w) = web(w) {
        guard((), || {
            if let Some(mut s) = w.server.take() {
                s.stop();
            }
        });
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_web_running(w: *mut LdWeb) -> bool {
    running(w).is_some()
}

/// The bound port, or -1.
#[no_mangle]
pub unsafe extern "C" fn ld_web_port(w: *mut LdWeb) -> i32 {
    running(w).and_then(WebServer::addr).map_or(-1, |a| i32::from(a.port()))
}

/// Status as tab-separated lines (never null): `error\t<message>`,
/// `client\t<address>\t<watching 0|1>\t<seconds>`, `traffic\t<bytes>\t<frames>`.
#[no_mangle]
pub unsafe extern "C" fn ld_web_status(w: *mut LdWeb) -> *mut LdString {
    let text = running(w).map_or_else(String::new, |s| {
        guard(String::new(), || {
            let st = s.status();
            let mut out = String::new();
            if let Some(e) = &st.error {
                let _ = writeln!(out, "error\t{e}");
            }
            for c in &st.clients {
                let _ = writeln!(out, "client\t{}\t{}\t{}", c.addr, u8::from(c.watching), c.seconds);
            }
            let _ = writeln!(out, "traffic\t{}\t{}", st.bytes_sent, st.frames_sent);
            out
        })
    });
    LdString::new(text)
}

/// Whether any browser watches (the host snapshots only then).
#[no_mangle]
pub unsafe extern "C" fn ld_web_watched(w: *mut LdWeb) -> bool {
    running(w).is_some_and(|s| guard(false, || s.watched()))
}

/// A new screen size in pixels, split into `tile`-pixel tiles: every tile is
/// unknown again. Returns the generation `ld_web_put_tile` quotes (0: not running).
#[no_mangle]
pub unsafe extern "C" fn ld_web_reset_screen(w: *mut LdWeb, width: i32, height: i32, tile: i32) -> u32 {
    let Some(s) = running(w) else { return 0 };
    let dim = |v: i32| u32::try_from(v).unwrap_or(0);
    guard(0, || s.reset_screen(dim(width), dim(height), dim(tile)))
}

/// Of the `count` tile indices in `candidates`, writes those whose pixels
/// changed since they were last seen to `changed` (room for `count`) and
/// returns how many. `pixels`: the whole screen, `stride` bytes per row,
/// `bpp` bytes per pixel, `height` rows.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn ld_web_changed_tiles(
    w: *mut LdWeb,
    pixels: *const u8,
    stride: i32,
    height: i32,
    bpp: i32,
    candidates: *const u32,
    count: usize,
    changed: *mut u32,
) -> usize {
    let Some(s) = running(w) else { return 0 };
    if pixels.is_null() || candidates.is_null() || changed.is_null() || stride <= 0 || height <= 0 || bpp <= 0 {
        return 0;
    }
    let pixels = std::slice::from_raw_parts(pixels, stride as usize * height as usize);
    let candidates = std::slice::from_raw_parts(candidates, count);
    let out = std::slice::from_raw_parts_mut(changed, count);
    guard(0, || {
        let list = s.changed_tiles(pixels, stride as usize, bpp as usize, candidates);
        out[..list.len()].copy_from_slice(&list);
        list.len()
    })
}

/// The encoded image of a tile: format 0 PNG, 1 JPEG. Ignored (false) if
/// `generation` is out of date. Call `ld_web_publish` after a batch.
#[no_mangle]
pub unsafe extern "C" fn ld_web_put_tile(
    w: *mut LdWeb,
    generation: u32,
    index: u32,
    format: i32,
    data: *const u8,
    len: usize,
) -> bool {
    let Some(s) = running(w) else { return false };
    if data.is_null() {
        return false;
    }
    let data = std::slice::from_raw_parts(data, len);
    guard(false, || s.put_tile(generation, index, u8::try_from(format).unwrap_or(0), data))
}

/// Sends the tiles put since the last call to the browsers.
#[no_mangle]
pub unsafe extern "C" fn ld_web_publish(w: *mut LdWeb) {
    if let Some(s) = running(w) {
        guard((), || s.publish());
    }
}

/// The next input event from a browser; false if none.
#[no_mangle]
pub unsafe extern "C" fn ld_web_take_input(w: *mut LdWeb, out: *mut LdWebInput) -> bool {
    let (Some(s), Some(out)) = (running(w), out.as_mut()) else { return false };
    guard(false, || {
        let Some(Event { kind, x, y, dx, dy, buttons, key, mods, client }) = s.take_input() else { return false };
        *out = LdWebInput { kind, x, y, dx, dy, buttons, key, mods, client };
        true
    })
}
