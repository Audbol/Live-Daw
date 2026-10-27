//! The routing outside the strips in shows and scenes: sources patched to
//! outputs, USER SIGNALs and USER PATCHes, buses as channel sources, sends
//! from any bus to any bus, a matrix's DIRECT IN, and how older shows map.

use livedaw_engine::routing::{matrix_direct, strip_source_input};
use livedaw_engine::sources::{source_mode, USR_SOURCE_BASE, USR_SOURCE_INDEX};
use livedaw_engine::{Engine, StripKind};
use livedaw_session::console::section;
use livedaw_session::*;
use std::sync::atomic::Ordering::Relaxed;

fn round_trip(engine: &Engine) -> (Engine, Vec<String>) {
    let text = Show::new().to_xml(engine, &mut NoPlugins).to_document();
    let e2 = Engine::new(0);
    let warnings = Show::new().load_from_text(&e2, &mut NoPlugins, &text, true).unwrap();
    (e2, warnings)
}

fn routed(engine: &Engine) {
    engine.set_num_channels(3);
    engine.route_output_source(6, 2);
    engine.route_output_source(40, USR_SOURCE_BASE + 1);
    let t = engine.sources();
    let u = &t.user_signals()[1];
    u.set_strip(Some((StripKind::Bus, 12)));
    u.side.store(2, Relaxed);
    u.post.store(true, Relaxed);
    t.set_user_patch(5, 17);
    t.set_user_patch(4, 0x10000 + 3); // playback channel 4
    engine.set_source_mode(USR_SOURCE_INDEX + 24 + 4, source_mode::MID_SIDE);
    t.get(USR_SOURCE_INDEX + 1).unwrap().invert.store(true, Relaxed);
    t.set_name(USR_SOURCE_INDEX + 1, "B12 R");
    let ch = engine.channel_at(1).unwrap();
    ch.params.input_left.store(strip_source_input(StripKind::Main, 2, 0), Relaxed);
    ch.params.alt_input_left.store(USR_SOURCE_BASE + 29, Relaxed);
    // Bus 2 -> Bus 14 and Bus 16 -> Bus 1.
    let b2 = engine.bus_in_slot(1).unwrap();
    let send = b2.sends.get_or_create(13).unwrap();
    send.enabled.store(true, Relaxed);
    send.level_db.store(-7.0);
    engine.bus_in_slot(15).unwrap().sends.get_or_create(0).unwrap().enabled.store(true, Relaxed);
    let m = engine.strip_at(StripKind::Matrix, 5).unwrap();
    m.params.direct_on.store(true, Relaxed);
    m.params.direct_source.store(matrix_direct::SOLO_BUS, Relaxed);
    m.params.direct_level_db.store(-3.0);
}

fn check_routed(e: &Engine) {
    assert_eq!(e.output_patch().get(6), 2);
    assert_eq!(e.output_patch().get(40), USR_SOURCE_BASE + 1);
    assert_eq!(e.output_patch().patched().count(), 2);
    let t = e.sources();
    let u = &t.user_signals()[1];
    assert_eq!(u.strip(), Some((StripKind::Bus, 12)));
    assert!(u.post.load(Relaxed) && u.side.load(Relaxed) == 2);
    assert!(t.user_signals()[0].strip().is_none());
    assert_eq!(t.user_patches()[5].source.load(Relaxed), 17);
    assert_eq!(t.user_patches()[4].source.load(Relaxed), 0x10003);
    assert_eq!(t.get(USR_SOURCE_INDEX + 28).unwrap().mode.load(Relaxed), source_mode::MID_SIDE);
    assert!(t.get(USR_SOURCE_INDEX + 1).unwrap().invert.load(Relaxed));
    assert_eq!(t.name(USR_SOURCE_INDEX + 1), "B12 R");
    let ch = e.channel_at(1).unwrap();
    assert_eq!(ch.params.input_left.load(Relaxed), strip_source_input(StripKind::Main, 2, 0));
    assert_eq!(ch.params.alt_input_left.load(Relaxed), USR_SOURCE_BASE + 29);
    let b2 = e.bus_in_slot(1).unwrap();
    let send = b2.sends.find(13).unwrap();
    assert!(send.enabled.load(Relaxed) && send.level_db.load() == -7.0);
    assert!(e.bus_in_slot(15).unwrap().sends.find(0).unwrap().enabled.load(Relaxed));
    let m = e.strip_at(StripKind::Matrix, 5).unwrap();
    assert!(m.params.direct_on.load(Relaxed));
    assert_eq!(m.params.direct_source.load(Relaxed), matrix_direct::SOLO_BUS);
    assert_eq!(m.params.direct_level_db.load(), -3.0);
}

#[test]
fn routing_survives_a_show_round_trip() {
    let engine = Engine::new(0);
    routed(&engine);
    let (e2, warnings) = round_trip(&engine);
    assert!(warnings.is_empty(), "{warnings:?}");
    check_routed(&e2);
}

#[test]
fn scenes_recall_the_routing_with_routing() {
    let engine = Engine::new(0);
    let mut np = NoPlugins;
    routed(&engine);
    let mut show = Show::new();
    show.add_scene(&engine, &mut np, "A", -1);
    // Change everything, then recall ROUTING (+ SENDS for the bus sends and DIRECT IN).
    engine.clear_output(6);
    engine.route_output_source(7, 3);
    engine.sources().user_signals()[1].set_strip(None);
    engine.sources().user_signals()[9].set_strip(Some((StripKind::Channel, 2)));
    engine.sources().set_user_patch(5, -1);
    engine.channel_at(1).unwrap().params.input_left.store(0, Relaxed);
    engine.bus_in_slot(1).unwrap().sends.find(13).unwrap().enabled.store(false, Relaxed);
    engine.strip_at(StripKind::Matrix, 5).unwrap().params.direct_source.store(matrix_direct::OFF, Relaxed);
    show.scene_mut(0).unwrap().scope = section::ROUTING | section::SENDS;
    show.recall_scene(&engine, &mut np, 0);
    assert_eq!(engine.output_patch().get(7), -1, "an output the scene doesn't list is OFF");
    assert!(engine.sources().user_signals()[9].strip().is_none());
    check_routed(&engine);

    // Another scope leaves the routing alone.
    engine.route_output_source(7, 3);
    show.scene_mut(0).unwrap().scope = section::FADER;
    show.recall_scene(&engine, &mut np, 0);
    assert_eq!(engine.output_patch().get(7), 3);
}

/// Shows from before: no `<Patch>` (nothing routed straight to outputs, the
/// USR group OFF), and a matrix's direct input from a channel becomes that
/// channel's matrix send, as the WING sends channels to matrices.
#[test]
fn older_shows_load_with_their_routing_mapped() {
    let engine = Engine::new(0);
    engine.set_num_channels(4);
    let a = engine.channel_at(2).unwrap();
    a.params.matrix[1].level_db.store(-20.0);
    let text = Show::new().to_xml(&engine, &mut NoPlugins).to_document();
    assert!(text.contains("<Patch"));
    // As Sidecar 0.1.5 wrote them: no Patch, `src` / `pre` on the direct input.
    let start = text.find("<Patch").unwrap();
    let end = start + text[start..].find("/>").unwrap() + 2;
    let mut old = text.clone();
    old.replace_range(start..end, "");
    let mtx2 = old.find("ref=\"mtx:1\"").unwrap();
    let dir = mtx2 + old[mtx2..].find("<Dir ").unwrap();
    let dir_end = dir + old[dir..].find("/>").unwrap() + 2;
    old.replace_range(dir..dir_end, r#"<Dir on="1" inv="0" pre="0" src="2" lvl="-6"/>"#);
    let mtx3 = old.find("ref=\"mtx:2\"").unwrap();
    let dir = mtx3 + old[mtx3..].find("<Dir ").unwrap();
    let dir_end = dir + old[dir..].find("/>").unwrap() + 2;
    old.replace_range(dir..dir_end, r#"<Dir on="1" inv="1" pre="1" src="0" lvl="0"/>"#);
    assert_eq!(old.matches(" in=\"OFF\"").count(), 6, "the other matrices' DIRECT IN");

    let e2 = Engine::new(0);
    e2.route_output_source(3, 1); // whatever the engine had before is reset
    let warnings = Show::new().load_from_text(&e2, &mut NoPlugins, &old, true).unwrap();
    assert_eq!(e2.output_patch().patched().count(), 0);
    assert!(e2.sources().user_signals().iter().all(|u| u.strip().is_none()));
    // MTX 2 from CH 3: CH 3's matrix send 2, on at -6 dB, post-fader with PAN LINK.
    let m2 = e2.strip_at(StripKind::Matrix, 1).unwrap();
    assert!(!m2.params.direct_on.load(Relaxed));
    assert_eq!(m2.params.direct_source.load(Relaxed), matrix_direct::OFF);
    let send = &e2.channel_at(2).unwrap().params.matrix[1];
    assert!(send.on.load(Relaxed) && !send.pre.load(Relaxed) && send.pan_link.load(Relaxed));
    assert_eq!(send.level_db.load(), -6.0);
    // MTX 3 from CH 1, inverted: CH 1's send 3 (PRE), and a warning about the polarity.
    let send = &e2.channel_at(0).unwrap().params.matrix[2];
    assert!(send.on.load(Relaxed) && send.pre.load(Relaxed));
    assert!(!e2.strip_at(StripKind::Matrix, 2).unwrap().params.direct_invert.load(Relaxed));
    assert_eq!(warnings.len(), 1, "{warnings:?}");
    assert!(warnings[0].contains("MTX 3") && warnings[0].contains("CH 1"), "{warnings:?}");
}
