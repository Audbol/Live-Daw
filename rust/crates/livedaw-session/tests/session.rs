//! Show / scene / snippet / console-state tests against a real engine (no audio device).

use livedaw_dsp::wing::{model_index, DYN_MODELS};
use livedaw_engine::{Engine, InsertCallbacks, Strip, StripKind};
use livedaw_session::console::{self, find_strip};
use livedaw_session::layers::{CENTER, LEFT};
use livedaw_session::xml::{self, Element};
use livedaw_session::*;
use std::collections::HashMap;
use std::ffi::c_void;
use std::sync::atomic::Ordering::Relaxed;
use std::sync::Arc;
use std::time::{Duration, Instant};

unsafe extern "C" fn pass_through(_: *mut c_void, _: *const *mut f32, _: i32, _: i32) -> i32 {
    0
}

/// Pretend plugin host: a "plugin" is a name and a state string per FX slot.
#[derive(Default)]
struct FakeHost {
    plugins: HashMap<usize, (String, String)>,
    fail_to_create: Vec<String>,
}

impl FakeHost {
    fn load(&mut self, engine: &Engine, slot: usize, name: &str, state: &str) {
        let cb = InsertCallbacks { process: pass_through, release: None, ctx: std::ptr::null_mut() };
        engine.set_fx_plugin(slot, Some(cb));
        self.plugins.insert(slot, (name.into(), state.into()));
    }
}

struct Bridge<'a> {
    host: &'a mut FakeHost,
    engine: &'a Engine,
}

impl PluginBridge for Bridge<'_> {
    fn capture_fx(&mut self, slot: usize) -> Option<CapturedInsert> {
        let (name, state) = self.host.plugins.get(&slot)?;
        let mut desc = Element::new("PLUGIN");
        desc.set("name", name).set("format", "Fake");
        Some(CapturedInsert { description: desc.to_compact(), state: state.clone() })
    }

    fn restore_fx(&mut self, slot: usize, mode: InsertMode, description: &str, state: &str) -> Result<(), RestoreError> {
        let desc = xml::parse(description).map_err(|e| RestoreError::Failed(e.to_string()))?;
        let name = desc.str_or("name", "").to_owned();
        match mode {
            InsertMode::StateOnly => match self.host.plugins.get_mut(&slot) {
                Some((n, s)) if *n == name => {
                    *s = state.into();
                    Ok(())
                }
                _ => Err(RestoreError::Mismatch),
            },
            InsertMode::Instantiate => {
                if self.host.fail_to_create.contains(&name) {
                    return Err(RestoreError::Failed("not installed".into()));
                }
                self.host.load(self.engine, slot, &name, state);
                Ok(())
            }
        }
    }

    fn clear_fx(&mut self, slot: usize) {
        self.host.plugins.remove(&slot);
        self.engine.set_fx_plugin(slot, None);
    }
}

fn ch(engine: &Engine, i: usize) -> Arc<Strip> {
    engine.channel_at(i).unwrap()
}

fn dyn_param(s: &Strip, id: &str) -> f32 {
    s.params.dynamics.param(id).unwrap().load()
}

#[test]
fn console_survives_a_show_round_trip() {
    let engine = Engine::new(0);
    let mut host = FakeHost::default();
    engine.set_num_channels(3);
    let big_bus = engine.add_bus(79).unwrap();

    let kick = ch(&engine, 0);
    kick.set_name("Kick \"in\" & <out>");
    let p = &kick.params;
    p.colour.store(9, Relaxed);
    p.icon.store(42, Relaxed);
    p.filter.low_cut.store(true, Relaxed);
    p.filter.low_cut_freq.store(95.0);
    p.gate.on.store(true, Relaxed);
    p.gate.param("thr").unwrap().store(-33.0);
    p.eq.param("2g", false).unwrap().store(-4.5);
    p.dynamics.set_model(model_index(DYN_MODELS, "B560").unwrap());
    p.dynamics.param("thr").unwrap().store(-12.0);
    p.process_order.store(5, Relaxed);
    p.stereo.store(true, Relaxed);
    p.recall_safe.store(true, Relaxed);
    p.mains[1].on.store(true, Relaxed);
    p.matrix[3].level_db.store(-7.0);
    p.dca_mask.store(0b100, Relaxed);
    p.mute_group_mask.store(0b10, Relaxed);
    kick.set_tags("#Drums,Band \"A\"");
    let send = kick.sends.get_or_create(79).unwrap();
    send.enabled.store(true, Relaxed);
    send.level_db.store(-3.0);
    send.mode.store(2, Relaxed);
    p.pre_insert.on.store(true, Relaxed);
    p.pre_insert.fx.store(2, Relaxed);
    host.load(&engine, 2, "Reverb", "ABC=");
    engine.controls.fx[2].bypass.store(true, Relaxed);
    engine.set_dca_name(2, "Drums");
    engine.controls.dcas[2].fader_db.store(-4.0);
    engine.controls.mute_groups.store(0b1, Relaxed);
    engine.set_mute_group_name(0, "Band");
    engine.main(0).unwrap().params.fader_db.store(-2.0);
    engine.strip_at(StripKind::Matrix, 1).unwrap().params.direct_source.store(2, Relaxed);
    engine.controls.solo_afl.store(true, Relaxed);
    engine.controls.output_ceiling_db.store(-1.5);

    let mut show = Show::new();
    show.name = "Gig".into();
    show.layers.add_user_layer("Drums", vec![StripRef::of(&kick).into(), StripRef::of(&big_bus).into(), FaderRef::Dca(2)]);
    show.layers.select(CENTER, "dca-1");
    show.add_scene(&engine, &mut Bridge { host: &mut host, engine: &engine }, "Verse", -1);
    let text = show.to_xml(&engine, &mut Bridge { host: &mut host, engine: &engine }).to_document();

    // Load into a fresh engine and host.
    let e2 = Engine::new(0);
    let mut host2 = FakeHost::default();
    let mut loaded = Show::new();
    let warnings = loaded.load_from_text(&e2, &mut Bridge { host: &mut host2, engine: &e2 }, &text, true).unwrap();
    assert!(warnings.is_empty(), "{warnings:?}");

    assert_eq!(loaded.name, "Gig");
    assert_eq!(loaded.num_scenes(), 1);
    assert_eq!(loaded.current_scene(), 0);
    assert_eq!(loaded.layers.user_layers()[0].faders.len(), 3);
    assert_eq!(loaded.layers.selected(CENTER), "dca-1");
    assert_eq!(e2.num_channels(), 3);
    assert_eq!(e2.num_strips(StripKind::Bus), 17);
    assert!(e2.bus_in_slot(79).is_some());

    let k = ch(&e2, 0);
    let p = &k.params;
    assert_eq!(k.name(), "Kick \"in\" & <out>");
    assert_eq!((p.colour.load(Relaxed), p.icon.load(Relaxed)), (9, 42));
    assert!(p.filter.low_cut.load(Relaxed));
    assert_eq!(p.filter.low_cut_freq.load(), 95.0);
    assert!(p.gate.on.load(Relaxed));
    assert_eq!(p.gate.param("thr").unwrap().load(), -33.0);
    assert_eq!(p.eq.param("2g", false).unwrap().load(), -4.5);
    assert_eq!(DYN_MODELS[p.dynamics.model_index()].token, "B560");
    assert_eq!(dyn_param(&k, "thr"), -12.0);
    assert_eq!(p.process_order.load(Relaxed), 5);
    assert!(p.stereo.load(Relaxed));
    assert!(p.recall_safe.load(Relaxed));
    assert!(p.mains[0].on.load(Relaxed) && p.mains[1].on.load(Relaxed));
    assert_eq!(p.matrix[3].level_db.load(), -7.0);
    assert_eq!((p.dca_mask.load(Relaxed), p.mute_group_mask.load(Relaxed)), (0b100, 0b10));
    assert_eq!(k.tags(), "#Drums,Band \"A\"");
    let send = k.sends.find(79).unwrap();
    assert!(send.enabled.load(Relaxed));
    assert_eq!((send.level_db.load(), send.mode.load(Relaxed)), (-3.0, 2));
    assert_eq!((p.pre_insert.on.load(Relaxed), p.pre_insert.fx.load(Relaxed)), (true, 2));
    assert_eq!(host2.plugins.get(&2), Some(&("Reverb".to_string(), "ABC=".to_string())));
    assert!(e2.controls.fx[2].bypass.load(Relaxed));
    assert_eq!(e2.dca_name(2), "Drums");
    assert_eq!(e2.controls.dcas[2].fader_db.load(), -4.0);
    assert_eq!(e2.controls.mute_groups.load(Relaxed), 1);
    assert_eq!(e2.mute_group_name(0), "Band");
    assert_eq!(e2.main(0).unwrap().params.fader_db.load(), -2.0);
    assert_eq!(e2.strip_at(StripKind::Matrix, 1).unwrap().params.direct_source.load(Relaxed), 2);
    assert!(e2.controls.solo_afl.load(Relaxed));
    assert_eq!(e2.controls.output_ceiling_db.load(), -1.5);
    assert_eq!(ch(&e2, 1).name(), "Ch 2");
}

#[test]
fn scenes_respect_scope_and_recall_safe() {
    let engine = Engine::new(0);
    let mut np = NoPlugins;
    engine.set_num_channels(2);
    let mut show = Show::new();
    show.add_scene(&engine, &mut np, "A", -1);

    let (a, b) = (ch(&engine, 0), ch(&engine, 1));
    a.params.fader_db.store(-20.0);
    a.params.input_left.store(7, Relaxed);
    a.params.dca_mask.store(1, Relaxed);
    b.params.fader_db.store(-30.0);
    b.params.recall_safe.store(true, Relaxed);
    engine.controls.dcas[0].fader_db.store(-10.0);

    let r = show.recall_scene(&engine, &mut np, 0);
    assert_eq!(r.strips_skipped_safe, 1);
    assert_eq!(a.params.fader_db.load(), 0.0);
    assert_eq!(a.params.input_left.load(Relaxed), 7, "routing is outside the default scope");
    assert_eq!(a.params.dca_mask.load(Relaxed), 0, "DCA assignments are in the default scope");
    assert_eq!(engine.controls.dcas[0].fader_db.load(), 0.0, "DCA levels recall with the faders");
    assert_eq!(b.params.fader_db.load(), -30.0);

    show.scene_mut(0).unwrap().scope = section::ALL;
    show.recall_scene(&engine, &mut np, 0);
    assert_eq!(a.params.input_left.load(Relaxed), 0);
}

#[test]
fn snippets_touch_only_their_strips_and_sections() {
    let engine = Engine::new(0);
    let mut np = NoPlugins;
    engine.set_num_channels(3);
    let mut show = Show::new();
    show.add_scene(&engine, &mut np, "A", -1);
    let (a, b) = (ch(&engine, 0), ch(&engine, 1));
    a.params.fader_db.store(-6.0);
    a.params.mute.store(true, Relaxed);
    b.params.fader_db.store(-9.0);
    show.add_snippet(&engine, &mut np, 0, "S", section::FADER, vec![StripRef::of(&a)]);

    a.params.fader_db.store(-50.0);
    a.params.gate.on.store(true, Relaxed);
    a.params.recall_safe.store(true, Relaxed);
    b.params.fader_db.store(-50.0);
    let r = show.recall_snippet(&engine, &mut np, 0, 0);
    assert_eq!(r.strips_applied, 1);
    assert_eq!(a.params.fader_db.load(), -6.0, "snippets ignore recall safe");
    assert!(a.params.mute.load(Relaxed));
    assert!(a.params.gate.on.load(Relaxed), "the gate is not in the snippet");
    assert_eq!(b.params.fader_db.load(), -50.0);

    // Auto-fire with the scene: the scene sets 0 dB, the snippet overrides it.
    a.params.recall_safe.store(false, Relaxed);
    show.snippet_mut(0, 0).unwrap().fire_with_scene = true;
    show.recall_scene(&engine, &mut np, 0);
    assert_eq!(a.params.fader_db.load(), -6.0);
    assert_eq!(b.params.fader_db.load(), 0.0);
}

#[test]
fn model_changes_recall_with_their_parameters() {
    let engine = Engine::new(0);
    let mut np = NoPlugins;
    engine.set_num_channels(1);
    let a = ch(&engine, 0);
    a.params.dynamics.set_model(model_index(DYN_MODELS, "LA").unwrap());
    a.params.dynamics.param("peak").unwrap().store(70.0);
    let state = console::capture(&engine, section::DYNAMICS, None, &mut np);

    a.params.dynamics.set_model(model_index(DYN_MODELS, "COMP").unwrap());
    console::apply(&engine, &state, section::ALL, None, true, None, &mut np);
    assert_eq!(DYN_MODELS[a.params.dynamics.model_index()].token, "LA");
    assert_eq!(dyn_param(&a, "peak"), 70.0);
}

#[test]
fn fades_glide_and_survive_removed_strips() {
    let engine = Engine::new(0);
    let mut np = NoPlugins;
    engine.set_num_channels(2);
    let bus = engine.bus_in_slot(0).unwrap();
    let (a, b) = (ch(&engine, 0), ch(&engine, 1));
    a.sends.get_or_create(bus.id() as usize).unwrap().level_db.store(-10.0);
    a.params.mains[0].level_db.store(-10.0);

    let mut show = Show::new();
    show.add_scene(&engine, &mut np, "A", -1);
    show.scene_mut(0).unwrap().fade_seconds = 2.0;
    a.params.fader_db.store(-40.0);
    b.params.fader_db.store(-40.0);
    a.sends.find(bus.id() as usize).unwrap().level_db.store(-30.0);
    a.params.mains[0].level_db.store(-30.0);

    let start = Instant::now();
    show.recall_scene(&engine, &mut np, 0);
    assert!(show.is_fading());
    assert_eq!(a.params.fader_db.load(), -40.0, "nothing jumps");
    assert!(show.tick_at(&engine, start + Duration::from_millis(1000)));
    let mid = a.params.fader_db.load();
    assert!(mid > -30.0 && mid < -10.0, "{mid}");
    let send_mid = a.sends.find(bus.id() as usize).unwrap().level_db.load();
    assert!(send_mid > -30.0 && send_mid < -10.0, "{send_mid}");
    let main_mid = a.params.mains[0].level_db.load();
    assert!(main_mid > -30.0 && main_mid < -10.0, "{main_mid}");

    // Remove a strip mid-fade: the fade keeps it alive, nothing dangles.
    engine.remove_strip(Arc::as_ptr(&b));
    drop(b);
    engine.collect();
    assert!(!show.tick_at(&engine, start + Duration::from_secs(5)));
    assert_eq!(a.params.fader_db.load(), 0.0);
    assert_eq!(a.sends.find(bus.id() as usize).unwrap().level_db.load(), -10.0);

    // Cancel jumps to the targets.
    a.params.fader_db.store(-40.0);
    show.recall_scene(&engine, &mut np, 0);
    show.cancel_fade(&engine);
    assert!(!show.is_fading());
    assert_eq!(a.params.fader_db.load(), 0.0);
}

#[test]
fn fades_glide_dcas_widths_and_send_pans_while_switches_jump() {
    let engine = Engine::new(0);
    let mut np = NoPlugins;
    engine.set_num_channels(1);
    let bus = engine.bus_in_slot(0).unwrap();
    let a = ch(&engine, 0);
    let slot = bus.id() as usize;
    let dca = &engine.controls.dcas[0];
    let send = a.sends.get_or_create(slot).unwrap();
    dca.fader_db.store(-10.0);
    a.params.width.store(50.0);
    send.pan.store(-50.0);
    a.params.matrix[0].pan.store(-50.0);
    a.params.matrix[0].pan_link.store(false, Relaxed);
    a.params.mute.store(true, Relaxed);
    dca.mute.store(true, Relaxed);

    let mut show = Show::new();
    show.add_scene(&engine, &mut np, "A", -1);
    show.scene_mut(0).unwrap().fade_seconds = 2.0;
    dca.fader_db.store(-30.0);
    a.params.width.store(100.0);
    send.pan.store(50.0);
    a.params.matrix[0].pan.store(50.0);
    a.params.mute.store(false, Relaxed);
    dca.mute.store(false, Relaxed);

    let start = Instant::now();
    show.recall_scene(&engine, &mut np, 0);
    assert!(a.params.mute.load(Relaxed) && dca.mute.load(Relaxed), "switches jump at the start");
    assert!(!a.params.matrix[0].pan_link.load(Relaxed), "the saved pan decides an old show's link");
    assert_eq!(dca.fader_db.load(), -30.0, "nothing glides before the first tick");
    assert!(show.tick_at(&engine, start + Duration::from_millis(1000)));
    let between = |v: f32, x: f32, y: f32| v > x.min(y) && v < x.max(y);
    assert!(between(dca.fader_db.load(), -30.0, -10.0), "{}", dca.fader_db.load());
    assert!(between(a.params.width.load(), 100.0, 50.0), "{}", a.params.width.load());
    assert!(between(send.pan.load(), 50.0, -50.0), "{}", send.pan.load());
    assert!(between(a.params.matrix[0].pan.load(), 50.0, -50.0), "{}", a.params.matrix[0].pan.load());

    assert!(!show.tick_at(&engine, start + Duration::from_secs(3)));
    assert_eq!(dca.fader_db.load(), -10.0);
    assert_eq!(a.params.width.load(), 50.0);
    assert_eq!(send.pan.load(), -50.0);
    assert_eq!(a.params.matrix[0].pan.load(), -50.0);
}

#[test]
fn a_value_moved_during_a_fade_leaves_the_fade() {
    let engine = Engine::new(0);
    let mut np = NoPlugins;
    engine.set_num_channels(2);
    let bus = engine.bus_in_slot(0).unwrap();
    let slot = bus.id() as usize;
    let (a, b) = (ch(&engine, 0), ch(&engine, 1));
    let dca = &engine.controls.dcas[0];
    a.sends.get_or_create(slot).unwrap().level_db.store(0.0);

    let mut show = Show::new();
    show.add_scene(&engine, &mut np, "A", -1);
    show.scene_mut(0).unwrap().fade_seconds = 2.0;
    for s in [&a, &b] {
        s.params.fader_db.store(-40.0);
    }
    dca.fader_db.store(-40.0);
    a.sends.find(slot).unwrap().level_db.store(-40.0);

    let start = Instant::now();
    show.recall_scene(&engine, &mut np, 0);
    assert!(show.tick_at(&engine, start + Duration::from_millis(500)));
    // Moved by hand (surface, MIDI, a remote: they all write the parameter).
    a.params.fader_db.store(-25.0);
    dca.fader_db.store(-5.0);
    a.sends.find(slot).unwrap().level_db.store(-12.0);
    assert!(show.tick_at(&engine, start + Duration::from_millis(1000)));
    assert_eq!(a.params.fader_db.load(), -25.0);
    assert_eq!(dca.fader_db.load(), -5.0);
    assert_eq!(a.sends.find(slot).unwrap().level_db.load(), -12.0);
    let mid = b.params.fader_db.load();
    assert!(mid > -40.0 && mid < 0.0, "the others keep fading: {mid}");
    assert!(!show.tick_at(&engine, start + Duration::from_secs(3)));
    assert_eq!(a.params.fader_db.load(), -25.0);
    assert_eq!(b.params.fader_db.load(), 0.0);

    // A value moved just before the fade is cancelled (by the next recall) stays too.
    for s in [&a, &b] {
        s.params.fader_db.store(-40.0);
    }
    show.recall_scene(&engine, &mut np, 0);
    a.params.fader_db.store(-20.0);
    show.cancel_fade(&engine);
    assert_eq!(a.params.fader_db.load(), -20.0);
    assert_eq!(b.params.fader_db.load(), 0.0);
}

#[test]
fn recall_restores_fx_state_but_never_swaps_plugins() {
    let engine = Engine::new(0);
    let mut host = FakeHost::default();
    host.load(&engine, 0, "Comp", "one");
    let mut show = Show::new();
    show.add_scene(&engine, &mut Bridge { host: &mut host, engine: &engine }, "A", -1);

    host.plugins.get_mut(&0).unwrap().1 = "two".into();
    let r = show.recall_scene(&engine, &mut Bridge { host: &mut host, engine: &engine }, 0);
    assert!(r.warnings.is_empty());
    assert_eq!(host.plugins[&0].1, "one");

    host.load(&engine, 0, "Other", "x");
    let r = show.recall_scene(&engine, &mut Bridge { host: &mut host, engine: &engine }, 0);
    assert_eq!(r.warnings, vec!["FX 1: expected Comp, not recalled".to_string()]);
    assert_eq!(host.plugins[&0].0, "Other");
}

#[test]
fn show_files_are_atomic_with_backup_and_safe_mode() {
    let dir = std::env::temp_dir().join(format!("livedaw-session-test-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let path = dir.join("gig.lshow");

    let engine = Engine::new(0);
    let mut host = FakeHost::default();
    engine.set_num_channels(2);
    host.load(&engine, 1, "Delay", "S");
    let mut show = Show::new();
    show.name = "First".into();
    show.save_to_file(&engine, &mut Bridge { host: &mut host, engine: &engine }, &path).unwrap();
    show.name = "Second".into();
    show.save_to_file(&engine, &mut Bridge { host: &mut host, engine: &engine }, &path).unwrap();
    assert!(show::backup_path(&path).is_file());

    // Corrupt the main file: loading falls back to the backup (the first save).
    std::fs::write(&path, "<LiveDAWShow version=\"3\"><Cons").unwrap();
    let e2 = Engine::new(0);
    let mut host2 = FakeHost::default();
    let mut loaded = Show::new();
    loaded.load_from_file(&e2, &mut Bridge { host: &mut host2, engine: &e2 }, &path, false).unwrap();
    assert_eq!(loaded.name, "First");
    assert_eq!(e2.num_channels(), 2);
    assert!(host2.plugins.is_empty(), "safe mode instantiates nothing");
    assert!(!e2.fx(1).unwrap().is_loaded());

    // A plugin that fails to load is reported, the rest of the show still loads.
    host2.fail_to_create.push("Delay".into());
    let warnings = loaded.load_from_file(&e2, &mut Bridge { host: &mut host2, engine: &e2 }, &show::backup_path(&path), true);
    assert_eq!(warnings.unwrap(), vec!["FX 2: Delay (not installed)".to_string()]);

    assert!(Show::new().load_from_file(&e2, &mut NoPlugins, &dir.join("missing.lshow"), true).is_err());
    std::fs::remove_dir_all(&dir).unwrap();
}

#[test]
fn clear_builds_a_wing() {
    let engine = Engine::new(0);
    let mut show = Show::new();
    show.clear(&engine, 40);
    assert_eq!(engine.num_channels(), 40);
    assert_eq!(engine.num_strips(StripKind::Aux), 8);
    assert_eq!(engine.num_strips(StripKind::Bus), 16);
    assert_eq!(engine.num_strips(StripKind::Main), 4);
    assert_eq!(engine.num_strips(StripKind::Matrix), 8);
    let left: Vec<_> = show.layers.default_layers(&engine, LEFT).into_iter().map(|l| l.name).collect();
    assert_eq!(&left[..4], ["CH 1-12", "CH 13-24", "CH 25-36", "CH 37-40 · AUX 1-8"]);
    assert_eq!(show.layers.selected_layer(&engine, CENTER).name, "DCA 1-16");
}

#[test]
fn legacy_session_files() {
    let engine = Engine::new(0);
    let mut np = NoPlugins;
    let mut show = Show::new();

    // Version 1 "session": a bare console; a right input meant stereo.
    let v1 = r#"<?xml version="1.0"?><LiveDAWSession><Inputs>
        <Strip kind="input" id="5" name="Gtr" inL="0" inR="1"/><Strip kind="input" id="9" inL="2"/>
        </Inputs></LiveDAWSession>"#;
    show.load_from_text(&engine, &mut np, v1, true).unwrap();
    assert_eq!(show.name, "Imported session");
    let gtr = find_strip(&engine, &StripRef::new(StripKind::Channel, 5)).unwrap();
    assert_eq!(gtr.name(), "Gtr");
    assert!(gtr.params.stereo.load(Relaxed));
    assert!(!find_strip(&engine, &StripRef::new(StripKind::Channel, 9)).unwrap().params.stereo.load(Relaxed));

    assert!(show.load_from_text(&engine, &mut np, "<LiveDAWShow version=\"99\"><Console/></LiveDAWShow>", true).is_err());
    assert!(show.load_from_text(&engine, &mut np, "<Something/>", true).is_err());
    assert!(show.load_from_text(&engine, &mut np, "not xml", true).is_err());
    assert_eq!(engine.num_channels(), 2, "a rejected file leaves the console alone");
}

#[test]
fn legacy_v2_show_converts_to_the_wing_layout() {
    let v2 = r#"<?xml version="1.0"?>
      <LiveDAWShow version="2" name="Old gig" currentScene="0">
        <Console outputProtection="1" outputCeilingDb="-0.5">
          <Monitor afl="1" levelDb="-6" outL="2" outR="3"/>
          <Master><Strip kind="master" id="0" ref="master:0" sections="511" name="Master" faderDb="-3"/></Master>
          <Buses>
            <Strip kind="aux" id="0" ref="aux:0" sections="511" name="Wedge" mainAssign="0"/>
            <Strip kind="group" id="1" ref="group:1" sections="511" name="Drums" mainAssign="1"/>
          </Buses>
          <Inputs>
            <Strip kind="input" id="1" ref="input:1" sections="511" name="Kick" colour="ff2fb84a" faderDb="-4" pan="0.5"
                   trimDb="3" hpf="1" hpfFreq="70" stereo="0" inL="0" inR="-1" mainAssign="1">
              <Gate on="1" threshold="-35" range="30" attack="1" hold="40" release="120"/>
              <Comp on="1" threshold="-15" ratio="3" attack="8" release="90" knee="3" makeup="2"/>
              <Sends><Send slot="0" on="1" level="-6" pre="1"/><Send slot="1" on="1" level="0" pre="0"/></Sends>
              <Inserts><Insert slot="0" bypass="0"><PLUGIN name="Tape"/><State>T1</State></Insert></Inserts>
            </Strip>
            <Strip kind="input" id="2" ref="input:2" sections="511" name="Snare"/>
          </Inputs>
        </Console>
        <Layers bankSize="8" selected="in-1"><Layer id="user-abc" name="Mine" strips="input:2,group:1,master:0"/></Layers>
        <Scenes>
          <Scene name="Intro" scope="509" fade="0">
            <State sections="256">
              <Strip ref="input:1" sections="256" faderDb="-12" pan="0" mute="1"/>
              <Strip ref="master:0" sections="256" faderDb="-8"/>
            </State>
            <Snippets>
              <Snippet name="Kick up" sections="256" strips="input:1" fireWithScene="0">
                <State sections="256"><Strip ref="input:1" sections="256" faderDb="-1"/></State>
              </Snippet>
            </Snippets>
          </Scene>
        </Scenes>
      </LiveDAWShow>"#;

    let engine = Engine::new(0);
    let mut host = FakeHost::default();
    let mut show = Show::new();
    let warnings = show.load_from_text(&engine, &mut Bridge { host: &mut host, engine: &engine }, v2, true).unwrap();
    assert!(warnings.is_empty(), "{warnings:?}");
    assert_eq!(show.name, "Old gig");
    assert_eq!(engine.num_channels(), 2);

    let kick = find_strip(&engine, &StripRef::new(StripKind::Channel, 1)).unwrap();
    let p = &kick.params;
    assert_eq!(kick.name(), "Kick");
    assert_eq!(p.colour.load(Relaxed), 5, "green");
    assert_eq!((p.fader_db.load(), p.pan.load()), (-4.0, 50.0));
    assert_eq!(p.input.trim_db.load(), 3.0);
    assert!(p.filter.low_cut.load(Relaxed));
    assert_eq!(p.filter.low_cut_freq.load(), 70.0);
    assert!(p.gate.on.load(Relaxed));
    assert_eq!(p.gate.param("thr").unwrap().load(), -35.0);
    assert_eq!(p.gate.param("hld").unwrap().load(), 40.0);
    assert!(p.dynamics.on.load(Relaxed));
    assert_eq!(DYN_MODELS[p.dynamics.model_index()].token, "COMP");
    assert_eq!(dyn_param(&kick, "thr"), -15.0);
    assert_eq!(p.dynamics.gain.load(), 2.0);
    assert!(p.mains[0].on.load(Relaxed));
    let pre = kick.sends.find(0).unwrap();
    assert_eq!((pre.enabled.load(Relaxed), pre.level_db.load(), pre.mode.load(Relaxed)), (true, -6.0, 0));
    assert_eq!(kick.sends.find(1).unwrap().mode.load(Relaxed), 1);
    assert_eq!((p.pre_insert.on.load(Relaxed), p.pre_insert.fx.load(Relaxed)), (true, 0));
    assert_eq!(host.plugins.get(&0), Some(&("Tape".to_string(), "T1".to_string())));

    let wedge = engine.bus_in_slot(0).unwrap();
    assert_eq!(wedge.name(), "Wedge");
    assert!(!wedge.params.mains[0].on.load(Relaxed));
    assert!(engine.bus_in_slot(1).unwrap().params.mains[0].on.load(Relaxed), "groups keep feeding the main");
    assert_eq!(engine.main(0).unwrap().params.fader_db.load(), -3.0);
    assert!(engine.controls.solo_afl.load(Relaxed));
    assert_eq!(engine.controls.output_ceiling_db.load(), -0.5);

    let user = &show.layers.user_layers()[0];
    assert_eq!(user.name, "Mine");
    assert_eq!(
        user.faders,
        vec![
            StripRef::new(StripKind::Channel, 2).into(),
            StripRef::new(StripKind::Bus, 1).into(),
            StripRef::new(StripKind::Main, 0).into()
        ]
    );

    let mut np = NoPlugins;
    show.recall_scene(&engine, &mut np, 0);
    assert_eq!(p.fader_db.load(), -12.0);
    assert!(p.mute.load(Relaxed));
    assert_eq!(engine.main(0).unwrap().params.fader_db.load(), -8.0);
    assert_eq!(show.snippet(0, 0).unwrap().strips, vec![StripRef::new(StripKind::Channel, 1)]);
    show.recall_snippet(&engine, &mut np, 0, 0);
    assert_eq!(p.fader_db.load(), -1.0);

    // Saved again, it's a version 3 show.
    let text = show.to_xml(&engine, &mut Bridge { host: &mut host, engine: &engine }).to_document();
    assert!(text.contains("<LiveDAWShow version=\"3\""), "{}", &text[..200]);
}

#[test]
fn capture_and_apply_by_section() {
    let engine = Engine::new(0);
    let mut np = NoPlugins;
    engine.set_num_channels(2);
    let a = ch(&engine, 0);
    a.params.eq.param("1g", false).unwrap().store(5.0);
    a.params.dynamics.param("thr").unwrap().store(-12.0);
    let state = console::capture(&engine, section::EQ, None, &mut np);
    a.params.eq.param("1g", false).unwrap().store(0.0);
    a.params.dynamics.param("thr").unwrap().store(-30.0);

    // Asking for more than was captured only touches the captured part.
    let r = console::apply(&engine, &state, section::ALL, None, true, None, &mut np);
    assert_eq!(r.strips_applied, 2 + 8 + 16 + 4 + 8);
    assert_eq!(a.params.eq.param("1g", false).unwrap().load(), 5.0);
    assert_eq!(dyn_param(&a, "thr"), -30.0);

    // Strips missing from the console are counted, not fatal.
    engine.set_num_channels(1);
    let r = console::apply(&engine, &state, section::ALL, None, true, None, &mut np);
    assert_eq!(r.strips_missing, 1);
}

#[test]
fn builtin_fx_devices_save_load_and_recall() {
    use livedaw_dsp::wing::FX_MODELS;
    let fx = |t: &str| model_index(FX_MODELS, t).unwrap();
    let engine = Engine::new(0);
    let mut host = FakeHost::default();

    // A hall reverb in FX 1 with a long decay, a graphic EQ in FX 9.
    assert!(engine.set_fx_model(0, fx("HALL"), true));
    engine.controls.fx[0].param("dcy").unwrap().store(3.5);
    engine.controls.fx[0].mix.store(40.0);
    assert!(engine.set_fx_model(8, fx("GEQ"), true));
    assert!(!engine.set_fx_model(9, fx("HALL"), true), "premium devices need slots 1-8");

    let mut show = Show::new();
    show.add_scene(&engine, &mut Bridge { host: &mut host, engine: &engine }, "A", -1);
    let text = show.to_xml(&engine, &mut Bridge { host: &mut host, engine: &engine }).to_document();

    let e2 = Engine::new(0);
    let mut host2 = FakeHost::default();
    let mut loaded = Show::new();
    let warnings = loaded.load_from_text(&e2, &mut Bridge { host: &mut host2, engine: &e2 }, &text, true).unwrap();
    assert!(warnings.is_empty(), "{warnings:?}");
    assert_eq!(e2.controls.fx[0].model_index(), fx("HALL"));
    assert_eq!(e2.controls.fx[0].param("dcy").unwrap().load(), 3.5);
    assert_eq!(e2.controls.fx[0].mix.load(), 40.0);
    assert!(e2.fx(0).unwrap().has_device());
    assert_eq!(e2.controls.fx[8].model_index(), fx("GEQ"));

    // Recalls switch built-in devices (creating one is safe at any time).
    assert!(e2.set_fx_model(0, fx("ST-DL"), true));
    let r = loaded.recall_scene(&e2, &mut Bridge { host: &mut host2, engine: &e2 }, 0);
    assert!(r.warnings.is_empty(), "{:?}", r.warnings);
    assert_eq!(e2.controls.fx[0].model_index(), fx("HALL"));
    assert_eq!(e2.controls.fx[0].param("dcy").unwrap().load(), 3.5);

    // ... but never replace a plugin.
    host2.load(&e2, 0, "Comp", "x");
    let r = loaded.recall_scene(&e2, &mut Bridge { host: &mut host2, engine: &e2 }, 0);
    assert_eq!(r.warnings.len(), 1);
    assert_eq!(e2.controls.fx[0].model_index(), livedaw_engine::params::FX_EXT);
    assert!(host2.plugins.contains_key(&0));
}

// ---- WING console settings and scene list features ------------------------------

fn round_trip(engine: &Engine, show: &Show) -> (Engine, Show) {
    let text = show.to_xml(engine, &mut NoPlugins).to_document();
    let e2 = Engine::new(0);
    let mut loaded = Show::new();
    loaded.load_from_text(&e2, &mut NoPlugins, &text, true).unwrap();
    (e2, loaded)
}

#[test]
fn wing_settings_survive_a_show_round_trip() {
    let engine = Engine::new(0);
    engine.set_num_channels(1);
    let c = &engine.controls;
    let a = ch(&engine, 0);
    let send = a.sends.get_or_create(3).unwrap();
    send.ignore_mute.store(true, Relaxed);
    send.pan_reversed.store(true, Relaxed);
    a.params.matrix[2].ignore_mute.store(true, Relaxed);
    c.bus_solo_afl.store(false, Relaxed);
    c.main_solo_afl.store(false, Relaxed);
    c.matrix_solo_afl.store(false, Relaxed);
    c.monitor_dim_db.store(12.0);
    c.monitor_dim.store(true, Relaxed);
    c.monitor_mono.store(true, Relaxed);
    c.dca_mute_groups.store(false, Relaxed);
    c.main_link.store(3, Relaxed);
    c.startup_main_mute.store(true, Relaxed);
    c.automix_enable[1].store(false, Relaxed);

    let (e2, _) = round_trip(&engine, &Show::new());
    let c2 = &e2.controls;
    let b = ch(&e2, 0);
    let send = b.sends.find(3).unwrap();
    assert!(send.ignore_mute.load(Relaxed) && send.pan_reversed.load(Relaxed));
    assert!(!b.sends.find(4).unwrap().ignore_mute.load(Relaxed), "untouched sends keep the WING default");
    assert!(b.params.matrix[2].ignore_mute.load(Relaxed) && !b.params.matrix[1].ignore_mute.load(Relaxed));
    assert!(!c2.bus_solo_afl.load(Relaxed) && !c2.main_solo_afl.load(Relaxed) && !c2.matrix_solo_afl.load(Relaxed));
    assert_eq!(c2.monitor_dim_db.load(), 12.0);
    assert!(c2.monitor_dim.load(Relaxed) && c2.monitor_mono.load(Relaxed));
    assert!(!c2.dca_mute_groups.load(Relaxed));
    assert_eq!(c2.main_link.load(Relaxed), 3);
    assert!(c2.startup_main_mute.load(Relaxed));
    assert!(c2.automix_enable[0].load(Relaxed) && !c2.automix_enable[1].load(Relaxed));
}

#[test]
fn key_source_and_tap_survive_a_show_round_trip() {
    let engine = Engine::new(0);
    engine.set_num_channels(3);
    let a = ch(&engine, 0);
    a.params.gate.sc_source.store(2, Relaxed);
    a.params.gate.sc_tap.store(7, Relaxed); // POST
    a.params.dynamics.sc_source.store(1, Relaxed);
    a.params.dynamics.sc_tap.store(5, Relaxed); // PFL
    let bus = engine.bus_in_slot(0).unwrap();
    bus.params.dynamics.sc_source.store(0, Relaxed);
    bus.params.dynamics.sc_tap.store(6, Relaxed); // AFL

    let (e2, _) = round_trip(&engine, &Show::new());
    let b = ch(&e2, 0);
    assert_eq!((b.params.gate.sc_source.load(Relaxed), b.params.gate.sc_tap.load(Relaxed)), (2, 7));
    assert_eq!((b.params.dynamics.sc_source.load(Relaxed), b.params.dynamics.sc_tap.load(Relaxed)), (1, 5));
    let d = &e2.bus_in_slot(0).unwrap().params.dynamics;
    assert_eq!((d.sc_source.load(Relaxed), d.sc_tap.load(Relaxed)), (0, 6));
    assert_eq!(ch(&e2, 1).params.gate.sc_tap.load(Relaxed), 0, "untouched: IN");
}

#[test]
fn matrix_pan_link_and_dca_icons_survive_a_show_round_trip() {
    let engine = Engine::new(0);
    engine.set_num_channels(1);
    let a = ch(&engine, 0);
    a.params.matrix[1].pan_link.store(false, Relaxed);
    a.params.matrix[1].pan.store(-40.0);
    a.params.matrix[2].pan_reversed.store(true, Relaxed);
    let d = &engine.controls.dcas[4];
    d.icon.store(112, Relaxed);
    d.led.store(false, Relaxed);

    let (e2, _) = round_trip(&engine, &Show::new());
    let b = ch(&e2, 0);
    let m = &b.params.matrix;
    assert!(!m[1].pan_link.load(Relaxed) && m[1].pan.load() == -40.0);
    assert!(m[2].pan_link.load(Relaxed) && m[2].pan_reversed.load(Relaxed));
    assert!(m[0].pan_link.load(Relaxed) && !m[0].pan_reversed.load(Relaxed));
    let d2 = &e2.controls.dcas[4];
    assert_eq!(d2.icon.load(Relaxed), 112);
    assert!(!d2.led.load(Relaxed) && e2.controls.dcas[0].led.load(Relaxed));

    // Shows from before PAN LINK: a centred send stays linked (as it sounded),
    // one with a pan keeps that pan.
    let mut text = Show::new().to_xml(&engine, &mut NoPlugins).to_document();
    for attr in [" plink=\"0\"", " plink=\"1\"", " prev=\"0\"", " prev=\"1\"", " icon=\"112\"", " led=\"0\""] {
        text = text.replace(attr, "");
    }
    let e3 = Engine::new(0);
    Show::new().load_from_text(&e3, &mut NoPlugins, &text, true).unwrap();
    let m = &ch(&e3, 0).params.matrix;
    assert!(!m[1].pan_link.load(Relaxed), "panned: its own pan");
    assert!(m[2].pan_link.load(Relaxed) && !m[2].pan_reversed.load(Relaxed));
    assert_eq!(e3.controls.dcas[4].icon.load(Relaxed), 0);
    assert!(e3.controls.dcas[4].led.load(Relaxed));
}

#[test]
fn shows_without_the_new_settings_load_with_the_old_behaviour() {
    let engine = Engine::new(0);
    engine.set_num_channels(1);
    let a = ch(&engine, 0);
    a.sends.get_or_create(3).unwrap().enabled.store(true, Relaxed); // PRE
    let post = a.sends.get_or_create(4).unwrap();
    post.mode.store(1, Relaxed);
    a.params.matrix[0].pre.store(true, Relaxed);
    let mut show = Show::new();
    show.add_scene(&engine, &mut NoPlugins, "A", -1);
    let mut text = show.to_xml(&engine, &mut NoPlugins).to_document();
    for attr in [" ignmute=\"0\"", " prev=\"0\"", " tag=\"0\"", " skip=\"0\"", " link=\"0\""] {
        text = text.replace(attr, "");
    }
    assert!(text.contains("<Prefs") && !text.contains("ignmute"));
    let start = text.find("<Prefs").unwrap();
    let end = start + text[start..].find("/>").unwrap() + 2;
    text.replace_range(start..end, "");

    let e2 = Engine::new(0);
    e2.controls.bus_solo_afl.store(false, Relaxed);
    e2.controls.dca_mute_groups.store(false, Relaxed);
    let mut loaded = Show::new();
    loaded.load_from_text(&e2, &mut NoPlugins, &text, true).unwrap();
    let b = ch(&e2, 0);
    // Pre-fader sends of old shows never followed the channel mute.
    assert!(b.sends.find(3).unwrap().ignore_mute.load(Relaxed));
    assert!(!b.sends.find(4).unwrap().ignore_mute.load(Relaxed));
    assert!(b.params.matrix[0].ignore_mute.load(Relaxed) && !b.params.matrix[1].ignore_mute.load(Relaxed));
    assert!(e2.controls.bus_solo_afl.load(Relaxed), "buses soloed AFL");
    assert!(e2.controls.dca_mute_groups.load(Relaxed), "DCA mutes muted their strips");
    let scene = loaded.scene(0).unwrap();
    assert_eq!((scene.tag, scene.skip, scene.link_next), (0, false, false));
}

#[test]
fn scenes_recall_solo_and_automix_settings_but_not_preferences_or_keys() {
    let engine = Engine::new(0);
    let mut np = NoPlugins;
    let c = &engine.controls;
    let mut show = Show::new();
    show.add_scene(&engine, &mut np, "A", -1);
    show.scene_mut(0).unwrap().scope = section::ALL;
    c.bus_solo_afl.store(false, Relaxed);
    c.automix_enable[0].store(false, Relaxed);
    c.monitor_dim_db.store(10.0);
    c.main_link.store(2, Relaxed);
    c.dca_mute_groups.store(false, Relaxed);
    c.monitor_dim.store(true, Relaxed);
    show.recall_scene(&engine, &mut np, 0);
    assert!(c.bus_solo_afl.load(Relaxed), "solo PFL/AFL recalls with ROUTING");
    assert!(c.automix_enable[0].load(Relaxed), "automix enables recall with INSERTS");
    assert_eq!(c.monitor_dim_db.load(), 20.0);
    assert_eq!(c.main_link.load(Relaxed), 2, "preferences are not in scenes");
    assert!(!c.dca_mute_groups.load(Relaxed));
    assert!(c.monitor_dim.load(Relaxed), "the DIM key is not in scenes");

    // Outside the scope, nothing changes.
    c.bus_solo_afl.store(false, Relaxed);
    c.automix_enable[0].store(false, Relaxed);
    show.scene_mut(0).unwrap().scope = section::FADER;
    show.recall_scene(&engine, &mut np, 0);
    assert!(!c.bus_solo_afl.load(Relaxed) && !c.automix_enable[0].load(Relaxed));
}

#[test]
fn scene_tags_skip_and_link() {
    let engine = Engine::new(0);
    let mut np = NoPlugins;
    engine.set_num_channels(1);
    let a = ch(&engine, 0);
    let mut show = Show::new();
    for i in 0..5 {
        a.params.fader_db.store(-(i as f32) - 1.0); // scene i: -(i+1) dB
        show.add_scene(&engine, &mut np, &format!("S{i}"), -1);
    }
    let fader = || a.params.fader_db.load();
    show.recall_scene(&engine, &mut np, 0);
    assert_eq!(fader(), -1.0);

    // SKIP: GO NEXT / GO PREV and direct recalls pass over it.
    show.scene_mut(1).unwrap().skip = true;
    show.recall_next(&engine, &mut np);
    assert_eq!((show.current_scene(), fader()), (2, -3.0));
    show.recall_previous(&engine, &mut np);
    assert_eq!((show.current_scene(), fader()), (0, -1.0));
    let r = show.recall_scene(&engine, &mut np, 1);
    assert_eq!((r.strips_applied, show.current_scene()), (0, 0));

    // At the ends GO NEXT / GO PREV stay on the last / first playable scene.
    show.scene_mut(4).unwrap().skip = true;
    show.recall_scene(&engine, &mut np, 3);
    show.recall_next(&engine, &mut np);
    assert_eq!(show.current_scene(), 3);

    // Tags
    show.scene_mut(3).unwrap().tag = 7;
    show.scene_mut(4).unwrap().tag = 7; // skipped: not found
    assert_eq!(show.find_scene_by_tag(7), 3);
    assert_eq!(show.find_scene_by_tag(8), -1);
    assert_eq!(show.find_scene_by_tag(0), -1);
    show.recall_scene(&engine, &mut np, 0);
    show.recall_by_tag(&engine, &mut np, 7);
    assert_eq!((show.current_scene(), fader()), (3, -4.0));

    // LINK: scene 2 also recalls scene 3 (the later one wins); the chain stops at a skipped scene.
    show.scene_mut(1).unwrap().skip = false;
    show.scene_mut(2).unwrap().link_next = true;
    show.recall_scene(&engine, &mut np, 0);
    let r = show.recall_scene(&engine, &mut np, 2);
    assert_eq!((show.current_scene(), fader()), (3, -4.0));
    assert_eq!(r.strips_applied, 2 * (1 + 8 + 16 + 4 + 8));
    show.scene_mut(3).unwrap().link_next = true; // 4 is skipped
    show.recall_scene(&engine, &mut np, 2);
    assert_eq!(show.current_scene(), 3);

    // The link moves with its scene: scene 2 (linked) moved to the end links to nothing.
    show.scene_mut(3).unwrap().link_next = false;
    show.move_scene(2, 4);
    assert!(show.scene(4).unwrap().link_next);
    show.recall_scene(&engine, &mut np, 0);
    show.scene_mut(4).unwrap().skip = false;
    show.recall_scene(&engine, &mut np, 4);
    assert_eq!((show.current_scene(), fader()), (4, -3.0));

    // Every scene linked: each recalled once.
    for i in 0..5 {
        show.scene_mut(i).unwrap().link_next = true;
        show.scene_mut(i).unwrap().skip = false;
    }
    let r = show.recall_scene(&engine, &mut np, 0);
    assert_eq!(r.strips_applied, 5 * (1 + 8 + 16 + 4 + 8));

    // Saved with the show.
    show.scene_mut(1).unwrap().skip = true;
    show.scene_mut(0).unwrap().tag = 128;
    show.scene_mut(1).unwrap().link_next = false;
    let (_, loaded) = round_trip(&engine, &show);
    let s0 = loaded.scene(0).unwrap();
    let s1 = loaded.scene(1).unwrap();
    assert_eq!((s0.tag, s0.skip, s0.link_next), (128, false, true));
    assert_eq!((s1.tag, s1.skip, s1.link_next), (0, true, false));
}

#[test]
fn send_midi_goes_with_every_recall_of_its_scene() {
    let engine = Engine::new(0);
    let mut np = NoPlugins;
    engine.set_num_channels(1);
    let mut show = Show::new();
    for i in 0..3 {
        show.add_scene(&engine, &mut np, &format!("S{i}"), -1);
    }
    show.scene_mut(0).unwrap().midi = "B0,07,64".into();
    show.scene_mut(1).unwrap().midi = "U:C002".into();
    show.recall_scene(&engine, &mut np, 0);
    assert_eq!(show.take_pending_midi(), vec!["B0,07,64".to_string()]);
    assert!(show.take_pending_midi().is_empty(), "taken once");

    // GO NEXT, then a scene without MIDI: nothing left over from before.
    show.recall_next(&engine, &mut np);
    assert_eq!(show.take_pending_midi(), vec!["U:C002".to_string()]);
    show.recall_scene(&engine, &mut np, 0);
    show.recall_scene(&engine, &mut np, 2);
    assert!(show.take_pending_midi().is_empty());

    // A linked scene sends its own MIDI after the first; a skipped one sends none.
    show.scene_mut(0).unwrap().link_next = true;
    show.recall_scene(&engine, &mut np, 0);
    assert_eq!(show.take_pending_midi(), vec!["B0,07,64".to_string(), "U:C002".to_string()]);
    show.scene_mut(0).unwrap().skip = true;
    show.recall_scene(&engine, &mut np, 0);
    assert!(show.take_pending_midi().is_empty());

    // Saved with the show (and left out while empty).
    let xml = show.to_xml(&engine, &mut NoPlugins).to_document();
    assert_eq!(xml.matches("midi=").count(), 2);
    let (_, loaded) = round_trip(&engine, &show);
    assert_eq!(loaded.scene(1).unwrap().midi, "U:C002");
    assert_eq!(loaded.scene(2).unwrap().midi, "");
}

#[test]
fn alt_inputs_and_input_select_are_saved_and_recalled_with_routing() {
    let engine = Engine::new(0);
    let mut np = NoPlugins;
    engine.set_num_channels(2);
    let c = &engine.controls;
    let a = ch(&engine, 0);
    let aux = engine.strip_at(StripKind::Aux, 3).unwrap();
    a.params.alt_input_left.store(0x10000 + 5, Relaxed); // playback channel 6
    a.params.alt_input_right.store(0x10000 + 6, Relaxed);
    a.params.alt_selected.store(true, Relaxed);
    aux.params.alt_input_left.store(12, Relaxed);
    c.input_select_global.store(true, Relaxed);
    c.global_alt.store(true, Relaxed);

    // Show files keep them.
    let (e2, _) = round_trip(&engine, &Show::new());
    let b = ch(&e2, 0);
    assert_eq!((b.params.alt_input_left.load(Relaxed), b.params.alt_input_right.load(Relaxed)), (0x10005, 0x10006));
    assert!(b.params.alt_selected.load(Relaxed));
    assert_eq!(e2.strip_at(StripKind::Aux, 3).unwrap().params.alt_input_left.load(Relaxed), 12);
    assert_eq!(ch(&e2, 1).params.alt_input_left.load(Relaxed), -1);
    assert!(e2.controls.input_select_global.load(Relaxed) && e2.controls.global_alt.load(Relaxed));

    // Scenes recall them with ROUTING (CONN), not with other sections.
    let mut show = Show::new();
    show.add_scene(&engine, &mut np, "A", -1);
    show.scene_mut(0).unwrap().scope = section::ROUTING;
    a.params.alt_input_left.store(3, Relaxed);
    a.params.alt_selected.store(false, Relaxed);
    c.input_select_global.store(false, Relaxed);
    c.global_alt.store(false, Relaxed);
    show.recall_scene(&engine, &mut np, 0);
    assert_eq!(a.params.alt_input_left.load(Relaxed), 0x10005);
    assert!(a.params.alt_selected.load(Relaxed));
    assert!(c.input_select_global.load(Relaxed) && c.global_alt.load(Relaxed));
    a.params.alt_selected.store(false, Relaxed);
    c.global_alt.store(false, Relaxed);
    show.scene_mut(0).unwrap().scope = section::FADER | section::PREAMP;
    show.recall_scene(&engine, &mut np, 0);
    assert!(!a.params.alt_selected.load(Relaxed) && !c.global_alt.load(Relaxed));

    // Shows from before ALT inputs load as before.
    let mut text = Show::new().to_xml(&engine, &mut np).to_document();
    let attr = |name: &str, v: &str| format!(" {name}=\"{v}\"");
    for (name, v) in [("altL", "65541"), ("altR", "65542"), ("altSel", "0"), ("altL", "-1"), ("altR", "-1"), ("altL", "12")] {
        text = text.replace(&attr(name, v), "");
    }
    let start = text.find("<InputSelect").unwrap();
    let end = start + text[start..].find("/>").unwrap() + 2;
    text.replace_range(start..end, "");
    assert!(!text.contains(" alt"), "every ALT attribute removed");
    let e3 = Engine::new(0);
    let mut loaded = Show::new();
    loaded.load_from_text(&e3, &mut np, &text, true).unwrap();
    assert_eq!(ch(&e3, 0).params.alt_input_left.load(Relaxed), -1);
    assert!(!ch(&e3, 0).params.alt_selected.load(Relaxed));
    assert!(!e3.controls.input_select_global.load(Relaxed) && !e3.controls.global_alt.load(Relaxed));
}

// ---- scopes, global safes, partial load / update, restore, snippet REC -----------------

fn mute_groups(engine: &Engine) -> u32 {
    engine.controls.mute_groups.load(Relaxed)
}

#[test]
fn per_strip_scene_scope_and_item_masks() {
    let engine = Engine::new(0);
    let mut np = NoPlugins;
    engine.set_num_channels(3);
    let mut show = Show::new();
    show.add_scene(&engine, &mut np, "A", -1);
    let (a, b) = (ch(&engine, 0), ch(&engine, 1));
    {
        let scene = show.scene_mut(0).unwrap();
        let mut scope = scene.recall_scope();
        scope.strips = Some(vec![StripRef::of(&a)]);
        scope.dcas = 0b10; // DCA 2 only
        scope.fx = 0;
        scene.set_recall_scope(&scope);
    }
    a.params.fader_db.store(-20.0);
    b.params.fader_db.store(-20.0);
    engine.controls.dcas[0].fader_db.store(-7.0);
    engine.controls.dcas[1].fader_db.store(-7.0);
    engine.controls.fx[2].mix.store(30.0);

    let r = show.recall_scene(&engine, &mut np, 0);
    assert_eq!(r.strips_applied, 1);
    assert_eq!(a.params.fader_db.load(), 0.0);
    assert_eq!(b.params.fader_db.load(), -20.0, "outside the scene's strips");
    assert_eq!(engine.controls.dcas[0].fader_db.load(), -7.0, "DCA 1 is outside the scope");
    assert_eq!(engine.controls.dcas[1].fader_db.load(), 0.0);
    assert_eq!(engine.controls.fx[2].mix.load(), 30.0, "no FX slot in the scope");

    // The scope survives a save and load; older scenes recall everything.
    let (_, loaded) = round_trip(&engine, &show);
    assert_eq!(loaded.scene(0).unwrap().recall_scope(), show.scene(0).unwrap().recall_scope());
    let old = show.to_xml(&engine, &mut np).to_document().replace("scopeStrips=", "x=").replace("scopeDca=", "y=");
    let mut older = Show::new();
    older.load_from_text(&Engine::new(0), &mut np, &old, true).unwrap();
    let scope = older.scene(0).unwrap().recall_scope();
    assert_eq!((scope.strips, scope.dcas), (None, scope::ALL_DCAS));
}

#[test]
fn global_safes_beyond_strips() {
    let engine = Engine::new(0);
    let mut np = NoPlugins;
    engine.set_num_channels(2);
    let mut show = Show::new();
    show.add_scene(&engine, &mut np, "A", -1);
    show.scene_mut(0).unwrap().scope = section::ALL;
    let a = ch(&engine, 0);

    show.global_safe = GlobalSafe { sections: section::GATE, dcas: 1, mute_groups: 0b100, fx: 1 << 3, console: true };
    a.params.gate.on.store(true, Relaxed);
    a.params.fader_db.store(-12.0);
    engine.controls.dcas[0].fader_db.store(-5.0);
    engine.controls.dcas[1].fader_db.store(-5.0);
    engine.controls.mute_groups.store(0b101, Relaxed);
    engine.controls.fx[3].mix.store(40.0);
    engine.controls.fx[4].mix.store(40.0);
    engine.controls.monitor_dim_db.store(-30.0);

    show.recall_scene(&engine, &mut np, 0);
    assert!(a.params.gate.on.load(Relaxed), "the gate is a safe section");
    assert_eq!(a.params.fader_db.load(), 0.0);
    assert_eq!(engine.controls.dcas[0].fader_db.load(), -5.0, "DCA 1 is safe");
    assert_eq!(engine.controls.dcas[1].fader_db.load(), 0.0);
    assert_eq!(mute_groups(&engine), 0b100, "group 3 is safe, group 1 recalled off");
    assert_eq!(engine.controls.fx[3].mix.load(), 40.0, "FX 4 is safe");
    assert_eq!(engine.controls.fx[4].mix.load(), 100.0);
    assert_eq!(engine.controls.monitor_dim_db.load(), -30.0, "the console settings are safe");

    // RESTORE ignores every safe and the scope.
    show.scene_mut(0).unwrap().scope = section::FADER;
    a.params.recall_safe.store(true, Relaxed);
    let r = show.recall_scene_with(&engine, &mut np, 0, None, false);
    assert_eq!(r.strips_skipped_safe, 0);
    assert!(!a.params.gate.on.load(Relaxed));
    assert_eq!(engine.controls.dcas[0].fader_db.load(), 0.0);
    assert_eq!(mute_groups(&engine), 0);
    assert_eq!(engine.controls.fx[3].mix.load(), 100.0);

    // The safes are saved with the show; a new show has none.
    let (_, loaded) = round_trip(&engine, &show);
    assert_eq!(loaded.global_safe, show.global_safe);
    let mut cleared = Show::new();
    cleared.global_safe.dcas = 3;
    cleared.clear(&engine, 2);
    assert!(cleared.global_safe.is_empty());
}

#[test]
fn partial_load_and_update_keep_their_scope() {
    let engine = Engine::new(0);
    let mut np = NoPlugins;
    engine.set_num_channels(2);
    let mut show = Show::new();
    show.add_scene(&engine, &mut np, "A", -1);
    let (a, b) = (ch(&engine, 0), ch(&engine, 1));

    // PARTIAL UPDATE: only channel 1's fader goes into the scene.
    a.params.fader_db.store(-4.0);
    a.params.gate.on.store(true, Relaxed);
    b.params.fader_db.store(-8.0);
    let only_a = Scope::strips(section::FADER, vec![StripRef::of(&a)]);
    show.store_scene_partial(&engine, &mut np, 0, &only_a);
    assert_eq!(show.partial_scope, only_a, "PARTIAL UPDATE keeps its scope");

    a.params.fader_db.store(-30.0);
    a.params.gate.on.store(false, Relaxed);
    b.params.fader_db.store(-30.0);
    show.scene_mut(0).unwrap().scope = section::ALL;
    show.recall_scene(&engine, &mut np, 0);
    assert_eq!(a.params.fader_db.load(), -4.0);
    assert!(!a.params.gate.on.load(Relaxed), "the gate was not updated");
    assert_eq!(b.params.fader_db.load(), 0.0, "channel 2 kept the stored level");

    // PARTIAL LOAD: channel 2 only, respecting safes unless NO GLOB.
    a.params.fader_db.store(-30.0);
    b.params.fader_db.store(-30.0);
    b.params.recall_safe.store(true, Relaxed);
    let only_b = Scope::strips(section::FADER, vec![StripRef::of(&b)]);
    let r = show.recall_scene_with(&engine, &mut np, 0, Some(&only_b), false);
    assert_eq!((r.strips_applied, r.strips_skipped_safe), (0, 1));
    assert_eq!(b.params.fader_db.load(), -30.0);
    show.recall_scene_with(&engine, &mut np, 0, Some(&only_b), true);
    assert_eq!(b.params.fader_db.load(), 0.0, "NO GLOB ignores the safes");
    assert_eq!(a.params.fader_db.load(), -30.0, "outside the partial scope");
    assert_eq!(show.partial_scope, only_b);

    let (_, loaded) = round_trip(&engine, &show);
    assert_eq!(loaded.partial_scope, only_b);
}

#[test]
fn snippet_rec_records_the_changed_parameters() {
    let engine = Engine::new(0);
    let mut np = NoPlugins;
    engine.set_num_channels(4);
    let mut show = Show::new();
    show.add_scene(&engine, &mut np, "A", -1);
    let (a, b, c) = (ch(&engine, 0), ch(&engine, 1), ch(&engine, 2));
    assert_eq!(show.recorded_count(&engine), 0);

    show.start_recording(&engine);
    assert!(show.is_recording());
    a.params.fader_db.store(-6.0);
    b.params.gate.on.store(true, Relaxed);
    b.params.fader_db.store(-3.0);
    engine.controls.dcas[4].mute.store(true, Relaxed);
    assert_eq!(show.recorded_count(&engine), 4);
    assert_eq!(show.stop_recording(&engine), 4);
    assert!(!show.is_recording());
    c.params.fader_db.store(-40.0); // after STOP: not recorded

    let changes = show.recorded_changes(None).unwrap();
    assert_eq!(changes.strips, vec![(StripRef::of(&b), section::GATE | section::FADER), (StripRef::of(&a), section::FADER)]);
    assert_eq!(changes.dcas, 1 << 4);

    // REC FOCUS limits what SAVE keeps: faders of channel 2 only.
    let focus = Scope::strips(section::FADER, vec![StripRef::of(&b)]);
    let limited = show.recorded_changes(Some(&focus)).unwrap();
    assert_eq!(limited.strips, vec![(StripRef::of(&b), section::FADER)]);
    assert_eq!(limited.count, 1);

    let index = show.add_recorded_snippet(&engine, &mut np, 0, "Rec", None);
    assert_eq!(index, 0);
    assert!(show.recorded_changes(None).is_none(), "SAVE uses up the recording");
    let sn = show.snippet(0, 0).unwrap().clone();
    assert_eq!(sn.parameter_count(), 4);
    assert_eq!(sn.dcas, 1 << 4);

    // Recall: exactly the changed pairs, not their cross product.
    a.params.fader_db.store(-50.0);
    a.params.gate.on.store(true, Relaxed);
    b.params.gate.on.store(false, Relaxed);
    b.params.fader_db.store(-50.0);
    c.params.fader_db.store(-50.0);
    engine.controls.dcas[4].mute.store(false, Relaxed);
    show.recall_snippet(&engine, &mut np, 0, 0);
    assert_eq!(a.params.fader_db.load(), -6.0);
    assert!(a.params.gate.on.load(Relaxed), "channel 1's gate was not recorded");
    assert!(b.params.gate.on.load(Relaxed));
    assert_eq!(b.params.fader_db.load(), -3.0);
    assert_eq!(c.params.fader_db.load(), -50.0);
    assert!(engine.controls.dcas[4].mute.load(Relaxed));

    // UPDATE keeps each strip's own sections; the snippet survives a round trip.
    b.params.fader_db.store(-9.0);
    show.store_snippet(&engine, &mut np, 0, 0);
    a.params.gate.on.store(false, Relaxed);
    show.recall_snippet(&engine, &mut np, 0, 0);
    assert!(!a.params.gate.on.load(Relaxed));
    assert_eq!(b.params.fader_db.load(), -9.0);
    let (_, loaded) = round_trip(&engine, &show);
    let l = loaded.snippet(0, 0).unwrap();
    assert_eq!((l.parameter_count(), l.dcas, l.scope()), (4, 1 << 4, show.snippet(0, 0).unwrap().scope()));

    // CLEAR ALL drops a recording; SAVE then has nothing to store.
    show.start_recording(&engine);
    a.params.fader_db.store(1.0);
    show.clear_recording();
    assert_eq!(show.add_recorded_snippet(&engine, &mut np, 0, "None", None), -1);
}

#[test]
fn snippet_items_and_partial_load() {
    let engine = Engine::new(0);
    let mut np = NoPlugins;
    engine.set_num_channels(3);
    let mut show = Show::new();
    show.add_scene(&engine, &mut np, "A", -1);
    let (a, b) = (ch(&engine, 0), ch(&engine, 1));
    a.params.fader_db.store(-6.0);
    show.add_snippet(&engine, &mut np, 0, "S", section::FADER, vec![StripRef::of(&a)]);

    // ADD ITEMS: channel 2's gate and DCA 1.
    b.params.gate.on.store(true, Relaxed);
    engine.controls.dcas[0].fader_db.store(-2.0);
    let items = Scope { dcas: 1, ..Scope::strips(section::GATE, vec![StripRef::of(&b)]) };
    show.add_snippet_items(&engine, &mut np, 0, 0, &items);
    let sn = show.snippet(0, 0).unwrap();
    assert_eq!(sn.strips, vec![StripRef::of(&a), StripRef::of(&b)]);
    assert_eq!((sn.sections, sn.dcas), (section::FADER | section::GATE, 1));

    b.params.gate.on.store(false, Relaxed);
    b.params.fader_db.store(-20.0);
    a.params.fader_db.store(-20.0);
    engine.controls.dcas[0].fader_db.store(-20.0);
    // PARTIAL LOAD: only the gate part.
    show.recall_snippet_with(&engine, &mut np, 0, 0, Some(&Scope::console(section::GATE)));
    assert!(b.params.gate.on.load(Relaxed));
    assert_eq!(a.params.fader_db.load(), -20.0);
    assert_eq!(engine.controls.dcas[0].fader_db.load(), -20.0, "DCA levels recall with FADER");
    show.recall_snippet(&engine, &mut np, 0, 0);
    assert_eq!(a.params.fader_db.load(), -6.0);
    assert_eq!(b.params.fader_db.load(), -20.0, "only channel 2's gate was added");
    assert_eq!(engine.controls.dcas[0].fader_db.load(), -2.0);

    // REMOVE ITEMS: the faders (of every strip) and DCA 1.
    show.remove_snippet_items(0, 0, &Scope { dcas: 1, ..Scope::strips(section::FADER, vec![]) });
    let sn = show.snippet(0, 0).unwrap();
    assert_eq!((sn.strips.clone(), sn.sections, sn.dcas), (vec![StripRef::of(&b)], section::GATE, 0));
}

// ---- spill ----------------------------------------------------------------------------

#[test]
fn spill_layers_follow_the_console() {
    use livedaw_dsp::wing::FX_MODELS;
    use livedaw_session::layers::{spill_faders, SpillKind, RIGHT, SPILL_ID};

    let engine = Engine::new(0);
    engine.set_num_channels(4);
    let (a, b, c) = (ch(&engine, 0), ch(&engine, 1), ch(&engine, 2));
    let strip = |s: &Strip| FaderRef::Strip(StripRef::of(s));

    // DCA spill: the strips assigned to the DCA, in console order.
    a.params.dca_mask.store(0b100, Relaxed);
    c.params.dca_mask.store(0b110, Relaxed);
    let bus = engine.bus_in_slot(3).unwrap();
    bus.params.dca_mask.store(0b100, Relaxed);
    assert_eq!(spill_faders(&engine, &SpillKind::Dca(2)), vec![strip(&a), strip(&c), strip(&bus)]);
    assert!(spill_faders(&engine, &SpillKind::Dca(99)).is_empty());

    // Automix spill: the channels in group Y (members only: INS 2 ON).
    b.params.post_insert.mode.store(2, Relaxed);
    c.params.post_insert.mode.store(2, Relaxed);
    b.params.post_insert.on.store(true, Relaxed);
    assert_eq!(spill_faders(&engine, &SpillKind::Automix(1)), vec![strip(&b)]);

    // Bus spill: the send levels of the sources sending to the bus.
    a.sends.get_or_create(3).unwrap().enabled.store(true, Relaxed);
    let to = StripRef::new(StripKind::Bus, 3);
    assert_eq!(spill_faders(&engine, &SpillKind::Bus(3)), vec![FaderRef::Send { from: StripRef::of(&a), to }]);

    // A strip's sends: every bus, main and matrix.
    let sends = spill_faders(&engine, &SpillKind::Sends(StripRef::of(&a)));
    assert_eq!(sends.len(), 16 + 4 + 8);
    assert_eq!(sends[0], FaderRef::Send { from: StripRef::of(&a), to: StripRef::new(StripKind::Bus, 0) });

    // FX spill: the device's continuous parameters.
    assert!(spill_faders(&engine, &SpillKind::Fx(9)).is_empty(), "an empty slot has none");
    let geq = livedaw_dsp::wing::model_index(FX_MODELS, "GEQ").unwrap();
    engine.set_fx_model(9, geq, true);
    let params = spill_faders(&engine, &SpillKind::Fx(9));
    assert!(params.len() > 10);
    assert!(params.iter().all(|p| matches!(p, FaderRef::FxParam { slot: 9, .. })));

    // The section shows the spill until it ends; the right section spills to the center.
    let mut show = Show::new();
    let target = layers::spill_target(Some(RIGHT));
    assert_eq!(target, CENTER);
    show.layers.set_offset(CENTER, 4);
    show.layers.start_spill(target, SpillKind::Dca(2));
    let shown = show.layers.selected_layer(&engine, CENTER);
    assert_eq!((shown.id.as_str(), shown.name.as_str(), shown.faders.len()), (SPILL_ID, "DCA 3 SPILL", 3));
    assert_eq!(show.layers.find_layer(&engine, CENTER, SPILL_ID).faders.len(), 3);
    show.layers.end_spill(CENTER);
    assert_eq!((show.layers.selected_layer(&engine, CENTER).id.as_str(), show.layers.offset(CENTER)), ("dca-1", 4));
    assert_eq!(show.layers.selected_layer(&engine, LEFT).id, "in-1");
}

// ---- LIBRARY presets (FX, CHAN) ----------------------------------------------------------

#[test]
fn fx_presets_round_trip_into_any_slot() {
    use livedaw_dsp::wing::FX_MODELS;
    use livedaw_session::presets::{self, PresetType};
    let fx = |t: &str| model_index(FX_MODELS, t).unwrap();
    let engine = Engine::new(0);
    let mut host = FakeHost::default();
    assert!(engine.set_fx_model(2, fx("HALL"), true));
    engine.controls.fx[2].param("dcy").unwrap().store(4.25);
    engine.controls.fx[2].mix.store(35.0);

    let preset = presets::capture_fx(&engine, &mut Bridge { host: &mut host, engine: &engine }, 2, "Long hall").unwrap();
    let info = presets::info(&preset).unwrap();
    assert_eq!((info.kind, info.name.as_str()), (PresetType::Fx, "Long hall"));

    // Through a file, into slot 5.
    let dir = std::env::temp_dir().join(format!("livedaw-presets-{}", std::process::id()));
    let path = dir.join("hall.lfx");
    presets::save(&path, &preset).unwrap();
    let loaded = presets::load(&path).unwrap();
    let warnings = presets::apply_fx(&engine, &mut Bridge { host: &mut host, engine: &engine }, &loaded, 5).unwrap();
    assert!(warnings.is_empty(), "{warnings:?}");
    assert_eq!(engine.controls.fx[5].model_index(), fx("HALL"));
    assert_eq!(engine.controls.fx[5].param("dcy").unwrap().load(), 4.25);
    assert_eq!(engine.controls.fx[5].mix.load(), 35.0);

    // A premium device doesn't go into slots 9-16.
    let w = presets::apply_fx(&engine, &mut Bridge { host: &mut host, engine: &engine }, &loaded, 12).unwrap();
    assert_eq!(w.len(), 1);
    assert_ne!(engine.controls.fx[12].model_index(), fx("HALL"));

    // A plugin's state only goes into the same plugin.
    host.load(&engine, 0, "Comp", "state-a");
    let plugin = presets::capture_fx(&engine, &mut Bridge { host: &mut host, engine: &engine }, 0, "Comp A").unwrap();
    assert_eq!(presets::info(&plugin).unwrap().detail, "Comp");
    host.load(&engine, 1, "Comp", "state-b");
    host.load(&engine, 3, "Other", "o");
    let ok = presets::apply_fx(&engine, &mut Bridge { host: &mut host, engine: &engine }, &plugin, 1).unwrap();
    assert!(ok.is_empty());
    assert_eq!(host.plugins[&1].1, "state-a");
    let mismatch = presets::apply_fx(&engine, &mut Bridge { host: &mut host, engine: &engine }, &plugin, 3).unwrap();
    assert_eq!(mismatch.len(), 1);
    assert_eq!(host.plugins[&3].1, "o");
    assert!(presets::apply_fx(&engine, &mut NoPlugins, &plugin, 99).is_err());
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn channel_presets_load_onto_other_strips_within_their_scope() {
    use livedaw_session::presets;
    let engine = Engine::new(0);
    let mut np = NoPlugins;
    engine.set_num_channels(4);
    let (a, b) = (ch(&engine, 0), ch(&engine, 1));
    a.set_name("Kick");
    a.params.gate.on.store(true, Relaxed);
    a.params.input.trim_db.store(6.0);
    a.params.fader_db.store(-7.0);
    a.params.input_left.store(9, Relaxed);

    let preset = presets::capture_channel(&engine, &mut np, &StripRef::of(&a), section::GATE | section::PREAMP, "Kick").unwrap();
    assert_eq!(presets::info(&preset).unwrap().scope, section::GATE | section::PREAMP);
    let r = presets::apply_channel(&engine, &mut np, &preset, &StripRef::of(&b), None).unwrap();
    assert_eq!(r.strips_applied, 1);
    assert!(b.params.gate.on.load(Relaxed));
    assert_eq!(b.params.input.trim_db.load(), 6.0);
    assert_eq!(b.params.fader_db.load(), 0.0, "the fader is outside the preset's scope");
    assert_eq!(b.params.input_left.load(Relaxed), 1, "nor the input patch");
    assert_ne!(b.name(), "Kick");

    // UPDATE SCOPE: the fader and name too; through a file, onto an aux.
    let mut updated = preset.clone();
    presets::set_channel_scope(&mut updated, section::FADER | section::NAME);
    let dir = std::env::temp_dir().join(format!("livedaw-chan-{}", std::process::id()));
    let path = dir.join("kick.lchn");
    presets::save(&path, &updated).unwrap();
    let loaded = presets::load(&path).unwrap();
    let aux = engine.strip_at(StripKind::Aux, 2).unwrap();
    presets::apply_channel(&engine, &mut np, &loaded, &StripRef::of(&aux), None).unwrap();
    assert_eq!((aux.params.fader_db.load(), aux.name()), (-7.0, "Kick".to_owned()));

    // Inputs don't load onto buses, mains or matrices; missing strips and files fail.
    let bus = StripRef::new(StripKind::Bus, 0);
    assert!(presets::apply_channel(&engine, &mut np, &loaded, &bus, None).is_err());
    assert!(presets::apply_channel(&engine, &mut np, &loaded, &StripRef::new(StripKind::Channel, 77), None).is_err());
    let _ = std::fs::remove_dir_all(&dir);
    assert!(presets::load(&dir.join("missing.lchn")).is_err());
}

// ---- LIBRARY scene and snippet files -----------------------------------------------------

#[test]
fn scene_and_snippet_files_round_trip_into_another_show() {
    use livedaw_session::library::{self, FileType};
    let engine = Engine::new(0);
    let mut np = NoPlugins;
    engine.set_num_channels(4);
    let (a, b) = (ch(&engine, 0), ch(&engine, 1));
    a.params.fader_db.store(-6.0);
    b.params.fader_db.store(-12.0);

    let mut show = Show::new();
    let verse = show.add_scene(&engine, &mut np, "Verse", -1);
    {
        let scene = show.scene_mut(verse).unwrap();
        scene.notes = "Capo 2".into();
        scene.fade_seconds = 2.5;
        scene.tag = 7;
        scene.link_next = true;
        scene.set_recall_scope(&Scope::strips(section::FADER, vec![StripRef::of(&a)]));
    }
    let solo = show.add_snippet(&engine, &mut np, verse, "Solo", section::FADER, vec![StripRef::of(&b)]);
    show.snippet_mut(verse, solo).unwrap().fire_with_scene = true;

    let dir = std::env::temp_dir().join(format!("livedaw-library-{}", std::process::id()));
    let (scene_path, snippet_path) = (dir.join("Verse.lsnap"), dir.join("Solo.lsnip"));
    library::save(&scene_path, &library::scene_file(&show, verse).unwrap()).unwrap();
    library::save(&snippet_path, &library::snippet_file(&show, verse, solo).unwrap()).unwrap();

    // The headers name what the files hold.
    let scene_file = library::load(&scene_path).unwrap();
    let info = library::info(&scene_file).unwrap();
    assert_eq!((info.kind, info.name.as_str(), info.count), (FileType::Scene, "Verse", 1));
    let snippet_file = library::load(&snippet_path).unwrap();
    let info = library::info(&snippet_file).unwrap();
    assert_eq!((info.kind, info.name.as_str()), (FileType::Snippet, "Solo"));
    assert!(info.count > 0);

    // Into another show, between its scenes: the settings come along.
    let mut other = Show::new();
    other.add_scene(&engine, &mut np, "Intro", -1);
    other.add_scene(&engine, &mut np, "Outro", -1);
    assert_eq!(other.current_scene(), 1);
    let at = library::import_scene(&mut other, &scene_file, 1).unwrap();
    assert_eq!((at, other.num_scenes(), other.current_scene()), (1, 3, 2));
    let imported = other.scene(at).unwrap();
    assert_eq!((imported.name.as_str(), imported.notes.as_str(), imported.fade_seconds), ("Verse", "Capo 2", 2.5));
    assert_eq!((imported.tag, imported.link_next, imported.scope), (7, true, section::FADER));
    assert_eq!(imported.scope_strips, Some(vec![StripRef::of(&a)]));
    assert_eq!(imported.snippets.len(), 1);
    assert!(imported.snippets[0].fire_with_scene);
    let sn = library::import_snippet(&mut other, &snippet_file, 0).unwrap();
    assert_eq!(other.snippet(0, sn).unwrap().name, "Solo");
    assert!(library::import_snippet(&mut other, &snippet_file, 9).is_err());

    // They recall what was saved.
    other.scene_mut(at).unwrap().fade_seconds = 0.0;
    other.scene_mut(at).unwrap().link_next = false;
    a.params.fader_db.store(0.0);
    b.params.fader_db.store(0.0);
    other.recall_scene(&engine, &mut np, at);
    assert_eq!(a.params.fader_db.load(), -6.0);
    assert_eq!(b.params.fader_db.load(), -12.0, "the scene's own snippet fires with it");
    b.params.fader_db.store(0.0);
    other.recall_snippet(&engine, &mut np, 0, sn);
    assert_eq!(b.params.fader_db.load(), -12.0);

    // Not library files, or the wrong kind.
    assert!(library::import_scene(&mut other, &snippet_file, 0).is_err());
    let lfx = dir.join("x.lfx");
    std::fs::write(&lfx, "<LiveDAWPreset type=\"fx\" version=\"1\"/>").unwrap();
    assert!(library::load(&lfx).is_err());
    assert!(library::load(&dir.join("missing.lsnap")).is_err());
    let _ = std::fs::remove_dir_all(&dir);
}
