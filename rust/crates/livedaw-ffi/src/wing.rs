//! C ABI for the WING remote (livedaw-wing): the discovery / OSC / Native
//! servers, WING snapshot and show files, and the tree coverage report.
//! Message thread only. Mirrored in `include/livedaw_engine.h`.
//!
//! An `LdWing` borrows the engine: the host destroys it (which stops the
//! servers) before destroying the engine.

use super::guard;
use super::session::{str_arg, LdShow, LdString};
use livedaw_engine::Engine;
use livedaw_wing::map::{LibAction, Library, Safes, UsbAction, UsbSettings};
use livedaw_wing::server::{ClientKind, Config, EngineHandle, EnginePtr, WingServer};
use livedaw_wing::{snap, Identity, WingState};
use std::fmt::Write as _;
use std::path::PathBuf;
use std::sync::Arc;

pub struct LdWing {
    engine: *const Engine,
    state: Arc<WingState>,
    server: Option<WingServer>,
}

unsafe fn wing<'a>(w: *mut LdWing) -> Option<&'a mut LdWing> {
    w.as_mut()
}

unsafe fn write(out: *mut *mut LdString, s: String) {
    if let Some(out) = out.as_mut() {
        *out = LdString::new(s);
    }
}

impl LdWing {
    fn engine(&self) -> &Engine {
        // SAFETY: the engine outlives this object (see the module docs).
        unsafe { &*self.engine }
    }
}

/// Creates the WING remote for `e` (servers stopped).
#[no_mangle]
pub unsafe extern "C" fn ld_wing_create(e: *const Engine) -> *mut LdWing {
    if e.is_null() {
        return std::ptr::null_mut();
    }
    guard(std::ptr::null_mut(), || {
        Box::into_raw(Box::new(LdWing { engine: e, state: Arc::new(WingState::default()), server: None }))
    })
}

/// Stops the servers and frees `w`.
#[no_mangle]
pub unsafe extern "C" fn ld_wing_destroy(w: *mut LdWing) {
    if !w.is_null() {
        guard((), || drop(Box::from_raw(w)));
    }
}

/// Port numbers: 0 picks a free port. `enable` bits: 1 discovery, 2 OSC,
/// 4 Native. Restarts the servers if they are running. Returns false if any
/// enabled server could not start (see `ld_wing_status`); the others run.
#[no_mangle]
pub unsafe extern "C" fn ld_wing_start(
    w: *mut LdWing,
    name: *const u8,
    name_len: usize,
    discovery_port: i32,
    osc_port: i32,
    native_port: i32,
    enable: u32,
) -> bool {
    let Some(w) = wing(w) else { return false };
    let name = str_arg(name, name_len).into_owned();
    guard(false, || {
        if let Some(mut s) = w.server.take() {
            s.stop();
        }
        let port = |p: i32| u16::try_from(p).unwrap_or(0);
        let mut identity = Identity::default();
        if !name.trim().is_empty() {
            identity.name = name.trim().to_string();
        }
        let config = Config {
            identity,
            discovery_port: port(discovery_port),
            osc_port: port(osc_port),
            native_port: port(native_port),
            discovery: enable & 1 != 0,
            osc: enable & 2 != 0,
            native: enable & 4 != 0,
            ..Config::default()
        };
        let handle: Arc<dyn EngineHandle> = Arc::new(EnginePtr(w.engine));
        let server = WingServer::start(handle, w.state.clone(), &config);
        let ok = server.status().errors.is_empty();
        w.server = Some(server);
        ok
    })
}

#[no_mangle]
pub unsafe extern "C" fn ld_wing_stop(w: *mut LdWing) {
    if let Some(w) = wing(w) {
        guard((), || {
            if let Some(mut s) = w.server.take() {
                s.stop();
            }
        });
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_wing_running(w: *mut LdWing) -> bool {
    wing(w).is_some_and(|w| w.server.is_some())
}

/// Renames the console (seen by discovery and clients at once).
#[no_mangle]
pub unsafe extern "C" fn ld_wing_set_name(w: *mut LdWing, name: *const u8, len: usize) {
    let Some(w) = wing(w) else { return };
    let name = str_arg(name, len).trim().to_string();
    if !name.is_empty() {
        guard((), || w.state.identity.lock().unwrap_or_else(|e| e.into_inner()).name = name);
    }
}

/// The bound port of a server (0 discovery, 1 OSC, 2 Native), or -1.
#[no_mangle]
pub unsafe extern "C" fn ld_wing_port(w: *mut LdWing, which: i32) -> i32 {
    let Some(w) = wing(w) else { return -1 };
    guard(-1, || {
        let Some(s) = &w.server else { return -1 };
        let st = s.status();
        let addr = match which {
            0 => st.discovery,
            1 => st.osc,
            2 => st.native,
            _ => None,
        };
        addr.map_or(-1, |a| i32::from(a.port()))
    })
}

/// Status as tab-separated lines (never null):
/// `error\t<message>`,
/// `client\t<native|osc>\t<address>\t<subscribed 0|1>\t<seconds>`,
/// `traffic\t<OSC messages>\t<Native bytes>`.
#[no_mangle]
pub unsafe extern "C" fn ld_wing_status(w: *mut LdWing) -> *mut LdString {
    let text = wing(w).map_or_else(String::new, |w| {
        guard(String::new(), || {
            let Some(s) = &w.server else { return String::new() };
            let st = s.status();
            let mut out = String::new();
            for e in &st.errors {
                let _ = writeln!(out, "error\t{e}");
            }
            for c in &st.clients {
                let kind = if c.kind == ClientKind::Native { "native" } else { "osc" };
                let _ = writeln!(out, "client\t{kind}\t{}\t{}\t{}", c.addr, u8::from(c.subscribed), c.seconds);
            }
            let _ = writeln!(out, "traffic\t{}\t{}", st.osc_messages, st.native_bytes);
            out
        })
    });
    LdString::new(text)
}

/// Applies a WING snapshot (.snap) file to the console. Returns false if it
/// could not be read; `report` gets the summary (or the error) followed by
/// the details of what was not applied. Channels may be added: resync strips.
#[no_mangle]
pub unsafe extern "C" fn ld_wing_import_snap(w: *mut LdWing, path: *const u8, len: usize, report: *mut *mut LdString) -> bool {
    let Some(w) = wing(w) else { return false };
    let path = PathBuf::from(str_arg(path, len).into_owned());
    let r = guard(Err("internal error while importing".into()), || {
        snap::import_file(w.engine(), &w.state, &path, &snap::ImportOptions::default())
    });
    match r {
        Ok(rep) => {
            write(report, rep.details());
            true
        }
        Err(e) => {
            write(report, e);
            false
        }
    }
}

/// Writes the console as a WING snapshot (.snap) file (experimental).
#[no_mangle]
pub unsafe extern "C" fn ld_wing_export_snap(w: *mut LdWing, path: *const u8, len: usize, message: *mut *mut LdString) -> bool {
    let Some(w) = wing(w) else { return false };
    let path = PathBuf::from(str_arg(path, len).into_owned());
    match guard(Err("internal error while exporting".into()), || snap::export_file(w.engine(), &w.state, &path)) {
        Ok(()) => true,
        Err(e) => {
            write(message, e);
            false
        }
    }
}

/// Adds the snapshot scenes of a WING show (.show) file to `show`, without
/// changing the live console (other than growing it to 40 channels).
#[no_mangle]
pub unsafe extern "C" fn ld_wing_import_show(
    w: *mut LdWing,
    show: *mut LdShow,
    path: *const u8,
    len: usize,
    report: *mut *mut LdString,
) -> bool {
    let (Some(w), Some(show)) = (wing(w), show.as_mut()) else { return false };
    let path = PathBuf::from(str_arg(path, len).into_owned());
    let r =
        guard(Err("internal error while importing".into()), || snap::import_show(&mut show.show, w.engine(), &w.state, &path));
    match r {
        Ok(rep) => {
            write(report, rep.summary());
            true
        }
        Err(e) => {
            write(report, e);
            false
        }
    }
}

/// Publishes the show for `/$ctl/lib`: its name, number of scenes and the
/// current scene (index, -1 none; name; EDIT TAG, 0 none). From then on the
/// host runs the scene recalls clients ask for (`ld_wing_take_library_action`).
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn ld_wing_set_library(
    w: *mut LdWing,
    show: *const u8,
    show_len: usize,
    num_scenes: i32,
    current: i32,
    scene: *const u8,
    scene_len: usize,
    tag: i32,
) {
    let Some(w) = wing(w) else { return };
    let library = Library {
        attached: true,
        show: str_arg(show, show_len).into_owned(),
        num_scenes,
        current,
        scene: str_arg(scene, scene_len).into_owned(),
        tag,
    };
    guard((), || w.state.set_library(library));
}

/// No host runs scene recalls any more (clients' GO is refused).
#[no_mangle]
pub unsafe extern "C" fn ld_wing_clear_library(w: *mut LdWing) {
    if let Some(w) = wing(w) {
        guard((), || w.state.set_library(Library::default()));
    }
}

/// The next scene recall a client asked for: true with `action` 1 GO (`index`
/// the scene, 0-based), 2 GO NEXT, 3 GO PREV or 4 GO TAG (`index` the tag).
#[no_mangle]
pub unsafe extern "C" fn ld_wing_take_library_action(w: *mut LdWing, action: *mut i32, index: *mut i32) -> bool {
    let Some(w) = wing(w) else { return false };
    let Some(a) = guard(None, || w.state.take_lib_action()) else { return false };
    let (code, i) = match a {
        LibAction::Go(i) => (1, i),
        LibAction::GoNext => (2, 0),
        LibAction::GoPrev => (3, 0),
        LibAction::GoTag(t) => (4, t),
    };
    if let Some(out) = action.as_mut() {
        *out = code;
    }
    if let Some(out) = index.as_mut() {
        *out = i;
    }
    true
}

/// Publishes the host's USB recorder settings for `/rec`: the storage folder,
/// REC PATH (relative to it, "/" separated) and the next recording's channels
/// (2, 4) and bits (16, 24). From then on clients' REC, NEW FILE and PLAYFILE
/// reach the host (`ld_wing_take_usb_action`). A client's change still to be
/// taken (`ld_wing_take_usb_settings`) is kept.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn ld_wing_set_usb(
    w: *mut LdWing,
    storage: *const u8,
    storage_len: usize,
    rec_path: *const u8,
    rec_path_len: usize,
    channels: i32,
    bits: i32,
) {
    let Some(w) = wing(w) else { return };
    let settings = UsbSettings {
        attached: true,
        storage: str_arg(storage, storage_len).into_owned(),
        rec_path: str_arg(rec_path, rec_path_len).into_owned(),
        channels,
        bits,
    };
    guard((), || w.state.set_usb_settings(settings));
}

/// No host takes the USB actions any more.
#[no_mangle]
pub unsafe extern "C" fn ld_wing_clear_usb(w: *mut LdWing) {
    if let Some(w) = wing(w) {
        guard((), || {
            w.state.take_usb_settings();
            w.state.set_usb_settings(UsbSettings::default());
        });
    }
}

/// The USB settings a client changed: true with the channels, bits and REC
/// PATH (relative to the storage folder; `rec_path` may be null).
#[no_mangle]
pub unsafe extern "C" fn ld_wing_take_usb_settings(
    w: *mut LdWing,
    channels: *mut i32,
    bits: *mut i32,
    rec_path: *mut *mut LdString,
) -> bool {
    let Some(w) = wing(w) else { return false };
    let Some(u) = guard(None, || w.state.take_usb_settings()) else { return false };
    if let Some(out) = channels.as_mut() {
        *out = u.channels;
    }
    if let Some(out) = bits.as_mut() {
        *out = u.bits;
    }
    write(rec_path, u.rec_path);
    true
}

/// The next USB recorder action a client asked for: true with `action` 1 REC
/// (a new recording), 2 NEW FILE or 3 PLAYFILE (`file`: the file, relative to
/// the storage folder; may be null).
#[no_mangle]
pub unsafe extern "C" fn ld_wing_take_usb_action(w: *mut LdWing, action: *mut i32, file: *mut *mut LdString) -> bool {
    let Some(w) = wing(w) else { return false };
    let Some(a) = guard(None, || w.state.take_usb_action()) else { return false };
    let (code, text) = match a {
        UsbAction::Record => (1, String::new()),
        UsbAction::NewFile => (2, String::new()),
        UsbAction::PlayFile(f) => (3, f),
    };
    if let Some(out) = action.as_mut() {
        *out = code;
    }
    write(file, text);
    true
}

/// Publishes the show's GLOBAL safes for `/$ctl/safes` (bit n = DCA, mute
/// group or FX slot n+1), unless a client's change is still to be taken.
#[no_mangle]
pub unsafe extern "C" fn ld_wing_set_safes(w: *mut LdWing, dcas: u32, mute_groups: u32, fx: u32) {
    if let Some(w) = wing(w) {
        guard((), || w.state.set_safes(Safes { attached: true, dcas, mute_groups, fx }));
    }
}

/// No show any more.
#[no_mangle]
pub unsafe extern "C" fn ld_wing_clear_safes(w: *mut LdWing) {
    if let Some(w) = wing(w) {
        guard((), || {
            w.state.take_safes();
            w.state.set_safes(Safes::default());
        });
    }
}

/// The global safes a client changed: true with the masks (any pointer may be null).
#[no_mangle]
pub unsafe extern "C" fn ld_wing_take_safes(w: *mut LdWing, dcas: *mut u32, mute_groups: *mut u32, fx: *mut u32) -> bool {
    let Some(w) = wing(w) else { return false };
    let Some(s) = guard(None, || w.state.take_safes()) else { return false };
    for (out, v) in [(dcas, s.dcas), (mute_groups, s.mute_groups), (fx, s.fx)] {
        if let Some(out) = out.as_mut() {
            *out = v;
        }
    }
    true
}

/// How much of the WING tree maps onto the engine (a text report).
#[no_mangle]
pub extern "C" fn ld_wing_coverage() -> *mut LdString {
    LdString::new(guard(String::new(), || livedaw_wing::map::coverage().report()))
}

/// MIDI SYSEX CONTROL on the WING tree: `cmd` 2 executes a node string, 3
/// dumps a node, 5 describes one (livedaw_wing::sysex). True with `out` the
/// data (empty for 2), false with `out` the console's error text.
#[no_mangle]
pub unsafe extern "C" fn ld_wing_sysex(w: *mut LdWing, cmd: i32, text: *const u8, len: usize, out: *mut *mut LdString) -> bool {
    let Some(w) = wing(w) else { return false };
    let text = str_arg(text, len).into_owned();
    let result = guard(Err(livedaw_wing::sysex::NODE_NOT_FOUND.to_owned()), || {
        let c = livedaw_wing::Console::new(w.engine(), &w.state);
        let r = match cmd {
            2 => livedaw_wing::sysex::execute(&c, &text).map(|_| String::new()),
            3 => livedaw_wing::sysex::dump(&c, &text),
            5 => livedaw_wing::sysex::describe(&c, &text),
            _ => Err(livedaw_wing::sysex::NODE_NOT_FOUND),
        };
        r.map_err(str::to_owned)
    });
    match result {
        Ok(data) => {
            write(out, data);
            true
        }
        Err(e) => {
            write(out, e);
            false
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::session::{ld_string_data, ld_string_free, ld_string_len};

    unsafe fn take(s: *mut LdString) -> String {
        let out = String::from_utf8_lossy(std::slice::from_raw_parts(ld_string_data(s), ld_string_len(s))).into_owned();
        ld_string_free(s);
        out
    }

    #[test]
    fn start_status_rename_stop() {
        let engine = Box::into_raw(Box::new(Engine::new(0)));
        unsafe {
            let w = ld_wing_create(engine);
            assert!(!ld_wing_running(w));
            let name = "FOH";
            assert!(ld_wing_start(w, name.as_ptr(), name.len(), 0, 0, 0, 7));
            assert!(ld_wing_running(w));
            let osc = ld_wing_port(w, 1);
            assert!(osc > 0 && ld_wing_port(w, 0) > 0 && ld_wing_port(w, 2) > 0);

            let sock = std::net::UdpSocket::bind("127.0.0.1:0").unwrap();
            sock.set_read_timeout(Some(std::time::Duration::from_secs(2))).unwrap();
            let ask = |sock: &std::net::UdpSocket| {
                sock.send_to(b"/?\0\0", ("127.0.0.1", osc as u16)).unwrap();
                let mut buf = [0u8; 512];
                let n = sock.recv(&mut buf).unwrap();
                String::from_utf8_lossy(&buf[..n]).into_owned()
            };
            assert!(ask(&sock).contains("FOH"));
            let renamed = "MONITORS";
            ld_wing_set_name(w, renamed.as_ptr(), renamed.len());
            assert!(ask(&sock).contains("MONITORS"));

            let status = take(ld_wing_status(w));
            assert!(status.contains("client\tosc\t127.0.0.1:"), "{status}");
            assert!(status.contains("traffic\t2\t0"), "{status}");
            assert!(take(ld_wing_coverage()).starts_with("WING tree coverage"));
            ld_wing_stop(w);
            assert!(!ld_wing_running(w) && ld_wing_port(w, 1) == -1);
            ld_wing_destroy(w);
            drop(Box::from_raw(engine));
        }
    }

    #[test]
    fn library_actions_reach_the_host() {
        let engine = Box::into_raw(Box::new(Engine::new(0)));
        unsafe {
            let w = ld_wing_create(engine);
            let (mut action, mut index) = (0, 0);
            assert!(!ld_wing_take_library_action(w, &mut action, &mut index));
            let (show, scene) = ("Gig", "Intro");
            ld_wing_set_library(w, show.as_ptr(), show.len(), 3, 0, scene.as_ptr(), scene.len(), 5);
            let state = (*w).state.clone();
            let c = livedaw_wing::Console::new(&*engine, &state);
            let node = |p: &str| livedaw_wing::schema().resolve(p, &c).unwrap();
            assert_eq!(c.get(node("/$ctl/lib/$activeid")), Some(livedaw_wing::Value::Int(5)));
            c.set(node("/$ctl/lib/$actionidx"), &livedaw_wing::Value::Int(2)).unwrap();
            c.set(node("/$ctl/lib/$action"), &livedaw_wing::Value::str("GO")).unwrap();
            c.set(node("/$ctl/lib/$action"), &livedaw_wing::Value::str("GOTAG")).unwrap();
            assert!(ld_wing_take_library_action(w, &mut action, &mut index));
            assert_eq!((action, index), (1, 2));
            assert!(ld_wing_take_library_action(w, &mut action, &mut index));
            assert_eq!((action, index), (4, 2));
            ld_wing_clear_library(w);
            assert!(c.set(node("/$ctl/lib/$action"), &livedaw_wing::Value::str("GO")).is_err());
            drop(c);
            ld_wing_destroy(w);
            drop(Box::from_raw(engine));
        }
    }

    #[test]
    fn usb_actions_and_safes_reach_the_host() {
        let engine = Box::into_raw(Box::new(Engine::new(0)));
        unsafe {
            (*engine).set_num_channels(4);
            let w = ld_wing_create(engine);
            let (storage, rec) = ("/tmp/usb", "REC");
            ld_wing_set_usb(w, storage.as_ptr(), storage.len(), rec.as_ptr(), rec.len(), 4, 16);
            ld_wing_set_safes(w, 0b11, 0, 0);
            let state = (*w).state.clone();
            let c = livedaw_wing::Console::new(&*engine, &state);
            let node = |p: &str| livedaw_wing::schema().resolve(p, &c).unwrap();
            assert_eq!(c.get(node("/rec/$path")), Some(livedaw_wing::Value::str("REC")));
            assert_eq!(c.get(node("/$ctl/safes/dca")), Some(livedaw_wing::Value::str("1,2")));
            c.set(node("/rec/resolution"), &livedaw_wing::Value::str("24")).unwrap();
            c.set(node("/rec/$action"), &livedaw_wing::Value::str("REC")).unwrap();
            c.set(node("/play/$playfile"), &livedaw_wing::Value::str("a.wav")).unwrap();
            c.set(node("/play/$action"), &livedaw_wing::Value::str("PLAYFILE")).unwrap();
            c.set(node("/$ctl/safes/mute"), &livedaw_wing::Value::str("8")).unwrap();

            let (mut channels, mut bits, mut path) = (0, 0, std::ptr::null_mut());
            assert!(ld_wing_take_usb_settings(w, &mut channels, &mut bits, &mut path));
            assert_eq!((channels, bits, take(path).as_str()), (4, 24, "REC"));
            assert!(!ld_wing_take_usb_settings(w, &mut channels, &mut bits, std::ptr::null_mut()));
            let (mut action, mut file) = (0, std::ptr::null_mut());
            assert!(ld_wing_take_usb_action(w, &mut action, &mut file));
            assert_eq!((action, take(file).as_str()), (1, ""));
            assert!(ld_wing_take_usb_action(w, &mut action, &mut file));
            assert_eq!((action, take(file).as_str()), (3, "a.wav"));
            assert!(!ld_wing_take_usb_action(w, &mut action, std::ptr::null_mut()));
            let (mut dcas, mut groups, mut fx) = (0, 0, 0);
            assert!(ld_wing_take_safes(w, &mut dcas, &mut groups, &mut fx));
            assert_eq!((dcas, groups, fx), (0b11, 1 << 7, 0));
            assert!(!ld_wing_take_safes(w, &mut dcas, std::ptr::null_mut(), std::ptr::null_mut()));
            ld_wing_clear_usb(w);
            assert!(c.set(node("/rec/$action"), &livedaw_wing::Value::str("REC")).is_err());
            ld_wing_clear_safes(w);
            drop(c);
            ld_wing_destroy(w);
            drop(Box::from_raw(engine));
        }
    }

    #[test]
    fn snapshot_export_and_import() {
        let engine = Box::into_raw(Box::new(Engine::new(0)));
        unsafe {
            (*engine).set_num_channels(8);
            let w = ld_wing_create(engine);
            let dir = std::env::temp_dir().join(format!("ld-wing-ffi-{}", std::process::id()));
            std::fs::create_dir_all(&dir).unwrap();
            let path = dir.join("x.snap");
            let p = path.to_string_lossy().into_owned();
            let mut msg = std::ptr::null_mut();
            assert!(ld_wing_export_snap(w, p.as_ptr(), p.len(), &mut msg));
            assert!(msg.is_null());
            let mut report = std::ptr::null_mut();
            assert!(ld_wing_import_snap(w, p.as_ptr(), p.len(), &mut report));
            assert!(take(report).starts_with("WING snapshot"));
            let missing = dir.join("missing.snap").to_string_lossy().into_owned();
            let mut report = std::ptr::null_mut();
            assert!(!ld_wing_import_snap(w, missing.as_ptr(), missing.len(), &mut report));
            assert!(!take(report).is_empty());
            ld_wing_destroy(w);
            drop(Box::from_raw(engine));
            let _ = std::fs::remove_dir_all(dir);
        }
    }
}
