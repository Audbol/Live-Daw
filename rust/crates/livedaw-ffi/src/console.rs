//! Console-level C ABI: sources (ROUTING > SOURCES), the strips' talk tags
//! and MUTE state (MUTEGROUP/SIP OVERRIDE), MON DCAs. Solo modes, talkback
//! and the monitor paths are fields of `EngineControls` (shared memory).

use crate::session::{str_arg, LdString};
use crate::{engine, guard, strip};
use livedaw_engine::sources::{self, OscParams, SourceParams};
use livedaw_engine::{Engine, Strip};
use std::sync::atomic::Ordering::Relaxed;

fn source(e: &Engine, index: i32) -> Option<&SourceParams> {
    e.sources().get(usize::try_from(index).ok()?)
}

/// The source table (`*count` entries): hardware ports, then playback channels.
#[no_mangle]
pub unsafe extern "C" fn ld_engine_sources(e: *const Engine, count: *mut i32) -> *const SourceParams {
    let Some(e) = engine(e) else { return std::ptr::null() };
    if let Some(c) = count.as_mut() {
        *c = e.sources().params().len() as i32;
    }
    e.sources().params().as_ptr()
}

/// The source table index of an input port (hardware input or playback
/// channel), -1 if the port has no source.
#[no_mangle]
pub extern "C" fn ld_source_index(port: i32) -> i32 {
    sources::source_index(port).map_or(-1, |i| i as i32)
}

/// The input port of a source table index, -1 if out of range.
#[no_mangle]
pub extern "C" fn ld_source_port(index: i32) -> i32 {
    usize::try_from(index).ok().and_then(sources::source_port).unwrap_or(-1)
}

/// Message thread: a source's name (empty = the port's name).
#[no_mangle]
pub unsafe extern "C" fn ld_source_name(e: *const Engine, index: i32) -> *mut LdString {
    LdString::new(engine(e).map(|e| e.sources().name(index.max(0) as usize)).unwrap_or_default())
}

#[no_mangle]
pub unsafe extern "C" fn ld_source_set_name(e: *const Engine, index: i32, utf8: *const u8, len: usize) {
    if let Some(e) = engine(e) {
        let name = str_arg(utf8, len);
        guard((), || e.sources().set_name(index.max(0) as usize, &name));
    }
}

/// MONO / STEREO / MID-SIDE for a source and its pair partner.
#[no_mangle]
pub unsafe extern "C" fn ld_source_set_mode(e: *const Engine, index: i32, mode: i32) {
    if let Some(e) = engine(e) {
        if source(e, index).is_some() {
            guard((), || e.set_source_mode(index as usize, mode));
        }
    }
}

/// INITIALIZE: a source's settings and name back to the defaults (its pair's
/// too, if paired; the pairing stays).
#[no_mangle]
pub unsafe extern "C" fn ld_source_initialize(e: *const Engine, index: i32) {
    if let Some(e) = engine(e) {
        if source(e, index).is_some() {
            guard((), || e.sources().initialize(index as usize));
        }
    }
}

/// The oscillators' settings (`*count` entries: OSC 1, OSC 2).
#[no_mangle]
pub unsafe extern "C" fn ld_engine_oscillators(e: *const Engine, count: *mut i32) -> *const OscParams {
    let Some(e) = engine(e) else { return std::ptr::null() };
    if let Some(c) = count.as_mut() {
        *c = e.sources().oscillators().len() as i32;
    }
    e.sources().oscillators().as_ptr()
}

/// HOME > TAGS talk tags (bits: TALK SOLO, TALK ON, TALK A SOLO, TALK A ON,
/// TALK B SOLO, TALK B ON).
#[no_mangle]
pub unsafe extern "C" fn ld_strip_talk_tags(s: *const Strip) -> u32 {
    strip(s).map_or(0, |s| s.console.talk_tags.load(Relaxed))
}

#[no_mangle]
pub unsafe extern "C" fn ld_strip_set_talk_tags(s: *const Strip, tags: u32) {
    if let Some(s) = strip(s) {
        s.console.talk_tags.store(tags & livedaw_engine::talk::talk_tag::ALL, Relaxed);
    }
}

/// The MUTE state of the last block: 0 off, 1 muted, 2 muted by a mute
/// group, DCA or SIP (the key flashes), 3 such a mute overridden.
#[no_mangle]
pub unsafe extern "C" fn ld_strip_mute_state(s: *const Strip) -> i32 {
    strip(s).map_or(0, |s| s.console.mute_state())
}

/// Soloed in the last block (own solo, a soloed DCA or TALK SOLO).
#[no_mangle]
pub unsafe extern "C" fn ld_strip_is_soloed(s: *const Strip) -> bool {
    strip(s).is_some_and(|s| s.console.is_soloed())
}

/// Message thread: the MUTE key (MUTEGROUP/SIP OVERRIDE aware).
#[no_mangle]
pub unsafe extern "C" fn ld_engine_press_mute(e: *const Engine, s: *const Strip) {
    if let (Some(e), Some(s)) = (engine(e), strip(s)) {
        guard((), || e.press_mute(s));
    }
}

/// The monitor paths (bit 0 = SPEAKERS, bit 1 = PHONES) whose level DCA `dca`
/// controls (named MON A: PHONES, MON B: SPEAKERS, MON LVL: both).
#[no_mangle]
pub unsafe extern "C" fn ld_engine_dca_monitor_paths(e: *const Engine, dca: i32) -> u32 {
    engine(e).map_or(0, |e| e.dca_monitor_paths(dca.max(0) as usize))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{ld_engine_create, ld_engine_destroy, ld_engine_set_group_name, ld_engine_set_num_channels, ld_engine_strip_at};

    #[test]
    fn sources_tags_and_mutes_through_the_c_abi() {
        unsafe {
            let e = ld_engine_create(0);
            let mut count = 0;
            let table = ld_engine_sources(e, &mut count);
            assert_eq!(count as usize, sources::NUM_SOURCES);
            assert_eq!(ld_source_index(ld_source_port(600)), 600);
            assert_eq!(ld_source_index(-1), -1);
            ld_source_set_mode(e, 5, 1);
            assert_eq!((*table.add(4)).mode.load(Relaxed), 1);
            let name = "KICK";
            ld_source_set_name(e, 4, name.as_ptr(), name.len());
            let s = ld_source_name(e, 4);
            let text = std::slice::from_raw_parts(crate::session::ld_string_data(s), crate::session::ld_string_len(s));
            assert_eq!(text, b"KICK");
            crate::session::ld_string_free(s);
            ld_source_initialize(e, 4);
            assert!((*table.add(4)).is_default() || (*table.add(4)).mode.load(Relaxed) == 1);

            // The oscillators: OSC 2 is the last source.
            assert_eq!(ld_source_index(sources::OSC_SOURCE_BASE + 1), count - 1);
            let osc = ld_engine_oscillators(e, &mut count);
            assert_eq!(count, 2);
            assert_eq!(std::mem::size_of::<OscParams>(), 12, "OscParams in livedaw_engine.h");
            assert_eq!((*osc.add(1)).freq_hz.load(), sources::OSC_DEFAULT_FREQ_HZ);

            ld_engine_set_num_channels(e, 1);
            let ch = ld_engine_strip_at(e, 0, 0);
            ld_strip_set_talk_tags(ch, 0xff);
            assert_eq!(ld_strip_talk_tags(ch), 0x3f);
            assert_eq!(ld_strip_mute_state(ch), 0);
            ld_engine_press_mute(e, ch);
            assert!((*ch).params.mute.load(Relaxed));

            let mon = "MON A";
            ld_engine_set_group_name(e, 0, 2, mon.as_ptr(), mon.len());
            assert_eq!(ld_engine_dca_monitor_paths(e, 2), 2);
            ld_engine_destroy(e);
        }
    }
}
