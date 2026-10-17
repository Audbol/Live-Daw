//! TAP WIDTH / TAP EQ and the RTA settings in shows and scenes.

use livedaw_engine::{Engine, StripKind};
use livedaw_session::console;
use livedaw_session::*;
use std::sync::atomic::Ordering::Relaxed;

fn engine_with_channels(n: usize) -> Engine {
    let e = Engine::new(0);
    e.set_num_channels(n);
    e
}

#[test]
fn tap_width_tap_eq_and_rta_settings_survive_a_show() {
    let engine = engine_with_channels(2);
    let ch = engine.channel_at(1).unwrap();
    ch.params.tap_width.store(-40.0);
    ch.params.tap_eq.on.store(true, Relaxed);
    ch.params.tap_eq.bands[1].gain_db.store(-6.5);
    ch.params.tap_eq.bands[2].freq.store(8000.0);
    ch.params.tap_eq.bands[0].q.store(2.0);
    let c = engine.analysis().controls();
    c.home.display.store(1, Relaxed);
    c.home.colour.store(3, Relaxed);
    c.eq.range.store(0, Relaxed);
    c.eq.detector.store(1, Relaxed);
    c.eq.autogain.store(false, Relaxed);
    c.eq.gain_db.store(24.0);
    c.filter_tap.store(0, Relaxed);
    c.band_solo_trim_db.store(6.0);

    let show = Show::new();
    let text = show.to_xml(&engine, &mut NoPlugins).to_document();
    assert!(text.contains("<Tap ") && text.contains("<Rta "));

    let e2 = engine_with_channels(0);
    let mut loaded = Show::new();
    loaded.load_from_text(&e2, &mut NoPlugins, &text, true).unwrap();
    let ch2 = e2.channel_at(1).unwrap();
    assert_eq!(ch2.params.tap_width.load(), -40.0);
    assert!(ch2.params.tap_eq.on.load(Relaxed));
    assert_eq!(ch2.params.tap_eq.bands[1].gain_db.load(), -6.5);
    assert_eq!(ch2.params.tap_eq.bands[2].freq.load(), 8000.0);
    assert_eq!(ch2.params.tap_eq.bands[0].q.load(), 2.0);
    let c2 = e2.analysis().controls();
    assert_eq!((c2.home.display.load(Relaxed), c2.home.colour.load(Relaxed)), (1, 3));
    assert_eq!((c2.eq.range.load(Relaxed), c2.eq.detector.load(Relaxed)), (0, 1));
    assert!(!c2.eq.autogain.load(Relaxed));
    assert_eq!(c2.eq.gain_db.load(), 24.0);
    assert_eq!(c2.filter_tap.load(Relaxed), 0);
    assert_eq!(c2.band_solo_trim_db.load(), 6.0);
}

#[test]
fn shows_from_before_load_with_the_defaults() {
    let engine = engine_with_channels(1);
    let show = Show::new();
    let mut xml = show.to_xml(&engine, &mut NoPlugins).to_document();
    // Strip what earlier versions didn't write.
    while let Some(start) = xml.find("<Tap ") {
        let end = start + xml[start..].find("</Tap>").unwrap() + "</Tap>".len();
        xml.replace_range(start..end, "");
    }
    let start = xml.find("<Rta ").unwrap();
    let end = start + xml[start..].find("</Rta>").unwrap() + "</Rta>".len();
    xml.replace_range(start..end, "");

    let e2 = engine_with_channels(0);
    e2.analysis().controls().home.range.store(0, Relaxed);
    let mut loaded = Show::new();
    loaded.load_from_text(&e2, &mut NoPlugins, &xml, true).unwrap();
    let ch = e2.channel_at(0).unwrap();
    assert_eq!(ch.params.tap_width.load(), 100.0);
    assert!(!ch.params.tap_eq.on.load(Relaxed));
    assert_eq!(e2.analysis().controls().home.range.load(Relaxed), 1, "a loaded show starts from the defaults");
}

#[test]
fn scenes_recall_tap_settings_with_the_sends() {
    let engine = engine_with_channels(1);
    let ch = engine.channel_at(0).unwrap();
    let all = console::capture(&engine, section::ALL, None, &mut NoPlugins);
    ch.params.tap_width.store(0.0);
    ch.params.tap_eq.on.store(true, Relaxed);
    ch.params.tap_eq.bands[0].gain_db.store(9.0);

    // Without SENDS in the scope nothing changes.
    console::apply(&engine, &all, section::ALL & !section::SENDS, None, true, None, &mut NoPlugins);
    assert_eq!(ch.params.tap_width.load(), 0.0);
    // With SENDS the captured values come back.
    console::apply(&engine, &all, section::SENDS, None, true, None, &mut NoPlugins);
    assert_eq!(ch.params.tap_width.load(), 100.0);
    assert!(!ch.params.tap_eq.on.load(Relaxed));
    assert_eq!(ch.params.tap_eq.bands[0].gain_db.load(), 0.0);

    // Buses have no TAP WIDTH / TAP EQ.
    let bus_state = console::capture(&engine, section::SENDS, Some(&[StripRef::new(StripKind::Bus, 0)]), &mut NoPlugins);
    assert!(!bus_state.to_compact().contains("<Tap"));
}
