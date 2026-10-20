//! Console-level state in shows and scenes: solo mode, override, talkback,
//! talk tags, the monitor paths, sources and MON DCAs.

use livedaw_engine::sources::source_mode;
use livedaw_engine::talk::talk_tag;
use livedaw_engine::{Engine, StripKind};
use livedaw_session::console::{self, find_strip, section};
use livedaw_session::xml;
use livedaw_session::{NoPlugins, StripRef};
use std::sync::atomic::Ordering::Relaxed;

/// Saves the console, parses the XML back and loads it into a fresh engine.
fn round_trip(engine: &Engine) -> Engine {
    let text = console::save_console(engine, &mut NoPlugins).to_document();
    let parsed = xml::parse(&text).unwrap();
    let other = Engine::new(0);
    let warnings = console::load_console(&other, &parsed, &mut NoPlugins, false);
    assert!(warnings.is_empty(), "{warnings:?}");
    other
}

#[test]
fn console_level_state_survives_a_show_round_trip() {
    let engine = Engine::new(0);
    engine.set_num_channels(40);
    let c = &engine.controls;
    c.solo_mode.store(2, Relaxed);
    c.solo_bus.store(1, Relaxed);
    c.mute_override.store(true, Relaxed);

    let t = &c.talk;
    t.assign_kind.store(StripKind::Channel as i32, Relaxed);
    t.assign_index.store(39, Relaxed);
    t.level_db.store(-3.5);
    t.paths[0].buses.store(0b1111, Relaxed);
    t.paths[0].individual.store(true, Relaxed);
    t.paths[0].mon_dim_db.store(12.0);
    t.paths[1].mode.store(2, Relaxed);
    t.paths[1].matrices.store(0b1000_0001, Relaxed);
    t.paths[1].mains.store(0b10, Relaxed);
    t.paths[1].bus_dim_db.store(3.0);
    t.paths[1].on.store(true, Relaxed); // a key state: not saved

    c.speakers.source.store(4, Relaxed);
    c.speakers.pfl_dim_db.store(6.0);
    c.phones.source.source.store(30, Relaxed);
    c.phones.source.source_mix_db.store(-20.0);
    c.phones.level_db.store(-12.0);
    c.phones.dim_db.store(30.0);
    c.phones.output_left.store(6, Relaxed);
    c.phones.output_right.store(7, Relaxed);

    let src = engine.sources();
    src.params()[0].gain_db.store(24.0);
    src.params()[0].phantom.store(true, Relaxed);
    src.params()[0].colour.store(9, Relaxed);
    src.params()[0].icon.store(12, Relaxed);
    src.set_name(0, "Kick In");
    src.set_mode(2, source_mode::STEREO);
    src.params()[2].invert.store(true, Relaxed);
    let osc = src.osc(1).unwrap();
    osc.mode.store(2, Relaxed);
    osc.level_db.store(-30.0);
    osc.freq_hz.store(63.0);
    src.params()[3].invert.store(true, Relaxed);
    src.set_mode(600, source_mode::MID_SIDE);
    src.params()[601].mute.store(true, Relaxed);

    engine.channel_at(3).unwrap().console.talk_tags.store(talk_tag::A_ON | talk_tag::SOLO, Relaxed);
    engine.strip_at(StripKind::Aux, 7).unwrap().console.talk_tags.store(talk_tag::B_SOLO, Relaxed);
    engine.set_dca_name(4, "MON A");

    let other = round_trip(&engine);
    let o = &other.controls;
    assert_eq!(o.solo_mode.load(Relaxed), 2);
    assert_eq!(o.solo_bus.load(Relaxed), 1);
    assert!(o.mute_override.load(Relaxed));

    let ot = &o.talk;
    assert_eq!(ot.assignment(), Some((StripKind::Channel, 39)));
    assert_eq!(ot.level_db.load(), -3.5);
    assert_eq!(ot.paths[0].buses.load(Relaxed), 0b1111);
    assert!(ot.paths[0].individual.load(Relaxed));
    assert_eq!(ot.paths[0].mon_dim_db.load(), 12.0);
    assert_eq!(ot.paths[1].mode.load(Relaxed), 2);
    assert_eq!(ot.paths[1].matrices.load(Relaxed), 0b1000_0001);
    assert_eq!(ot.paths[1].mains.load(Relaxed), 0b10);
    assert_eq!(ot.paths[1].bus_dim_db.load(), 3.0);
    assert!(!ot.paths[1].on.load(Relaxed), "talk keys start off");

    assert_eq!(o.speakers.source.load(Relaxed), 4);
    assert_eq!(o.speakers.pfl_dim_db.load(), 6.0);
    assert_eq!(o.phones.source.source.load(Relaxed), 30);
    assert_eq!(o.phones.source.source_mix_db.load(), -20.0);
    assert_eq!(o.phones.level_db.load(), -12.0);
    assert_eq!(o.phones.dim_db.load(), 30.0);
    assert_eq!((o.phones.output_left.load(Relaxed), o.phones.output_right.load(Relaxed)), (6, 7));

    let os = other.sources();
    let p = os.params();
    assert_eq!(p[0].gain_db.load(), 24.0);
    assert!(p[0].phantom.load(Relaxed));
    assert_eq!((p[0].colour.load(Relaxed), p[0].icon.load(Relaxed)), (9, 12));
    assert_eq!(os.name(0), "Kick In");
    assert_eq!((p[2].mode.load(Relaxed), p[3].mode.load(Relaxed)), (source_mode::STEREO, source_mode::STEREO));
    assert!(p[2].invert.load(Relaxed) && p[3].invert.load(Relaxed));
    assert_eq!(p[601].mode.load(Relaxed), source_mode::MID_SIDE);
    assert!(p[601].mute.load(Relaxed));
    assert!(p[1].is_default() && p[599].is_default());
    let osc = os.osc(1).unwrap();
    assert_eq!((osc.mode.load(Relaxed), osc.level_db.load(), osc.freq_hz.load()), (2, -30.0, 63.0));
    assert!(os.osc(0).unwrap().is_default());

    let tags = |kind, id| find_strip(&other, &StripRef::new(kind, id)).unwrap().console.talk_tags.load(Relaxed);
    let ch4 = engine.channel_at(3).unwrap().id();
    assert_eq!(tags(StripKind::Channel, ch4), talk_tag::A_ON | talk_tag::SOLO);
    assert_eq!(tags(StripKind::Aux, 7), talk_tag::B_SOLO);
    assert_eq!(tags(StripKind::Channel, 1), 0);
    assert_eq!(other.dca_monitor_paths(4), 2, "MON DCAs follow their names (MON A: phones)");

    // Older shows (no console-level elements) load the defaults.
    let bare = xml::parse(r#"<Console version="3"><Channels/></Console>"#).unwrap();
    console::load_console(&other, &bare, &mut NoPlugins, false);
    assert_eq!(other.controls.solo_mode.load(Relaxed), 1);
    assert_eq!(other.controls.talk.assignment(), None);
    assert!(other.sources().params()[0].is_default());
    assert!(other.sources().osc(1).unwrap().is_default());
    assert_eq!(other.controls.phones.output_left.load(Relaxed), -1);
}

#[test]
fn scenes_recall_the_monitor_paths_with_routing() {
    let engine = Engine::new(0);
    let c = &engine.controls;
    c.phones.level_db.store(-9.0);
    c.speakers.source.store(12, Relaxed);
    let state = console::capture(&engine, section::ALL, None, &mut NoPlugins);
    c.phones.level_db.store(0.0);
    c.speakers.source.store(0, Relaxed);
    console::apply(&engine, &state, section::FADER, None, false, None, &mut NoPlugins);
    assert_eq!(c.phones.level_db.load(), 0.0, "not in the scope");
    console::apply(&engine, &state, section::ROUTING, None, false, None, &mut NoPlugins);
    assert_eq!(c.phones.level_db.load(), -9.0);
    assert_eq!(c.speakers.source.load(Relaxed), 12);
}

#[test]
fn monitor_processing_source_solo_and_source_tags_survive_a_show() {
    let engine = Engine::new(0);
    engine.set_num_channels(40);
    let m = &engine.controls.monitor;
    for (i, p) in m.paths.iter().enumerate() {
        let k = i as f32;
        p.direct_source.store(45 + i as i32, Relaxed);
        p.direct_level_db.store(-6.0 - k);
        p.band_solo_db.store(9.0 + k);
        p.eq_on.store(true, Relaxed);
        p.eq[0].gain_db.store(3.0 + k);
        p.eq[4].freq.store(1800.0 + k);
        p.eq[6].q.store(2.5 + k);
        p.pan.store(-20.0 + k);
        p.width.store(80.0 - k);
        p.invert.store(i == 1, Relaxed);
        p.delay_on.store(true, Relaxed);
        p.delay_m.store(12.5 + k);
        p.limiter_db.store(-6.0 - k);
    }
    m.source_solo.store(2, Relaxed);
    m.source_solo_on.store(true, Relaxed); // a key state: not saved
    engine.sources().params()[5].mute_groups.store(0b1001, Relaxed);

    let other = round_trip(&engine);
    let o = &other.controls.monitor;
    for (i, p) in o.paths.iter().enumerate() {
        let k = i as f32;
        assert_eq!(p.direct_source.load(Relaxed), 45 + i as i32);
        assert_eq!(p.direct_level_db.load(), -6.0 - k);
        assert_eq!(p.band_solo_db.load(), 9.0 + k);
        assert!(p.eq_on.load(Relaxed));
        assert_eq!(p.eq[0].gain_db.load(), 3.0 + k);
        assert_eq!(p.eq[4].freq.load(), 1800.0 + k);
        assert_eq!(p.eq[6].q.load(), 2.5 + k);
        assert_eq!((p.pan.load(), p.width.load()), (-20.0 + k, 80.0 - k));
        assert_eq!(p.invert.load(Relaxed), i == 1);
        assert!(p.delay_on.load(Relaxed));
        assert_eq!(p.delay_m.load(), 12.5 + k);
        assert_eq!(p.limiter_db.load(), -6.0 - k);
    }
    assert_eq!(o.source_solo.load(Relaxed), 2);
    assert!(!o.source_solo_on.load(Relaxed), "a source solo isn't saved");
    assert_eq!(other.sources().params()[5].mute_groups.load(Relaxed), 0b1001);

    // Scenes recall the paths' processing with ROUTING.
    let state = console::capture(&other, section::ALL, None, &mut NoPlugins);
    o.paths[1].limiter_db.store(0.0);
    o.paths[0].eq_on.store(false, Relaxed);
    console::apply(&other, &state, section::FADER, None, false, None, &mut NoPlugins);
    assert_eq!(o.paths[1].limiter_db.load(), 0.0, "not in the scope");
    console::apply(&other, &state, section::ROUTING, None, false, None, &mut NoPlugins);
    assert_eq!(o.paths[1].limiter_db.load(), -7.0);
    assert!(o.paths[0].eq_on.load(Relaxed));

    // A show from before (one BAND SOLO trim with the RTA settings, no <Dsp>).
    let old = xml::parse(r#"<Console version="3"><Channels/><Rta bandSoloTrim="5"/></Console>"#).unwrap();
    console::load_console(&other, &old, &mut NoPlugins, false);
    assert_eq!(o.paths[0].band_solo_db.load(), 5.0);
    assert_eq!(o.paths[1].band_solo_db.load(), 5.0);
    assert!(!o.paths[1].eq_on.load(Relaxed), "the defaults");
    assert_eq!(o.source_solo.load(Relaxed), 0);
}
