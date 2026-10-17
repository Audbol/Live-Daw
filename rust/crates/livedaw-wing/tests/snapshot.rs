//! WING snapshot import (a fixture written to the documented format),
//! export, export -> import round trips, and show files.

use livedaw_engine::sends::send_mode;
use livedaw_engine::{Engine, StripKind, PLAYBACK_SOURCE_BASE};
use livedaw_session::console::NoPlugins;
use livedaw_session::Show;
use livedaw_wing::json::{self, Json};
use livedaw_wing::snap::{self, ImportOptions};
use livedaw_wing::WingState;
use std::path::PathBuf;
use std::sync::atomic::Ordering::Relaxed;

const FIXTURE: &str = include_str!("data/fixture.snap");

fn engine(channels: usize) -> Engine {
    let e = Engine::new(0);
    e.set_num_channels(channels);
    e
}

#[test]
fn imports_the_fixture() {
    let e = engine(2);
    let st = WingState::default();
    let r = snap::import_text(&e, &st, FIXTURE, &ImportOptions::default()).unwrap();
    assert_eq!(r.file_type, "snapshot.11");
    assert_eq!(r.channels_added, 2);
    assert!(r.applied > 250, "{}", r.details());
    let chs: Vec<_> = (0..4).map(|i| e.channel_at(i).unwrap()).collect();
    let ch = |n: usize| &chs[n - 1];
    let c1 = &ch(1).params;
    assert_eq!(ch(1).name(), "Kick");
    assert_eq!(c1.colour.load(Relaxed), 9);
    assert_eq!(c1.icon.load(Relaxed), 12);
    assert_eq!(c1.fader_db.load(), -4.5);
    assert_eq!(c1.pan.load(), -10.0);
    assert!(c1.input.invert.load(Relaxed));
    assert_eq!(c1.input.trim_db.load(), -3.5);
    assert_eq!(c1.input.delay_mode.load(Relaxed), 2);
    assert_eq!(c1.input.delay.load(), 2.5);
    assert_eq!(c1.input_left.load(Relaxed), 0); // A 1
    assert_eq!(c1.alt_input_left.load(Relaxed), PLAYBACK_SOURCE_BASE); // CRD 1
    assert!(c1.filter.low_cut.load(Relaxed));
    assert_eq!(c1.filter.low_cut_slope.load(Relaxed), 2);
    assert_eq!(c1.filter.tool_params[0].load(), 1.5);
    assert!(c1.gate.on.load(Relaxed));
    assert_eq!(c1.gate.param("thr").unwrap().load(), -35.0);
    assert_eq!(c1.gate.sc_filter.load(Relaxed), 2); // HP12
    assert_eq!(c1.eq.param("1g", false).unwrap().load(), -4.0);
    assert_eq!(c1.dynamics.param("thr").unwrap().load(), -18.0);
    assert_eq!(c1.dynamics.gain.load(), 2.0);
    assert_eq!(c1.dynamics.sc_source.load(Relaxed), 1); // CH.2
    assert!(c1.pre_insert.on.load(Relaxed));
    assert_eq!(c1.pre_insert.fx.load(Relaxed), 1);
    assert_eq!(c1.post_insert.fx.load(Relaxed), 0);
    assert!(c1.mains[0].on.load(Relaxed));
    let s1 = ch(1).sends.find(0).unwrap();
    assert!(s1.enabled.load(Relaxed) && s1.ignore_mute.load(Relaxed));
    assert_eq!(s1.level_db.load(), -6.0);
    let s2 = ch(1).sends.find(1).unwrap();
    assert_eq!(s2.mode.load(Relaxed), send_mode::POST);
    let s9 = ch(1).sends.find(8).unwrap();
    assert_eq!(s9.mode.load(Relaxed), send_mode::GROUP);
    assert!(s9.pan_reversed.load(Relaxed));
    assert!(c1.matrix[0].on.load(Relaxed) && c1.matrix[0].pre.load(Relaxed));
    assert_eq!(c1.matrix[0].pan.load(), 20.0);
    assert_eq!(c1.dca_mask.load(Relaxed), 1);
    assert_eq!(c1.mute_group_mask.load(Relaxed), 1);
    // Channel 2: models chosen by name, fader at -oo.
    let c2 = &ch(2).params;
    assert!(c2.mute.load(Relaxed));
    assert!(c2.fader_db.load() <= -100.0);
    assert_eq!(livedaw_dsp::wing::DYN_MODELS[c2.gate.model_index()].token, "E88");
    assert_eq!(livedaw_dsp::wing::DYN_MODELS[c2.dynamics.model_index()].token, "LA");
    assert_eq!(c2.dynamics.mix.load(), 80.0);
    // Channel 3: a stereo source (io/in/A/3 mode ST) applied after the patch.
    let c3 = &ch(3).params;
    assert!(c3.stereo.load(Relaxed));
    assert_eq!((c3.input_left.load(Relaxed), c3.input_right.load(Relaxed)), (2, 3));
    assert_eq!(c3.process_order.load(Relaxed), 8);
    assert_eq!(c3.tap.load(Relaxed), 7);
    // Aux from the WING-LIVE card = a playback channel.
    let aux = e.strip_at(StripKind::Aux, 0).unwrap();
    assert_eq!(aux.params.input_left.load(Relaxed), PLAYBACK_SOURCE_BASE + 40);
    // Buses, bus-to-bus sends, mains, matrices, outputs.
    let bus1 = e.strip_by_number(StripKind::Bus, 1).unwrap();
    assert!(bus1.params.mono.load(Relaxed));
    assert!(bus1.params.input.delay_on.load(Relaxed));
    assert_eq!(bus1.params.eq.param("3g", true).unwrap().load(), -6.0);
    assert!(bus1.params.matrix[1].pre.load(Relaxed));
    assert_eq!(bus1.params.output_left.load(Relaxed), 2); // A 3/4 = Bus 1
    let bus10 = e.strip_by_number(StripKind::Bus, 10).unwrap();
    assert_eq!(bus10.sends.find(1).unwrap().level_db.load(), -4.0);
    assert_eq!(bus10.sends.find(1).unwrap().mode.load(Relaxed), send_mode::POST);
    let main = e.strip_at(StripKind::Main, 0).unwrap();
    assert_eq!(main.name(), "PA");
    assert_eq!(main.params.fader_db.load(), -3.0);
    assert_eq!((main.params.output_left.load(Relaxed), main.params.output_right.load(Relaxed)), (0, 1));
    let mtx = e.strip_at(StripKind::Matrix, 0).unwrap();
    assert!(mtx.params.direct_on.load(Relaxed));
    assert_eq!(mtx.params.direct_level_db.load(), -10.0);
    // DCAs, mute groups, FX, console settings.
    assert_eq!(e.dca_name(0), "DRUMS");
    assert_eq!(e.controls.dcas[0].fader_db.load(), -2.0);
    assert!(e.controls.dcas[1].mute.load(Relaxed));
    assert_eq!(e.mute_group_name(1), "KEYS");
    assert_eq!(e.controls.mute_groups.load(Relaxed), 0b10);
    let fx1 = &e.controls.fx[0];
    assert_eq!(livedaw_dsp::wing::FX_MODELS[fx1.model_index()].token, "HALL");
    assert_eq!(fx1.param("dcy").unwrap().load(), 2.2);
    let fx2 = &e.controls.fx[1];
    assert_eq!(fx2.mix.load(), 80.0);
    assert_eq!(fx2.param("feed").unwrap().load(), 35.0);
    assert_eq!(e.controls.main_link.load(Relaxed), 1);
    assert!(e.controls.solo_afl.load(Relaxed) && e.controls.bus_solo_afl.load(Relaxed));
    assert!(!e.controls.main_solo_afl.load(Relaxed));
    assert_eq!(e.controls.monitor_level_db.load(), -6.0);
    assert_eq!(e.controls.monitor_dim_db.load(), 18.0);
    assert!(!e.controls.automix_enable[1].load(Relaxed));
    assert!(e.controls.startup_main_mute.load(Relaxed));
    // What wasn't applied is reported.
    assert!(r.kept_by_group.contains_key("/ch/N/peq"), "{}", r.details());
    assert!(r.kept_by_group.contains_key("/io/in/A"));
    assert!(r.unknown.iter().any(|u| u == "/cards/wlive/bogus"), "{:?}", r.unknown);
    assert!(r.errors.iter().any(|e| e.starts_with("/fx/9/mdl")), "{:?}", r.errors);
    assert!(r.details().contains("Not applied"));
}

fn without_created(j: &Json) -> Json {
    match j {
        Json::Obj(kv) => Json::Obj(kv.iter().filter(|(k, _)| k != "created").cloned().collect()),
        other => other.clone(),
    }
}

#[test]
fn export_follows_the_format_and_round_trips() {
    let e = engine(40);
    let st = WingState::default();
    snap::import_text(&e, &st, FIXTURE, &ImportOptions::default()).unwrap();
    let text = snap::export_text(&e, &st);
    let j = json::parse(&text).unwrap();
    assert_eq!(j.get("type").and_then(Json::as_str), Some("snapshot.11"));
    for key in ["ae_data", "ce_data", "ae_globals", "ce_globals"] {
        assert!(j.get(key).is_some(), "{key}");
    }
    let ae = j.get("ae_data").unwrap();
    let keys: Vec<&str> = ae.entries().iter().map(|(k, _)| k.as_str()).collect();
    assert_eq!(keys, ["cfg", "io", "ch", "aux", "bus", "main", "mtx", "dca", "mgrp", "fx", "cards", "play", "rec"]);
    let ch1 = ae.get("ch").unwrap().get("1").unwrap();
    assert_eq!(ae.get("ch").unwrap().entries().len(), 40);
    assert_eq!(ch1.get("name").and_then(Json::as_str), Some("Kick"));
    assert_eq!(ch1.get("fdr").and_then(Json::as_f64), Some(-4.5));
    assert_eq!(ch1.get("mute"), Some(&Json::Bool(false)));
    assert_eq!(ch1.get("in").unwrap().get("conn").unwrap().get("grp").and_then(Json::as_str), Some("A"));
    // The model's parameters follow mdl, read-only and $ nodes aren't saved.
    let gate: Vec<&str> = ch1.get("gate").unwrap().entries().iter().map(|(k, _)| k.as_str()).collect();
    assert_eq!(&gate[..5], ["on", "mdl", "mix", "gain", "thr"]); // as consoles save them
    assert!(!gate.iter().any(|k| k.starts_with('$')));
    assert!(ch1.get("$solo").is_none() && ch1.get("$name").is_none());
    assert_eq!(ae.get("ch").unwrap().get("2").unwrap().get("fdr").and_then(Json::as_f64), Some(-144.0));
    assert_eq!(ch1.get("tags").and_then(Json::as_str), Some("#D1,#M1,kick"));
    // Kept values come back out.
    assert_eq!(ch1.get("peq").unwrap().get("1g").and_then(Json::as_f64), Some(2.0));
    assert_eq!(ae.get("fx").unwrap().get("1").unwrap().get("dcy").and_then(Json::as_f64), Some(2.2));

    // Export -> import into a fresh console -> export: the same file.
    let e2 = engine(40);
    let st2 = WingState::default();
    let r = snap::import_text(&e2, &st2, &text, &ImportOptions::default()).unwrap();
    assert_eq!(r.error_count, 0, "{}", r.details());
    assert_eq!(r.unknown_count, 0, "{}", r.details());
    let again = json::parse(&snap::export_text(&e2, &st2)).unwrap();
    assert_eq!(without_created(&again), without_created(&j));
}

fn temp_dir(name: &str) -> PathBuf {
    let d = std::env::temp_dir().join(format!("livedaw-wing-{name}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(d.join("_scenes")).unwrap();
    d
}

#[test]
fn imports_a_show_as_scenes() {
    let dir = temp_dir("show");
    std::fs::write(dir.join("_scenes/One.snap"), FIXTURE).unwrap();
    let two = FIXTURE.replace("\"fdr\": -4.5", "\"fdr\": -20").replace("\"name\": \"Kick\"", "\"name\": \"Kick 2\"");
    std::fs::write(dir.join("_scenes/Two.snap"), two).unwrap();
    let show_text = r##"{
	"type": "showfile.1",
	"creator_fw": "3.1-0-gfixture:release",
	"creator_model": "WING-EDIT",
	"created": "2026-10-01 18:00:00",
	"scenes": {
		"count": 4,
		"1": {"name": "Opener", "skip": false, "link": false, "type": "SNAP", "info": "walk-in", "tag": "#3", "midi_tx": "", "file": "_scenes/One.snap"},
		"2": {"name": "Band", "skip": true, "link": false, "type": "SNAP", "info": "", "tag": "", "midi_tx": "", "file": "I:/library/SHOWS/x/_scenes/Two.snap"},
		"3": {"name": "Mutes", "skip": false, "link": false, "type": "SNIP", "info": "", "tag": "", "midi_tx": "", "file": "_scenes/Mutes.snip"},
		"4": {"name": "Gone", "skip": false, "link": false, "type": "SNAP", "info": "", "tag": "", "midi_tx": "", "file": "_scenes/Missing.snap"}
	}
}"##;
    std::fs::write(dir.join("Test.show"), show_text).unwrap();

    let e = engine(40);
    e.channel_at(0).unwrap().params.fader_db.store(5.0);
    let st = WingState::default();
    let mut show = Show::new();
    let r = snap::import_show(&mut show, &e, &st, &dir.join("Test.show")).unwrap();
    assert_eq!(r.scenes_added, 2, "{}", r.summary());
    assert_eq!(r.skipped.len(), 2);
    // The live console didn't change.
    assert_eq!(e.channel_at(0).unwrap().params.fader_db.load(), 5.0);
    let s1 = show.scene(0).unwrap();
    assert_eq!((s1.name.as_str(), s1.tag, s1.notes.as_str()), ("Opener", 3, "walk-in"));
    assert!(show.scene(1).unwrap().skip);
    // Recalling the scenes sets the console.
    show.scene_mut(1).unwrap().skip = false;
    show.recall_scene(&e, &mut NoPlugins, 1);
    assert_eq!(e.channel_at(0).unwrap().params.fader_db.load(), -20.0);
    assert_eq!(e.channel_at(0).unwrap().name(), "Kick 2");
    show.recall_scene(&e, &mut NoPlugins, 0);
    assert_eq!(e.channel_at(0).unwrap().params.fader_db.load(), -4.5);
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn rejects_files_that_are_not_snapshots() {
    let e = engine(1);
    let st = WingState::default();
    assert!(snap::import_text(&e, &st, "{\"type\":\"other\"}", &ImportOptions::default()).is_err());
    assert!(snap::import_text(&e, &st, "not json", &ImportOptions::default()).is_err());
    assert!(snap::parse_show("{}").is_err());
}
