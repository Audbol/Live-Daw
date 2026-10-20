//! Console-wide state beyond the strips, added with the console-level
//! features: SOLO MODE, MUTEGROUP/SIP OVERRIDE and SOURCE SOLO, talkback, the
//! strips' talk tags, the monitor paths (sources, the PHONES path, and each
//! path's DIRECT IN, BAND SOLO, EQ, PAN / WIDTH, INVERT, DELAY and LIMITER)
//! and the sources of ROUTING > SOURCES (with their mute-group tags and the
//! oscillators' settings).
//!
//! Show files keep all of it ([`capture`] / [`apply`] from `save_console` /
//! `load_console`); scenes recall the monitor paths with ROUTING, like the
//! rest of the monitor setup ([`capture_monitor_paths`]). Missing elements
//! (older shows) leave the reset engine's defaults.

use crate::refs::StripRef;
use crate::xml::{fmt_f32, Element};
use livedaw_engine::atomic::AtomicF32;
use livedaw_engine::monitor::MonitorDspParams;
use livedaw_engine::params::MonitorSourceParams;
use livedaw_engine::sources::{source_mode, SourceParams};
use livedaw_engine::talk::talk_tag;
use livedaw_engine::Engine;
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicU32, Ordering::Relaxed};

fn put_b(e: &mut Element, name: &str, v: &AtomicBool) {
    e.set_bool(name, v.load(Relaxed));
}
fn put_i(e: &mut Element, name: &str, v: &AtomicI32) {
    e.set(name, v.load(Relaxed));
}
fn put_u(e: &mut Element, name: &str, v: &AtomicU32) {
    e.set(name, v.load(Relaxed));
}
fn put_f(e: &mut Element, name: &str, v: &AtomicF32) {
    e.set(name, fmt_f32(v.load()));
}
fn get_b(e: &Element, name: &str, v: &AtomicBool) {
    if e.has_attr(name) {
        v.store(e.bool_or(name, false), Relaxed);
    }
}
fn get_i(e: &Element, name: &str, v: &AtomicI32) {
    if e.has_attr(name) {
        v.store(e.int_or(name, 0), Relaxed);
    }
}
fn get_u(e: &Element, name: &str, v: &AtomicU32) {
    if e.has_attr(name) {
        v.store(e.int_or(name, 0) as u32, Relaxed);
    }
}
fn get_f(e: &Element, name: &str, v: &AtomicF32) {
    if e.has_attr(name) {
        v.store(e.double_or(name, 0.0) as f32);
    }
}

fn put_source(e: &mut Element, s: &MonitorSourceParams) {
    put_i(e, "src", &s.source);
    put_f(e, "srcLevel", &s.source_level_db);
    put_f(e, "srcMix", &s.source_mix_db);
    put_f(e, "pflDim", &s.pfl_dim_db);
}

fn get_source(e: &Element, s: &MonitorSourceParams) {
    get_i(e, "src", &s.source);
    get_f(e, "srcLevel", &s.source_level_db);
    get_f(e, "srcMix", &s.source_mix_db);
    get_f(e, "pflDim", &s.pfl_dim_db);
}

/// A path's processing: `<Dsp dirIn=.. dirLvl=.. bandSolo=.. eq=.. pan=..
/// wid=.. inv=.. dly=.. dlyM=.. lim=..><B i="0" g=".." f=".." q=".."/>...</Dsp>`.
fn put_dsp(parent: &mut Element, d: &MonitorDspParams) {
    let e = parent.add_new("Dsp");
    put_i(e, "dirIn", &d.direct_source);
    put_f(e, "dirLvl", &d.direct_level_db);
    put_f(e, "bandSolo", &d.band_solo_db);
    put_b(e, "eq", &d.eq_on);
    put_f(e, "pan", &d.pan);
    put_f(e, "wid", &d.width);
    put_b(e, "inv", &d.invert);
    put_b(e, "dly", &d.delay_on);
    put_f(e, "dlyM", &d.delay_m);
    put_f(e, "lim", &d.limiter_db);
    for (i, b) in d.eq.iter().enumerate() {
        let x = e.add_new("B");
        x.set("i", i);
        put_f(x, "g", &b.gain_db);
        put_f(x, "f", &b.freq);
        put_f(x, "q", &b.q);
    }
}

/// Shows from before the monitor processing have no `<Dsp>`: the defaults stay.
fn get_dsp(parent: &Element, d: &MonitorDspParams) {
    let Some(e) = parent.child("Dsp") else { return };
    get_i(e, "dirIn", &d.direct_source);
    get_f(e, "dirLvl", &d.direct_level_db);
    get_f(e, "bandSolo", &d.band_solo_db);
    get_b(e, "eq", &d.eq_on);
    get_f(e, "pan", &d.pan);
    get_f(e, "wid", &d.width);
    get_b(e, "inv", &d.invert);
    get_b(e, "dly", &d.delay_on);
    get_f(e, "dlyM", &d.delay_m);
    get_f(e, "lim", &d.limiter_db);
    for x in e.children_named("B") {
        let Some(b) = usize::try_from(x.int_or("i", -1)).ok().and_then(|i| d.eq.get(i)) else { continue };
        get_f(x, "g", &b.gain_db);
        get_f(x, "f", &b.freq);
        get_f(x, "q", &b.q);
    }
}

/// The monitor paths, added to the `<Monitor>` element: the SPEAKERS path's
/// SOURCE, SRC LEVEL, SRC MIX and PFL DIM as attributes and its processing
/// as `<Dsp>`, the PHONES as `<Phones>` (with its `<Dsp>`).
pub fn capture_monitor_paths(engine: &Engine, monitor: &mut Element) {
    let c = &engine.controls;
    put_source(monitor, &c.speakers);
    put_dsp(monitor, &c.monitor.paths[0]);
    let p = monitor.add_new("Phones");
    put_source(p, &c.phones.source);
    put_f(p, "levelDb", &c.phones.level_db);
    put_f(p, "dimDb", &c.phones.dim_db);
    put_i(p, "outL", &c.phones.output_left);
    put_i(p, "outR", &c.phones.output_right);
    put_dsp(p, &c.monitor.paths[1]);
}

pub fn apply_monitor_paths(engine: &Engine, monitor: &Element) {
    let c = &engine.controls;
    get_source(monitor, &c.speakers);
    get_dsp(monitor, &c.monitor.paths[0]);
    if let Some(p) = monitor.child("Phones") {
        get_source(p, &c.phones.source);
        get_f(p, "levelDb", &c.phones.level_db);
        get_f(p, "dimDb", &c.phones.dim_db);
        get_i(p, "outL", &c.phones.output_left);
        get_i(p, "outR", &c.phones.output_right);
        get_dsp(p, &c.monitor.paths[1]);
    }
}

fn capture_talk(engine: &Engine, parent: &mut Element) {
    let t = &engine.controls.talk;
    let e = parent.add_new("Talk");
    put_i(e, "assignKind", &t.assign_kind);
    put_i(e, "assignIndex", &t.assign_index);
    put_f(e, "levelDb", &t.level_db);
    for (i, p) in t.paths.iter().enumerate() {
        let x = e.add_new("Path");
        x.set("i", i);
        put_b(x, "individual", &p.individual);
        put_i(x, "mode", &p.mode);
        put_f(x, "monDim", &p.mon_dim_db);
        put_f(x, "busDim", &p.bus_dim_db);
        put_u(x, "buses", &p.buses);
        put_u(x, "matrices", &p.matrices);
        put_u(x, "mains", &p.mains);
    }
}

fn apply_talk(engine: &Engine, parent: &Element) {
    let Some(e) = parent.child("Talk") else { return };
    let t = &engine.controls.talk;
    get_i(e, "assignKind", &t.assign_kind);
    get_i(e, "assignIndex", &t.assign_index);
    get_f(e, "levelDb", &t.level_db);
    for x in e.children_named("Path") {
        let Some(p) = usize::try_from(x.int_or("i", -1)).ok().and_then(|i| t.paths.get(i)) else { continue };
        get_b(x, "individual", &p.individual);
        get_i(x, "mode", &p.mode);
        get_f(x, "monDim", &p.mon_dim_db);
        get_f(x, "busDim", &p.bus_dim_db);
        get_u(x, "buses", &p.buses);
        get_u(x, "matrices", &p.matrices);
        get_u(x, "mains", &p.mains);
    }
}

/// Sources that differ from the defaults (or have a name).
fn capture_sources(engine: &Engine, parent: &mut Element) {
    let table = engine.sources();
    let e = parent.add_new("Sources");
    for (i, p) in table.params().iter().enumerate() {
        let name = table.name(i);
        if p.is_default() && name.is_empty() {
            continue;
        }
        let x = e.add_new("Source");
        x.set("i", i);
        if !name.is_empty() {
            x.set("name", name);
        }
        put_f(x, "gain", &p.gain_db);
        put_b(x, "mute", &p.mute);
        put_b(x, "inv", &p.invert);
        put_b(x, "phantom", &p.phantom);
        put_i(x, "mode", &p.mode);
        put_i(x, "colour", &p.colour);
        put_i(x, "icon", &p.icon);
        put_u(x, "mgrp", &p.mute_groups);
    }
    // The oscillators (the OSC group's generator settings).
    for (i, o) in table.oscillators().iter().enumerate() {
        if o.is_default() {
            continue;
        }
        let x = e.add_new("Osc");
        x.set("i", i);
        put_i(x, "mode", &o.mode);
        put_f(x, "level", &o.level_db);
        put_f(x, "freq", &o.freq_hz);
    }
}

fn apply_source(x: &Element, p: &SourceParams) {
    get_f(x, "gain", &p.gain_db);
    get_b(x, "mute", &p.mute);
    get_b(x, "inv", &p.invert);
    get_b(x, "phantom", &p.phantom);
    get_i(x, "colour", &p.colour);
    get_i(x, "icon", &p.icon);
    get_u(x, "mgrp", &p.mute_groups);
}

fn apply_sources(engine: &Engine, parent: &Element) {
    let Some(e) = parent.child("Sources") else { return };
    let table = engine.sources();
    for x in e.children_named("Source") {
        let Ok(i) = usize::try_from(x.int_or("i", -1)) else { continue };
        let Some(p) = table.get(i) else { continue };
        if let Some(name) = x.attr("name") {
            table.set_name(i, name);
        }
        apply_source(x, p);
        // The mode is stored on both sources of a pair; set it without copying
        // settings across (each source's own values were saved).
        let mode = x.int_or("mode", source_mode::MONO);
        p.mode.store(mode.clamp(0, 2), Relaxed);
    }
    for x in e.children_named("Osc") {
        let Some(o) = usize::try_from(x.int_or("i", -1)).ok().and_then(|i| table.osc(i)) else { continue };
        get_i(x, "mode", &o.mode);
        get_f(x, "level", &o.level_db);
        get_f(x, "freq", &o.freq_hz);
    }
}

/// Talk tags of the strips that have any.
fn capture_talk_tags(engine: &Engine, parent: &mut Element) {
    let e = parent.add_new("TalkTags");
    for kind in livedaw_engine::StripKind::ALL {
        for s in engine.strips(kind) {
            let tags = s.console.talk_tags.load(Relaxed);
            if tags != 0 {
                e.add_new("T").set("ref", StripRef::of(&s)).set("tags", tags);
            }
        }
    }
}

fn apply_talk_tags(engine: &Engine, parent: &Element) {
    let Some(e) = parent.child("TalkTags") else { return };
    for x in e.children_named("T") {
        let r = StripRef::parse(x.str_or("ref", ""));
        if let Some(s) = crate::console::find_strip(engine, &r) {
            s.console.talk_tags.store(x.int_or("tags", 0) as u32 & talk_tag::ALL, Relaxed);
        }
    }
}

/// Everything above, for show files (call after the strips exist).
pub fn capture(engine: &Engine, parent: &mut Element) {
    let c = &engine.controls;
    let solo = parent.add_new("Solo");
    put_i(solo, "mode", &c.solo_mode);
    put_i(solo, "bus", &c.solo_bus);
    put_b(solo, "muteOverride", &c.mute_override);
    put_i(solo, "srcSolo", &c.monitor.source_solo);
    capture_talk(engine, parent);
    capture_sources(engine, parent);
    capture_talk_tags(engine, parent);
}

/// Restores [`capture`]. Talk tags need the strips: call it after the
/// structure is built.
pub fn apply(engine: &Engine, parent: &Element) {
    let c = &engine.controls;
    if let Some(solo) = parent.child("Solo") {
        get_i(solo, "mode", &c.solo_mode);
        get_i(solo, "bus", &c.solo_bus);
        get_b(solo, "muteOverride", &c.mute_override);
        get_i(solo, "srcSolo", &c.monitor.source_solo);
    }
    apply_talk(engine, parent);
    apply_sources(engine, parent);
    apply_talk_tags(engine, parent);
}
