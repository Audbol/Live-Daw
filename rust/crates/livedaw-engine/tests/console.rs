//! Console-level features: SOLO MODE (LIVE / STUDIO / SIP), MUTEGROUP/SIP
//! OVERRIDE, talkback, the two monitor paths, MON DCAs and sources.

mod common;
use common::{set_channels, unity_console};
use livedaw_engine::params::StripKind;
use livedaw_engine::solo::{mute_state, solo_mode};
use livedaw_engine::sources::{source_index, source_mode};
use livedaw_engine::talk::talk_tag;
use livedaw_engine::{Engine, Strip, PLAYBACK_SOURCE_BASE};
use std::sync::atomic::Ordering::Relaxed;
use std::sync::Arc;

const BLOCK: usize = 64;

/// Drives an engine with constant (DC) inputs and captures its outputs.
struct Rig {
    engine: Arc<Engine>,
    inputs: Vec<Vec<f32>>,
    outputs: Vec<Vec<f32>>,
}

impl Rig {
    fn new(num_ins: usize, num_outs: usize) -> Self {
        let engine = Arc::new(Engine::new(0));
        unity_console(&engine);
        let in_map: Vec<i32> = (0..num_ins as i32).collect();
        let out_map: Vec<i32> = (0..num_outs as i32).collect();
        unsafe { engine.prepare(48000.0, BLOCK, &in_map, &out_map) };
        Self { engine, inputs: vec![vec![0.0; BLOCK]; num_ins], outputs: vec![vec![0.0; BLOCK]; num_outs] }
    }

    fn set_input(&mut self, ch: usize, v: f32) {
        self.inputs[ch].fill(v);
    }

    fn run(&mut self, blocks: usize) {
        let ins: Vec<*const f32> = self.inputs.iter().map(|v| v.as_ptr()).collect();
        let outs: Vec<*mut f32> = self.outputs.iter_mut().map(|v| v.as_mut_ptr()).collect();
        for _ in 0..blocks {
            unsafe { self.engine.process(&ins, &outs, BLOCK) };
        }
    }

    fn out(&self, ch: usize) -> f32 {
        *self.outputs[ch].last().unwrap()
    }

    fn ch(&self, i: usize) -> Arc<Strip> {
        self.engine.channel_at(i).unwrap()
    }

    fn strip(&self, kind: StripKind, i: usize) -> Arc<Strip> {
        self.engine.strip_at(kind, i).unwrap()
    }
}

fn close(a: f32, b: f32, tol: f32) -> bool {
    (a - b).abs() <= tol
}

/// Channels at centre (unity on each side: the WING's pan law), each on its
/// own input with its own level.
fn channels(rig: &mut Rig, levels: &[f32]) {
    set_channels(&rig.engine, levels.len());
    for (i, &v) in levels.iter().enumerate() {
        rig.set_input(i, v);
    }
}

#[test]
fn sip_mutes_every_input_that_is_neither_soloed_nor_solo_safe() {
    let mut rig = Rig::new(4, 2);
    channels(&mut rig, &[0.1, 0.2, 0.4]);
    let engine = rig.engine.clone();
    let c = &engine.controls;
    let aux = rig.strip(StripKind::Aux, 0);
    aux.params.input_left.store(3, Relaxed);
    rig.set_input(3, 0.05);
    rig.run(4);
    assert!(close(rig.out(0), 0.75, 1e-5), "{}", rig.out(0));

    // STUDIO: a solo leaves the mix alone.
    rig.ch(0).params.solo.store(true, Relaxed);
    rig.run(4);
    assert!(close(rig.out(0), 0.75, 1e-5));
    assert!(!c.sip_active.load(Relaxed));

    // SIP: channels 2 and 3 and the aux are muted, a solo-safe channel stays.
    c.solo_mode.store(solo_mode::SIP, Relaxed);
    rig.ch(1).params.solo_safe.store(true, Relaxed);
    rig.run(4);
    assert!(c.sip_active.load(Relaxed));
    assert!(close(rig.out(0), 0.3, 1e-5), "{}", rig.out(0));
    assert_eq!(rig.ch(2).console.mute_state(), mute_state::GROUP);
    assert_eq!(aux.console.mute_state(), mute_state::GROUP);
    assert_eq!(rig.ch(0).console.mute_state(), mute_state::OFF);
    assert_eq!(rig.ch(1).console.mute_state(), mute_state::OFF);
    // Buses, mains and matrices aren't muted by SIP.
    assert_eq!(rig.strip(StripKind::Main, 0).console.mute_state(), mute_state::OFF);

    // A soloed DCA solos its members in place.
    rig.ch(0).params.solo.store(false, Relaxed);
    rig.ch(2).params.dca_mask.store(1, Relaxed);
    c.dcas[0].solo.store(true, Relaxed);
    rig.run(4);
    assert!(close(rig.out(0), 0.6, 1e-5), "{}", rig.out(0));

    // Soloing a bus doesn't mute the inputs; clearing the solos restores the mix.
    c.dcas[0].solo.store(false, Relaxed);
    rig.strip(StripKind::Bus, 0).params.solo.store(true, Relaxed);
    rig.run(4);
    assert!(!c.sip_active.load(Relaxed));
    assert!(close(rig.out(0), 0.75, 1e-5));
    rig.strip(StripKind::Bus, 0).params.solo.store(false, Relaxed);
    rig.run(4);
    assert!(close(rig.out(0), 0.75, 1e-5));
}

#[test]
fn studio_solos_to_the_selected_path_and_live_to_each_strips_path() {
    let mut rig = Rig::new(2, 6);
    channels(&mut rig, &[0.5, 0.25]);
    let engine = rig.engine.clone();
    let c = &engine.controls;
    c.monitor_output_left.store(2, Relaxed); // speakers (A)
    c.phones.output_left.store(4, Relaxed); // phones (B)
    rig.run(4);
    assert!(close(rig.out(2), 0.75, 1e-5) && close(rig.out(4), 0.75, 1e-5), "both follow Main 1");

    // STUDIO, solo bus SPEAKERS (B): only the speakers hear the solo.
    c.solo_bus.store(1, Relaxed);
    rig.ch(1).params.solo.store(true, Relaxed);
    rig.run(4);
    assert!(close(rig.out(2), 0.25, 1e-5), "{}", rig.out(2));
    assert!(close(rig.out(4), 0.75, 1e-5), "{}", rig.out(4));

    // PHONES (A): the other way round; SRC MIX keeps some of the source.
    c.solo_bus.store(0, Relaxed);
    c.phones.source.source_mix_db.store(-6.0206);
    rig.run(4);
    assert!(close(rig.out(2), 0.75, 1e-5));
    assert!(close(rig.out(4), 0.25 + 0.375, 1e-4), "{}", rig.out(4));
    c.phones.source.source_mix_db.store(-144.0);

    // PFL DIM attenuates pre-fader solos only.
    c.phones.source.pfl_dim_db.store(6.0206);
    rig.run(4);
    assert!(close(rig.out(4), 0.125, 1e-4), "{}", rig.out(4));
    c.solo_afl.store(true, Relaxed);
    rig.run(4);
    assert!(close(rig.out(4), 0.25, 1e-4), "AFL: {}", rig.out(4));
    c.solo_afl.store(false, Relaxed);
    c.phones.source.pfl_dim_db.store(0.0);

    // LIVE: each strip's SOLO BUS choice (A = phones, B = speakers).
    c.solo_mode.store(solo_mode::LIVE, Relaxed);
    rig.ch(0).params.solo.store(true, Relaxed);
    rig.ch(0).params.monitor_bus.store(0, Relaxed);
    rig.ch(1).params.monitor_bus.store(1, Relaxed);
    rig.run(4);
    assert!(close(rig.out(2), 0.25, 1e-5), "{}", rig.out(2));
    assert!(close(rig.out(4), 0.5, 1e-5), "{}", rig.out(4));

    // A path picked on MONITORS while soloing overrides them, until the solos clear.
    c.solo_bus.store(1, Relaxed);
    c.solo_bus_override.store(true, Relaxed);
    rig.run(4);
    assert!(close(rig.out(2), 0.75, 1e-5) && close(rig.out(4), 0.75, 1e-5));
    rig.ch(0).params.solo.store(false, Relaxed);
    rig.ch(1).params.solo.store(false, Relaxed);
    rig.run(2);
    assert!(!c.solo_bus_override.load(Relaxed));
}

#[test]
fn monitor_paths_have_their_own_source_level_dim_and_keys() {
    let mut rig = Rig::new(1, 6);
    channels(&mut rig, &[0.5]);
    let engine = rig.engine.clone();
    let c = &engine.controls;
    c.monitor_output_left.store(2, Relaxed);
    c.phones.output_left.store(4, Relaxed);
    c.phones.output_right.store(5, Relaxed);
    let ch = rig.ch(0);
    ch.sends.get_or_create(2).unwrap().enabled.store(true, Relaxed);
    ch.sends.get_or_create(2).unwrap().mode.store(1, Relaxed); // POST
    rig.strip(StripKind::Bus, 2).params.pan.store(-100.0);

    // PHONES plays BUS.3 at SRC LEVEL -6 dB.
    c.phones.source.source.store(4 + 8 + 2, Relaxed);
    c.phones.source.source_level_db.store(-6.0206);
    c.phones.level_db.store(-6.0206);
    rig.run(8);
    let bus = rig.strip(StripKind::Bus, 2);
    assert!(bus.meters.output_peak[0].load() > 0.0);
    assert!(close(rig.out(2), 0.5, 1e-5));
    assert!(close(rig.out(4), 0.5 * 0.25, 1e-4), "{}", rig.out(4));

    // DIM, MONO and mute act on the selected path only (PHONES).
    c.solo_bus.store(0, Relaxed);
    c.monitor_dim.store(true, Relaxed);
    c.phones.dim_db.store(6.0206);
    rig.run(4);
    assert!(close(rig.out(2), 0.5, 1e-5));
    assert!(close(rig.out(4), 0.0625, 1e-4), "{}", rig.out(4));
    c.monitor_dim.store(false, Relaxed);
    c.monitor_mono.store(true, Relaxed);
    rig.run(4);
    assert!(close(rig.out(4), 0.0625, 1e-4) && close(rig.out(5), 0.0625, 1e-4));
    c.monitor_mono.store(false, Relaxed);
    c.monitor_mute.store(true, Relaxed);
    rig.run(4);
    assert!(rig.out(4).abs() < 1e-7 && close(rig.out(2), 0.5, 1e-5));
    c.monitor_mute.store(false, Relaxed);
    assert!(c.phones.peak[0].exchange(0.0) > 0.0);
}

#[test]
fn dcas_named_mon_control_the_monitor_level() {
    let mut rig = Rig::new(1, 6);
    channels(&mut rig, &[0.5]);
    let engine = rig.engine.clone();
    let c = &engine.controls;
    c.monitor_output_left.store(2, Relaxed);
    c.phones.output_left.store(4, Relaxed);
    c.dcas[3].fader_db.store(-6.0206);
    engine.set_dca_name(3, "mon lvl");
    assert_eq!(engine.dca_monitor_paths(3), 3);
    rig.run(4);
    assert!(close(rig.out(2), 0.25, 1e-4) && close(rig.out(4), 0.25, 1e-4));
    assert!(close(rig.out(0), 0.5, 1e-5), "the DCA has no members");

    engine.set_dca_name(3, "MON A"); // the phones
    rig.run(4);
    assert!(close(rig.out(2), 0.5, 1e-5) && close(rig.out(4), 0.25, 1e-4));
    c.dcas[3].mute.store(true, Relaxed);
    rig.run(4);
    assert!(rig.out(4).abs() < 1e-7);
    engine.set_dca_name(3, "Drums");
    rig.run(4);
    assert!(close(rig.out(4), 0.5, 1e-5));
    engine.set_dca_name(5, "MON B");
    engine.reset();
    assert_eq!(engine.dca_monitor_paths(5), 0);
}

#[test]
fn mutegroup_sip_override_unmutes_until_the_group_changes() {
    let mut rig = Rig::new(2, 2);
    channels(&mut rig, &[0.5, 0.25]);
    let engine = rig.engine.clone();
    let c = &engine.controls;
    let ch = rig.ch(0);
    ch.params.mute_group_mask.store(0b11, Relaxed);
    c.mute_groups.store(1, Relaxed);
    rig.run(2);
    assert_eq!(ch.console.mute_state(), mute_state::GROUP);
    assert!(close(rig.out(0), 0.25, 1e-5));

    // Without the preference, MUTE mutes the strip itself.
    engine.press_mute(&ch);
    assert!(ch.params.mute.load(Relaxed));
    rig.run(2);
    assert_eq!(ch.console.mute_state(), mute_state::ON);
    engine.press_mute(&ch);
    assert!(!ch.params.mute.load(Relaxed));

    // With it, MUTE unmutes the group-muted strip...
    c.mute_override.store(true, Relaxed);
    rig.run(2);
    engine.press_mute(&ch);
    assert!(!ch.params.mute.load(Relaxed));
    rig.run(2);
    assert_eq!(ch.console.mute_state(), mute_state::OVERRIDDEN);
    assert!(close(rig.out(0), 0.75, 1e-5), "{}", rig.out(0));
    // ...until the group changes.
    c.mute_groups.store(0b11, Relaxed);
    rig.run(2);
    assert_eq!(ch.console.mute_state(), mute_state::GROUP);
    assert!(close(rig.out(0), 0.25, 1e-5));

    // Pressing again takes the override back.
    engine.press_mute(&ch);
    rig.run(2);
    assert_eq!(ch.console.mute_state(), mute_state::OVERRIDDEN);
    engine.press_mute(&ch);
    rig.run(2);
    assert_eq!(ch.console.mute_state(), mute_state::GROUP);

    // DCA mutes and SIP can be overridden too; releasing the group ends it.
    c.mute_groups.store(0, Relaxed);
    rig.run(2);
    assert_eq!(ch.console.mute_state(), mute_state::OFF);
    c.solo_mode.store(solo_mode::SIP, Relaxed);
    rig.ch(1).params.solo.store(true, Relaxed);
    rig.run(2);
    assert_eq!(ch.console.mute_state(), mute_state::GROUP);
    engine.press_mute(&ch);
    rig.run(2);
    assert!(close(rig.out(0), 0.75, 1e-5), "SIP overridden: {}", rig.out(0));
    rig.ch(1).params.solo.store(false, Relaxed);
    rig.run(2);
    assert_eq!(ch.console.mute_state(), mute_state::OFF);
    assert!(!ch.console.mute_override.load(Relaxed));
}

#[test]
fn talkback_reaches_its_destinations_with_modes_levels_and_dims() {
    let mut rig = Rig::new(3, 8);
    channels(&mut rig, &[0.4, 0.5, 0.2]);
    let engine = rig.engine.clone();
    let c = &engine.controls;
    let t = &c.talk;
    // CH 2 hosts talkback; it stays muted in the mix.
    let talk_ch = rig.ch(1);
    talk_ch.params.mute.store(true, Relaxed);
    t.assign_kind.store(StripKind::Channel as i32, Relaxed);
    t.assign_index.store(1, Relaxed);
    t.level_db.store(-6.0206);
    let a = &t.paths[0];
    a.buses.store(0b1, Relaxed); // BUS 1
    a.bus_dim_db.store(20.0);
    a.mon_dim_db.store(6.0206);
    let b = &t.paths[1];
    b.matrices.store(0b10, Relaxed); // MTX 2
    b.mains.store(0b10, Relaxed); // MAIN 2
    b.mon_dim_db.store(0.0);

    let bus = rig.strip(StripKind::Bus, 0);
    bus.params.output_left.store(2, Relaxed);
    bus.params.output_right.store(3, Relaxed);
    let ch0 = rig.ch(0);
    let send = ch0.sends.get_or_create(0).unwrap();
    send.enabled.store(true, Relaxed);
    send.mode.store(0, Relaxed);
    let mtx = rig.strip(StripKind::Matrix, 1);
    mtx.params.output_left.store(4, Relaxed);
    let main2 = rig.strip(StripKind::Main, 1);
    main2.params.output_left.store(5, Relaxed);
    c.monitor_output_left.store(6, Relaxed);
    rig.run(4);
    let ch0_on_bus = 0.4; // the PRE send follows the channel's pan (centre: unity on each side)
    assert!(close(rig.out(2), ch0_on_bus, 1e-5), "{}", rig.out(2));
    assert!(rig.out(4).abs() < 1e-7 && rig.out(5).abs() < 1e-7);
    assert!(close(rig.out(6), 0.6, 1e-5));

    // TALK A: CH 2's PFL at the TALK LEVEL on BUS 1, the rest of the bus
    // dimmed by BUS DIM, the selected monitor path by MON DIM.
    a.on.store(true, Relaxed);
    rig.run(4);
    assert!(close(rig.out(2), ch0_on_bus * 0.1 + 0.25, 1e-4), "{}", rig.out(2));
    assert!(close(rig.out(3), ch0_on_bus * 0.1 + 0.25, 1e-4), "talk is centred: {}", rig.out(3));
    assert!(close(rig.out(6), 0.3, 1e-4), "{}", rig.out(6));
    assert!(rig.out(4).abs() < 1e-7, "TALK B is off");

    // USE SEND LEVELS: the talkback channel's send level to the bus instead.
    a.individual.store(true, Relaxed);
    talk_ch.sends.get_or_create(0).unwrap().level_db.store(-12.0412);
    rig.run(4);
    assert!(close(rig.out(3), ch0_on_bus * 0.1 + 0.125, 1e-4), "{}", rig.out(3));

    // TALK B to a matrix and a main (no bus dim there).
    a.on.store(false, Relaxed);
    b.on.store(true, Relaxed);
    rig.run(4);
    assert!(close(rig.out(2), ch0_on_bus, 1e-5), "bus back to normal: {}", rig.out(2));
    assert!(close(rig.out(4), 0.25, 1e-4), "{}", rig.out(4));
    assert!(close(rig.out(5), 0.25, 1e-4), "{}", rig.out(5));

    // Talk tags: TALK B ON unmutes CH 3 and TALK SOLO solos CH 1 while talking.
    let ch2 = rig.ch(2);
    ch2.params.mute.store(true, Relaxed);
    ch2.console.talk_tags.store(talk_tag::B_ON, Relaxed);
    ch0.console.talk_tags.store(talk_tag::SOLO, Relaxed);
    rig.run(4);
    assert!(close(rig.out(0), 0.6, 1e-5), "{}", rig.out(0));
    assert!(c.any_solo_active.load(Relaxed));
    assert!(close(rig.out(6), 0.4, 1e-4), "the TALK SOLO channel on the monitor: {}", rig.out(6));
    b.on.store(false, Relaxed);
    rig.run(4);
    assert!(close(rig.out(0), 0.4, 1e-5), "{}", rig.out(0));
    assert!(!c.any_solo_active.load(Relaxed));

    // Without a talkback channel the keys do nothing.
    t.assign_kind.store(-1, Relaxed);
    b.on.store(true, Relaxed);
    rig.run(4);
    assert!(rig.out(4).abs() < 1e-7);
    assert!(close(rig.out(0), 0.4, 1e-5), "tags need talkback");
}

#[test]
fn talk_from_an_aux_channel() {
    let mut rig = Rig::new(2, 4);
    rig.set_input(1, 0.5);
    let engine = rig.engine.clone();
    let t = &engine.controls.talk;
    let aux = rig.strip(StripKind::Aux, 7);
    aux.params.input_left.store(1, Relaxed);
    t.assign_kind.store(StripKind::Aux as i32, Relaxed);
    t.assign_index.store(7, Relaxed);
    t.paths[0].mains.store(1, Relaxed);
    aux.params.mains[0].on.store(false, Relaxed);
    rig.run(4);
    assert!(rig.out(0).abs() < 1e-7);
    t.paths[0].on.store(true, Relaxed);
    rig.run(4);
    assert!(close(rig.out(0), 0.5, 1e-5) && close(rig.out(1), 0.5, 1e-5), "{} {}", rig.out(0), rig.out(1));
}

#[test]
fn sources_process_the_ports_before_the_channels() {
    let mut rig = Rig::new(4, 2);
    channels(&mut rig, &[0.25, 0.25]);
    let engine = rig.engine.clone();
    let src = engine.sources();
    let p = src.params();
    rig.run(2);
    assert!(close(rig.out(0), 0.5, 1e-5));

    // Gain and polarity of IN 1, mute of IN 2.
    p[0].gain_db.store(6.0206);
    p[1].mute.store(true, Relaxed);
    rig.run(4);
    assert!(close(rig.out(0), 0.5, 1e-4), "{}", rig.out(0));
    rig.ch(1).meters.input_peak[0].exchange(0.0);
    rig.run(2);
    assert!(close(rig.ch(1).meters.input_peak[0].exchange(0.0), 0.0, 1e-7));
    p[0].invert.store(true, Relaxed);
    rig.run(4);
    assert!(close(rig.out(0), -0.5, 1e-4), "{}", rig.out(0));
    p[0].invert.store(false, Relaxed);
    p[1].mute.store(false, Relaxed);
    p[0].gain_db.store(0.0);
    rig.run(4);
    assert!(close(rig.out(0), 0.5, 1e-5));

    // A STEREO pair shares the first source's settings.
    src.set_mode(1, source_mode::STEREO);
    p[0].gain_db.store(-6.0206);
    rig.run(4);
    assert!(close(rig.out(0), 0.25, 1e-4), "{}", rig.out(0));
    p[0].gain_db.store(0.0);

    // MID/SIDE: a stereo channel on IN 3-4 hears M + S and M - S.
    set_channels(&rig.engine, 3);
    let ms = rig.ch(2);
    ms.params.stereo.store(true, Relaxed);
    ms.params.input_left.store(2, Relaxed);
    ms.params.mains[0].on.store(false, Relaxed);
    ms.params.output_left.store(0, Relaxed); // direct out: the PFL copy
    ms.params.output_right.store(1, Relaxed);
    for ch in 0..2 {
        rig.ch(ch).params.mains[0].on.store(false, Relaxed);
    }
    rig.set_input(2, 0.5);
    rig.set_input(3, 0.125);
    src.set_mode(2, source_mode::MID_SIDE);
    rig.run(4);
    assert!(close(rig.out(0), 0.625, 1e-4) && close(rig.out(1), 0.375, 1e-4), "{} {}", rig.out(0), rig.out(1));
    assert!(p[2].peak.exchange(0.0) > 0.6);
    src.set_mode(3, source_mode::MONO);
    rig.run(4);
    assert!(close(rig.out(0), 0.5, 1e-4) && close(rig.out(1), 0.125, 1e-4));

    // Playback channels are sources too.
    assert_eq!(source_index(PLAYBACK_SOURCE_BASE), Some(livedaw_engine::sources::NUM_HW_SOURCES));
}

#[test]
fn channels_follow_their_sources_mode() {
    let engine = Engine::new(0);
    set_channels(&engine, 3);
    let ch = |i| engine.channel_at(i).unwrap();
    // CH 1 on IN 3, CH 2 on IN 4 (the pair's second source), CH 3's ALT on IN 3.
    ch(0).params.input_left.store(2, Relaxed);
    ch(1).params.input_left.store(3, Relaxed);
    ch(2).params.input_left.store(0, Relaxed);
    ch(2).params.alt_input_left.store(2, Relaxed);

    engine.set_source_mode(3, source_mode::STEREO);
    let p = &ch(0).params;
    assert!(p.stereo.load(Relaxed));
    assert_eq!(p.input_right.load(Relaxed), 3);
    assert!(!ch(1).params.stereo.load(Relaxed), "only the first source's channels follow");
    assert_eq!(ch(2).params.alt_input_right.load(Relaxed), 3);
    assert!(!ch(2).params.stereo.load(Relaxed));

    engine.set_source_mode(2, source_mode::MID_SIDE);
    assert!(p.stereo.load(Relaxed));

    engine.set_source_mode(2, source_mode::MONO);
    assert!(!p.stereo.load(Relaxed));
    assert_eq!((p.input_left.load(Relaxed), p.input_right.load(Relaxed)), (2, -1));
    assert_eq!(ch(2).params.alt_input_right.load(Relaxed), -1);
}

#[test]
fn unpatched_channels_stay_idle_with_processed_sources() {
    let mut rig = Rig::new(2, 2);
    set_channels(&rig.engine, 4);
    let engine = rig.engine.clone();
    for p in engine.sources().params() {
        p.gain_db.store(6.0);
    }
    rig.run(4);
    assert!(!rig.ch(0).is_idle() && !rig.ch(1).is_idle());
    assert!(rig.ch(2).is_idle() && rig.ch(3).is_idle(), "no port, no signal");
}
