//! Show / scene / snippet / console-state tests against a real engine (no audio device).

use livedaw_engine::{Engine, InsertCallbacks, Strip, StripKind};
use livedaw_session::console::{self, find_strip};
use livedaw_session::xml::{self, Element};
use livedaw_session::*;
use std::collections::HashMap;
use std::ffi::c_void;
use std::sync::atomic::Ordering::Relaxed;
use std::time::{Duration, Instant};

unsafe extern "C" fn pass_through(_: *mut c_void, _: *const *mut f32, _: i32, _: i32) -> i32 {
    0
}

/// Pretend plugin host: a "plugin" is a name and a state string per (strip, slot).
#[derive(Default)]
struct FakeHost {
    plugins: HashMap<(usize, usize), (String, String)>,
    fail_to_create: Vec<String>,
}

impl FakeHost {
    fn load(&mut self, engine: &Engine, strip: &Strip, slot: usize, name: &str, state: &str) {
        let cb = InsertCallbacks { process: pass_through, release: None, ctx: std::ptr::null_mut() };
        strip.inserts.set(slot, Some(cb), engine.garbage());
        self.plugins.insert((strip as *const Strip as usize, slot), (name.into(), state.into()));
    }

    fn get(&self, strip: &Strip, slot: usize) -> Option<&(String, String)> {
        self.plugins.get(&(strip as *const Strip as usize, slot))
    }
}

struct Bridge<'a> {
    host: &'a mut FakeHost,
    engine: &'a Engine,
}

impl PluginBridge for Bridge<'_> {
    fn capture_insert(&mut self, strip: &Strip, slot: usize) -> Option<CapturedInsert> {
        let (name, state) = self.host.get(strip, slot)?;
        let mut desc = Element::new("PLUGIN");
        desc.set("name", name).set("format", "Fake");
        Some(CapturedInsert { description: desc.to_compact(), state: state.clone() })
    }

    fn restore_insert(
        &mut self,
        strip: &Strip,
        slot: usize,
        mode: InsertMode,
        description: &str,
        state: &str,
    ) -> Result<(), RestoreError> {
        let desc = xml::parse(description).map_err(|e| RestoreError::Failed(e.to_string()))?;
        let name = desc.str_or("name", "").to_owned();
        match mode {
            InsertMode::StateOnly => match self.host.plugins.get_mut(&(strip as *const Strip as usize, slot)) {
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
                self.host.load(self.engine, strip, slot, &name, state);
                Ok(())
            }
        }
    }
}

fn input(engine: &Engine, i: usize) -> std::sync::Arc<Strip> {
    engine.input_at(i).unwrap()
}

#[test]
fn console_survives_a_show_round_trip() {
    let engine = Engine::new(0);
    let mut host = FakeHost::default();
    engine.set_num_inputs(3);
    let aux = engine.add_bus(StripKind::Aux, 79).unwrap();
    let group = engine.add_bus(StripKind::Group, -1).unwrap();

    let kick = input(&engine, 0);
    kick.set_name("Kick \"in\" & <out>");
    kick.set_colour(0xff112233);
    let p = &kick.params;
    p.hpf_enabled.store(true, Relaxed);
    p.gate.threshold_db.store(-33.0);
    p.eq.bands[2].gain_db.store(-4.5);
    p.comp.ratio.store(6.0);
    p.stereo.store(true, Relaxed);
    p.recall_safe.store(true, Relaxed);
    kick.sends.get_or_create(79).unwrap().enabled.store(true, Relaxed);
    kick.sends.get_or_create(79).unwrap().level_db.store(-3.0);
    host.load(&engine, &kick, 2, "Reverb", "ABC=");
    kick.inserts.set_bypassed(2, true);
    engine.controls.solo_afl.store(true, Relaxed);
    engine.controls.output_ceiling_db.store(-1.5);

    let mut show = Show::new();
    show.name = "Gig".into();
    show.layers.add_user_layer("Drums", vec![StripRef::of(&kick), StripRef::of(&aux)]);
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
    assert_eq!(loaded.layers.user_layers()[0].strips.len(), 2);
    assert_eq!(e2.num_inputs(), 3);
    assert_eq!(e2.num_buses(), 2);
    assert_eq!(e2.bus_in_slot(79).unwrap().kind(), StripKind::Aux);
    assert_eq!(e2.bus_in_slot(group.id()).unwrap().kind(), StripKind::Group);

    let k = input(&e2, 0);
    assert_eq!(k.name(), "Kick \"in\" & <out>");
    assert_eq!(k.colour(), 0xff112233);
    assert!(k.params.hpf_enabled.load(Relaxed));
    assert_eq!(k.params.gate.threshold_db.load(), -33.0);
    assert_eq!(k.params.eq.bands[2].gain_db.load(), -4.5);
    assert_eq!(k.params.comp.ratio.load(), 6.0);
    assert!(k.params.stereo.load(Relaxed));
    assert!(k.params.recall_safe.load(Relaxed));
    let send = k.sends.find(79).unwrap();
    assert!(send.enabled.load(Relaxed));
    assert_eq!(send.level_db.load(), -3.0);
    assert_eq!(host2.get(&k, 2), Some(&("Reverb".to_string(), "ABC=".to_string())));
    assert!(k.inserts.is_bypassed(2));
    assert!(e2.controls.solo_afl.load(Relaxed));
    assert_eq!(e2.controls.output_ceiling_db.load(), -1.5);
    assert_eq!(input(&e2, 1).name(), "Ch 2");
}

#[test]
fn scenes_respect_scope_and_recall_safe() {
    let engine = Engine::new(0);
    let mut np = NoPlugins;
    engine.set_num_inputs(2);
    let mut show = Show::new();
    show.add_scene(&engine, &mut np, "A", -1);

    let (a, b) = (input(&engine, 0), input(&engine, 1));
    a.params.fader_db.store(-20.0);
    a.params.input_left.store(7, Relaxed);
    b.params.fader_db.store(-30.0);
    b.params.recall_safe.store(true, Relaxed);

    let r = show.recall_scene(&engine, &mut np, 0);
    assert_eq!(r.strips_skipped_safe, 1);
    assert_eq!(a.params.fader_db.load(), 0.0);
    assert_eq!(a.params.input_left.load(Relaxed), 7, "routing is outside the default scope");
    assert_eq!(b.params.fader_db.load(), -30.0);

    show.scene_mut(0).unwrap().scope = section::ALL;
    show.recall_scene(&engine, &mut np, 0);
    assert_eq!(a.params.input_left.load(Relaxed), 0);
}

#[test]
fn snippets_touch_only_their_strips_and_sections() {
    let engine = Engine::new(0);
    let mut np = NoPlugins;
    engine.set_num_inputs(3);
    let mut show = Show::new();
    show.add_scene(&engine, &mut np, "A", -1);
    let (a, b) = (input(&engine, 0), input(&engine, 1));
    a.params.fader_db.store(-6.0);
    a.params.mute.store(true, Relaxed);
    b.params.fader_db.store(-9.0);
    show.add_snippet(&engine, &mut np, 0, "S", section::FADER, vec![StripRef::of(&a)]);

    a.params.fader_db.store(-50.0);
    a.params.gate.enabled.store(true, Relaxed);
    a.params.recall_safe.store(true, Relaxed);
    b.params.fader_db.store(-50.0);
    let r = show.recall_snippet(&engine, &mut np, 0, 0);
    assert_eq!(r.strips_applied, 1);
    assert_eq!(a.params.fader_db.load(), -6.0, "snippets ignore recall safe");
    assert!(a.params.mute.load(Relaxed));
    assert!(a.params.gate.enabled.load(Relaxed), "gate is not in the snippet");
    assert_eq!(b.params.fader_db.load(), -50.0);

    // Auto-fire with the scene: the scene sets 0 dB, the snippet overrides it.
    a.params.recall_safe.store(false, Relaxed);
    show.snippet_mut(0, 0).unwrap().fire_with_scene = true;
    show.recall_scene(&engine, &mut np, 0);
    assert_eq!(a.params.fader_db.load(), -6.0);
    assert_eq!(b.params.fader_db.load(), 0.0);
}

#[test]
fn fades_glide_and_survive_removed_strips() {
    let engine = Engine::new(0);
    let mut np = NoPlugins;
    engine.set_num_inputs(2);
    let aux = engine.add_bus(StripKind::Aux, -1).unwrap();
    let (a, b) = (input(&engine, 0), input(&engine, 1));
    a.sends.get_or_create(aux.id() as usize).unwrap().level_db.store(-10.0);

    let mut show = Show::new();
    show.add_scene(&engine, &mut np, "A", -1);
    show.scene_mut(0).unwrap().fade_seconds = 2.0;
    a.params.fader_db.store(-40.0);
    b.params.fader_db.store(-40.0);
    a.sends.find(aux.id() as usize).unwrap().level_db.store(-30.0);

    let start = Instant::now();
    show.recall_scene(&engine, &mut np, 0);
    assert!(show.is_fading());
    assert_eq!(a.params.fader_db.load(), -40.0, "nothing jumps");
    assert!(show.tick_at(start + Duration::from_millis(1000)));
    let mid = a.params.fader_db.load();
    assert!(mid > -30.0 && mid < -10.0, "{mid}");
    let send_mid = a.sends.find(aux.id() as usize).unwrap().level_db.load();
    assert!(send_mid > -30.0 && send_mid < -10.0, "{send_mid}");

    // Remove a strip mid-fade: the fade keeps it alive, nothing dangles.
    engine.remove_strip(std::sync::Arc::as_ptr(&b));
    drop(b);
    engine.collect();
    assert!(!show.tick_at(start + Duration::from_secs(5)));
    assert_eq!(a.params.fader_db.load(), 0.0);
    assert_eq!(a.sends.find(aux.id() as usize).unwrap().level_db.load(), -10.0);

    // Cancel jumps to the targets.
    a.params.fader_db.store(-40.0);
    show.recall_scene(&engine, &mut np, 0);
    show.cancel_fade();
    assert!(!show.is_fading());
    assert_eq!(a.params.fader_db.load(), 0.0);
}

#[test]
fn recall_restores_plugin_state_but_never_swaps_plugins() {
    let engine = Engine::new(0);
    let mut host = FakeHost::default();
    engine.set_num_inputs(1);
    let ch = input(&engine, 0);
    host.load(&engine, &ch, 0, "Comp", "one");
    let mut show = Show::new();
    show.add_scene(&engine, &mut Bridge { host: &mut host, engine: &engine }, "A", -1);

    host.plugins.get_mut(&(std::sync::Arc::as_ptr(&ch) as usize, 0)).unwrap().1 = "two".into();
    let r = show.recall_scene(&engine, &mut Bridge { host: &mut host, engine: &engine }, 0);
    assert!(r.warnings.is_empty());
    assert_eq!(host.get(&ch, 0).unwrap().1, "one");

    host.load(&engine, &ch, 0, "Other", "x");
    let r = show.recall_scene(&engine, &mut Bridge { host: &mut host, engine: &engine }, 0);
    assert_eq!(r.warnings, vec!["Ch 1 insert 1: expected Comp, not recalled".to_string()]);
    assert_eq!(host.get(&ch, 0).unwrap().0, "Other");
}

#[test]
fn show_files_are_atomic_with_backup_and_safe_mode() {
    let dir = std::env::temp_dir().join(format!("livedaw-session-test-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let path = dir.join("gig.lshow");

    let engine = Engine::new(0);
    let mut host = FakeHost::default();
    engine.set_num_inputs(2);
    host.load(&engine, &input(&engine, 0), 1, "Delay", "S");
    let mut show = Show::new();
    show.name = "First".into();
    show.save_to_file(&engine, &mut Bridge { host: &mut host, engine: &engine }, &path).unwrap();
    show.name = "Second".into();
    show.save_to_file(&engine, &mut Bridge { host: &mut host, engine: &engine }, &path).unwrap();
    assert!(show::backup_path(&path).is_file());

    // Corrupt the main file: loading falls back to the backup (the first save).
    std::fs::write(&path, "<LiveDAWShow version=\"2\"><Cons").unwrap();
    let e2 = Engine::new(0);
    let mut host2 = FakeHost::default();
    let mut loaded = Show::new();
    loaded.load_from_file(&e2, &mut Bridge { host: &mut host2, engine: &e2 }, &path, false).unwrap();
    assert_eq!(loaded.name, "First");
    assert_eq!(e2.num_inputs(), 2);
    assert!(host2.plugins.is_empty(), "safe mode instantiates nothing");
    assert!(!e2.input_at(0).unwrap().inserts.is_loaded(1));

    // A plugin that fails to load is reported, the rest of the show still loads.
    host2.fail_to_create.push("Delay".into());
    let warnings = loaded.load_from_file(&e2, &mut Bridge { host: &mut host2, engine: &e2 }, &show::backup_path(&path), true);
    assert_eq!(warnings.unwrap(), vec!["Ch 1: Delay (not installed)".to_string()]);

    assert!(Show::new().load_from_file(&e2, &mut NoPlugins, &dir.join("missing.lshow"), true).is_err());
    std::fs::remove_dir_all(&dir).unwrap();
}

#[test]
fn legacy_and_foreign_files() {
    let engine = Engine::new(0);
    let mut np = NoPlugins;
    let mut show = Show::new();

    // Version 1 "session": a bare console; a right input meant stereo.
    let v1 = r#"<?xml version="1.0"?><LiveDAWSession><Inputs>
        <Strip kind="input" id="5" name="Gtr" inL="0" inR="1"/><Strip kind="input" id="9" inL="2"/>
        </Inputs></LiveDAWSession>"#;
    show.load_from_text(&engine, &mut np, v1, true).unwrap();
    assert_eq!(show.name, "Imported session");
    let gtr = find_strip(&engine, &StripRef::new(StripKind::Input, 5)).unwrap();
    assert_eq!(gtr.name(), "Gtr");
    assert!(gtr.params.stereo.load(Relaxed));
    assert!(!find_strip(&engine, &StripRef::new(StripKind::Input, 9)).unwrap().params.stereo.load(Relaxed));

    assert!(show.load_from_text(&engine, &mut np, "<LiveDAWShow version=\"99\"><Console/></LiveDAWShow>", true).is_err());
    assert!(show.load_from_text(&engine, &mut np, "<Something/>", true).is_err());
    assert!(show.load_from_text(&engine, &mut np, "not xml", true).is_err());
    assert_eq!(engine.num_inputs(), 2, "a rejected file leaves the console alone");
}

#[test]
fn capture_and_apply_by_section() {
    let engine = Engine::new(0);
    let mut np = NoPlugins;
    engine.set_num_inputs(2);
    let a = input(&engine, 0);
    a.params.eq.bands[1].gain_db.store(5.0);
    a.params.comp.threshold_db.store(-12.0);
    let state = console::capture(&engine, section::EQ, None, &mut np);
    a.params.eq.bands[1].gain_db.store(0.0);
    a.params.comp.threshold_db.store(-30.0);

    // Asking for more than was captured only touches the captured part.
    let r = console::apply(&engine, &state, section::ALL, None, true, None, &mut np);
    assert_eq!(r.strips_applied, 3); // master + 2 inputs
    assert_eq!(a.params.eq.bands[1].gain_db.load(), 5.0);
    assert_eq!(a.params.comp.threshold_db.load(), -30.0);

    // Strips missing from the console are counted, not fatal.
    engine.set_num_inputs(1);
    let r = console::apply(&engine, &state, section::ALL, None, true, None, &mut np);
    assert_eq!(r.strips_missing, 1);
}
