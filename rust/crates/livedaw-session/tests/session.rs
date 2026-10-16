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
    assert!(show.tick_at(start + Duration::from_millis(1000)));
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
    assert!(!show.tick_at(start + Duration::from_secs(5)));
    assert_eq!(a.params.fader_db.load(), 0.0);
    assert_eq!(a.sends.find(bus.id() as usize).unwrap().level_db.load(), -10.0);

    // Cancel jumps to the targets.
    a.params.fader_db.store(-40.0);
    show.recall_scene(&engine, &mut np, 0);
    show.cancel_fade();
    assert!(!show.is_fading());
    assert_eq!(a.params.fader_db.load(), 0.0);
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
#[ignore = "needs the FX DSP families"]
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
