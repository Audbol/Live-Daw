//! Engine behaviour tests: the WING signal flow end to end.

use livedaw_dsp::wing::{model_index, DYN_MODELS, PROCESS_ORDERS};
mod common;
use common::{set_channels, unity_console};
use livedaw_engine::params::{StripKind, NUM_AUX, NUM_MAINS, NUM_MATRICES, WING_BUSES};
use livedaw_engine::{Engine, InsertCallbacks, Strip};
use std::ffi::c_void;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering::Relaxed};
use std::sync::Arc;

const BLOCK: usize = 128;
/// A mono channel's gain on each side at pan centre (the WING's pan law: unity).
const CENTRE: f32 = 1.0;
/// The near side's gain of a mono channel panned hard (the WING's pan law: +3 dB).
const EDGE: f32 = std::f32::consts::SQRT_2;
/// Each of two equal automix talkers' gain (half the power, -3 dB).
const SHARE: f32 = std::f32::consts::FRAC_1_SQRT_2;

/// Drives an engine with constant (DC) inputs and captures its outputs.
struct Rig {
    engine: Arc<Engine>,
    workers: Vec<std::thread::JoinHandle<()>>,
    inputs: Vec<Vec<f32>>,
    outputs: Vec<Vec<f32>>,
}

impl Rig {
    fn new(num_workers: usize, num_ins: usize, num_outs: usize) -> Self {
        let engine = Arc::new(Engine::new(num_workers));
        unity_console(&engine);
        let workers = (0..num_workers)
            .map(|i| {
                let e = engine.clone();
                std::thread::spawn(move || e.pool().run_worker(i))
            })
            .collect();
        while engine.pool().num_running_workers() < num_workers {
            std::thread::yield_now();
        }
        let in_map: Vec<i32> = (0..num_ins as i32).collect();
        let out_map: Vec<i32> = (0..num_outs as i32).collect();
        unsafe { engine.prepare(48000.0, BLOCK, &in_map, &out_map) };
        Self { engine, workers, inputs: vec![vec![0.0; BLOCK]; num_ins], outputs: vec![vec![0.0; BLOCK]; num_outs] }
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

impl Drop for Rig {
    fn drop(&mut self) {
        self.engine.pool().shutdown();
        for w in self.workers.drain(..) {
            w.join().unwrap();
        }
    }
}

fn close(a: f32, b: f32, tol: f32) -> bool {
    (a - b).abs() <= tol
}

#[test]
fn console_has_the_wing_layout() {
    let e = Engine::new(0);
    assert_eq!(e.num_strips(StripKind::Channel), 0);
    assert_eq!(e.num_strips(StripKind::Aux), NUM_AUX);
    assert_eq!(e.num_strips(StripKind::Bus), WING_BUSES);
    assert_eq!(e.num_strips(StripKind::Main), NUM_MAINS);
    assert_eq!(e.num_strips(StripKind::Matrix), NUM_MATRICES);
    assert_eq!(e.num_fx(), 16);
    set_channels(&e, 40);
    // Unnamed, as on the WING.
    assert_eq!(e.channel_at(39).unwrap().name(), "");
    assert_eq!(e.strip_at(StripKind::Bus, 15).unwrap().name(), "");
    // Wing buses can't be removed; extra ones can.
    let bus = e.strip_at(StripKind::Bus, 0).unwrap();
    assert!(!e.remove_strip(Arc::as_ptr(&bus)));
    let extra = e.add_bus(-1).unwrap();
    assert_eq!(extra.id(), 16);
    assert!(e.remove_strip(Arc::as_ptr(&extra)));
}

#[test]
fn channel_reaches_main_1_through_pan_law() {
    let mut rig = Rig::new(2, 4, 8);
    set_channels(&rig.engine, 2);
    rig.set_input(0, 0.5);
    rig.run(20);
    assert!(close(rig.out(0), 0.5 * CENTRE, 1e-5));
    assert!(close(rig.out(1), 0.5 * CENTRE, 1e-5));

    rig.ch(0).params.pan.store(-100.0);
    rig.run(20);
    assert!(close(rig.out(0), 0.5 * EDGE, 1e-5) && close(rig.out(1), 0.0, 1e-5));
}

#[test]
fn mute_fader_dcas_and_mute_groups() {
    let mut rig = Rig::new(2, 2, 2);
    set_channels(&rig.engine, 1);
    let ch = rig.ch(0);
    rig.set_input(0, 0.5);
    ch.params.fader_db.store(-6.0206);
    rig.run(20);
    assert!(close(rig.out(0), 0.25 * CENTRE, 1e-4));

    // DCA 3 adds its fader to the channel's.
    ch.params.dca_mask.store(1 << 2, Relaxed);
    rig.engine.controls.dcas[2].fader_db.store(-6.0206);
    rig.run(20);
    assert!(close(rig.out(0), 0.125 * CENTRE, 1e-4));
    rig.engine.controls.dcas[2].mute.store(true, Relaxed);
    rig.run(20);
    assert!(close(rig.out(0), 0.0, 1e-6));
    rig.engine.controls.dcas[2].mute.store(false, Relaxed);

    // Mute group 2
    ch.params.mute_group_mask.store(1 << 1, Relaxed);
    rig.engine.controls.mute_groups.store(1 << 1, Relaxed);
    rig.run(20);
    assert!(close(rig.out(0), 0.0, 1e-6));
    rig.engine.controls.mute_groups.store(0, Relaxed);

    ch.params.mute.store(true, Relaxed);
    rig.run(20);
    assert!(close(rig.out(0), 0.0, 1e-6));
    ch.params.mute.store(false, Relaxed);
    ch.params.mains[0].on.store(false, Relaxed);
    rig.run(20);
    assert!(close(rig.out(0), 0.0, 1e-6));
}

#[test]
fn stereo_channel_width_and_inputs() {
    let mut rig = Rig::new(1, 2, 2);
    set_channels(&rig.engine, 1);
    let ch = rig.ch(0);
    rig.set_input(0, 0.25);
    rig.set_input(1, -0.5);
    rig.run(20);
    assert_eq!(ch.num_channels(), 1);

    ch.params.stereo.store(true, Relaxed); // right input defaults to the next channel
    rig.run(20);
    assert_eq!(ch.num_channels(), 2);
    assert!(close(rig.out(0), 0.25, 1e-5) && close(rig.out(1), -0.5, 1e-5));

    ch.params.width.store(0.0); // mono
    rig.run(20);
    assert!(close(rig.out(0), -0.125, 1e-5) && close(rig.out(1), -0.125, 1e-5));
}

#[test]
fn bus_sends_pre_post_and_subgroup() {
    let mut rig = Rig::new(2, 2, 6);
    set_channels(&rig.engine, 1);
    let ch = rig.ch(0);
    rig.set_input(0, 0.5);
    let bus = rig.strip(StripKind::Bus, 2);
    bus.params.output_left.store(2, Relaxed);
    bus.params.output_right.store(3, Relaxed);
    let send = ch.sends.get_or_create(2).unwrap();
    send.enabled.store(true, Relaxed);

    // PRE (tap PFL): the channel fader doesn't matter.
    ch.params.fader_db.store(-100.0);
    rig.run(20);
    assert!(close(rig.out(2), 0.5 * CENTRE, 1e-5), "{}", rig.out(2));

    // POST follows the fader.
    send.mode.store(1, Relaxed);
    rig.run(20);
    assert!(rig.out(2).abs() < 1e-4);
    ch.params.fader_db.store(0.0);
    rig.run(20);
    assert!(close(rig.out(2), 0.5 * CENTRE, 1e-5));

    // Send pan not linked to the channel pan.
    send.pan_link.store(false, Relaxed);
    send.pan.store(100.0);
    rig.run(20);
    assert!(close(rig.out(2), 0.0, 1e-5) && close(rig.out(3), 0.5 * EDGE, 1e-5));

    // GRP: post-fader, post-pan; the bus feeds Main 1.
    send.mode.store(2, Relaxed);
    ch.params.mains[0].on.store(false, Relaxed);
    bus.params.mains[0].on.store(true, Relaxed);
    ch.params.pan.store(-100.0);
    rig.run(20);
    assert!(close(rig.out(0), 0.5 * EDGE, 1e-5) && close(rig.out(1), 0.0, 1e-5));
}

#[test]
fn upper_buses_feed_buses_1_to_8() {
    let mut rig = Rig::new(2, 1, 4);
    set_channels(&rig.engine, 1);
    let ch = rig.ch(0);
    ch.params.mains[0].on.store(false, Relaxed);
    rig.set_input(0, 0.5);
    let fx_bus = rig.strip(StripKind::Bus, 12);
    let bus1 = rig.strip(StripKind::Bus, 0);
    ch.sends.get_or_create(12).unwrap().enabled.store(true, Relaxed);
    fx_bus.sends.get_or_create(0).unwrap().enabled.store(true, Relaxed);
    fx_bus.sends.get_or_create(0).unwrap().mode.store(1, Relaxed);
    bus1.params.output_left.store(2, Relaxed);
    rig.run(20);
    // Mono channel -> stereo bus 13 at unity per side -> bus 1 (post, centred).
    assert!(close(rig.out(2), 0.5 * CENTRE, 1e-4), "{}", rig.out(2));
}

#[test]
fn matrix_takes_mains_and_mix_minus_direct_input() {
    let mut rig = Rig::new(2, 2, 6);
    set_channels(&rig.engine, 2);
    rig.ch(1).params.input_left.store(1, Relaxed);
    rig.set_input(0, 0.5);
    rig.set_input(1, 0.25);
    let main = rig.strip(StripKind::Main, 0);
    let mtx = rig.strip(StripKind::Matrix, 0);
    main.params.matrix[0].on.store(true, Relaxed);
    mtx.params.output_left.store(4, Relaxed);
    rig.run(20);
    assert!(close(rig.out(4), 0.75 * CENTRE, 1e-4));

    // Mix-minus: subtract channel 2 (post fader, panned) with the direct input.
    mtx.params.direct_on.store(true, Relaxed);
    mtx.params.direct_source.store(1, Relaxed);
    mtx.params.direct_invert.store(true, Relaxed);
    rig.run(20);
    assert!(close(rig.out(4), 0.5 * CENTRE, 1e-4), "{}", rig.out(4));
}

#[test]
fn solo_pfl_goes_to_the_monitor() {
    let mut rig = Rig::new(1, 2, 4);
    set_channels(&rig.engine, 2);
    rig.ch(1).params.input_left.store(1, Relaxed);
    rig.set_input(0, 0.5);
    rig.set_input(1, 0.25);
    rig.engine.controls.monitor_output_left.store(2, Relaxed);
    rig.engine.controls.monitor_output_right.store(3, Relaxed);
    rig.run(20);
    assert!(close(rig.out(2), 0.75 * CENTRE, 1e-4)); // follows Main 1

    let ch = rig.ch(1);
    ch.params.fader_db.store(-100.0);
    ch.params.solo.store(true, Relaxed);
    rig.run(20);
    assert!(close(rig.out(2), 0.25, 1e-4), "{}", rig.out(2)); // PFL: pre fader, mono both sides
    assert!(rig.engine.controls.any_solo_active.load(Relaxed));
}

#[test]
fn gate_model_and_process_order() {
    let mut rig = Rig::new(1, 1, 2);
    set_channels(&rig.engine, 1);
    let ch = rig.ch(0);
    rig.set_input(0, 0.001); // -60 dB, below the -40 dB default threshold
    ch.params.gate.on.store(true, Relaxed);
    rig.run(200);
    // Closed down towards its 40 dB default range (1e-5 here).
    assert!(rig.out(0).abs() < 2e-5, "gate didn't close: {}", rig.out(0));
    assert!(ch.meters.gate_active.load(Relaxed));

    // The gate slot can hold a compressor model too.
    ch.params.gate.set_model(model_index(DYN_MODELS, "COMP").unwrap());
    rig.run(50);
    assert!(close(rig.out(0), 0.001 * CENTRE, 1e-5));

    // Any of the 24 orders runs.
    for i in 0..PROCESS_ORDERS.len() {
        ch.params.process_order.store(i as i32, Relaxed);
        rig.run(2);
        assert!(rig.out(0).is_finite());
    }
}

#[test]
fn input_delay_shifts_the_signal() {
    let mut rig = Rig::new(0, 1, 2);
    set_channels(&rig.engine, 1);
    let ch = rig.ch(0);
    ch.params.input.delay_on.store(true, Relaxed);
    ch.params.input.delay_mode.store(3, Relaxed); // samples
    ch.params.input.delay.store(64.0);
    rig.inputs[0][0] = 1.0;
    rig.run(1);
    // (Gains ramp in from silence during the first block, so the impulse is small.)
    let first = rig.outputs[0].iter().position(|x| x.abs() > 0.01).unwrap();
    assert_eq!(first, 64);
}

/// The delay in meters, as the WING: 0.3 m per ms (3 m = 10 ms = 480
/// samples at 48 kHz) and never shorter than its 0.1 m = 16 samples.
#[test]
fn input_delay_in_meters_converts_as_the_wing() {
    for (meters, samples) in [(0.1, 16), (3.0, 480)] {
        let mut rig = Rig::new(0, 1, 2);
        set_channels(&rig.engine, 1);
        let ch = rig.ch(0);
        ch.params.input.delay_on.store(true, Relaxed);
        ch.params.input.delay_mode.store(0, Relaxed); // meters
        ch.params.input.delay.store(meters);
        rig.run(10); // ramps settle in silence
        rig.inputs[0].fill(0.0);
        rig.inputs[0][0] = 1.0;
        let mut first = None;
        for block in 0..4 {
            rig.run(1);
            rig.inputs[0][0] = 0.0;
            if let Some(i) = rig.outputs[0].iter().position(|x| x.abs() > 0.1) {
                first = Some(block * rig.outputs[0].len() + i);
                break;
            }
        }
        assert_eq!(first, Some(samples), "{meters} m");
    }
}

#[test]
fn unpatched_channels_idle_and_resume_cleanly() {
    let mut rig = Rig::new(Engine::default_num_workers(), 8, 2);
    set_channels(&rig.engine, 4096);
    for i in 0..8 {
        rig.set_input(i, 0.01);
    }
    rig.run(10);
    assert!(rig.ch(100).is_idle());
    assert!(!rig.ch(3).is_idle());

    let start = std::time::Instant::now();
    rig.run(200);
    let ms = start.elapsed().as_secs_f64() * 1000.0 / 200.0;
    println!("4096 channels, 8 patched: {ms:.3} ms per {:.2} ms block", 1000.0 * BLOCK as f64 / 48000.0);

    for i in 0..8 {
        rig.ch(i).params.mute.store(true, Relaxed);
    }
    rig.ch(100).params.input_left.store(0, Relaxed);
    rig.run(20);
    assert!(close(rig.out(0), 0.01 * CENTRE, 1e-5));
}

#[test]
fn automix_shares_gain() {
    let mut rig = Rig::new(1, 2, 2);
    set_channels(&rig.engine, 2);
    rig.ch(1).params.input_left.store(1, Relaxed);
    rig.set_input(0, 0.5);
    rig.set_input(1, 0.5);
    for i in 0..2 {
        let ins = &rig.ch(i).params.post_insert;
        ins.on.store(true, Relaxed);
        ins.mode.store(1, Relaxed); // AUTO_X
    }
    rig.run(50);
    // Two equal talkers: each gets half the power (-3 dB), the sum stays at one talker's level.
    assert!(close(rig.out(0), 0.5 * CENTRE * 2.0 * SHARE, 1e-3), "{}", rig.out(0));
    assert_eq!(rig.ch(0).automix_group(), Some(0));

    // AUTO_X with INS 2 off is not a member: it isn't shared and takes no
    // share from the others (channel 1 alone in the group keeps unity).
    rig.ch(1).params.post_insert.on.store(false, Relaxed);
    assert_eq!(rig.ch(1).automix_group(), None);
    rig.run(200);
    assert!(close(rig.ch(0).meters.automix_db.load(), 0.0, 0.05), "{}", rig.ch(0).meters.automix_db.load());
    assert!(close(rig.ch(1).meters.automix_db.load(), 0.0, 0.05), "{}", rig.ch(1).meters.automix_db.load());
    assert!(close(rig.out(0), 2.0 * 0.5 * CENTRE, 1e-3), "{}", rig.out(0));
    // ON again: it joins.
    rig.ch(1).params.post_insert.on.store(true, Relaxed);
    rig.ch(1).params.post_insert.mode.store(2, Relaxed); // AUTO_Y: its own group
    assert_eq!(rig.ch(1).automix_group(), Some(1));
    rig.run(50);
    assert!(close(rig.out(0), 2.0 * 0.5 * CENTRE, 1e-3), "{}", rig.out(0));
    rig.ch(1).params.post_insert.mode.store(1, Relaxed);
    rig.run(50);
    assert!(close(rig.out(0), 0.5 * CENTRE * 2.0 * SHARE, 1e-3), "{}", rig.out(0));
}

// ---- FX slots through the C ABI --------------------------------------------

struct FakePlugin {
    gain: f32,
    mode: u8, // 0 = gain, 1 = emit NaN, 2 = report failure (as a C++ exception would)
    released: Arc<AtomicBool>,
}

unsafe extern "C" fn fake_process(ctx: *mut c_void, ch: *const *mut f32, nc: i32, n: i32) -> i32 {
    let p = &*(ctx as *const FakePlugin);
    match p.mode {
        1 => {
            *(*ch) = f32::NAN;
            0
        }
        2 => 1,
        _ => {
            for c in 0..nc as usize {
                std::slice::from_raw_parts_mut(*ch.add(c), n as usize).iter_mut().for_each(|x| *x *= p.gain);
            }
            0
        }
    }
}

unsafe extern "C" fn fake_release(ctx: *mut c_void) {
    let p = Box::from_raw(ctx as *mut FakePlugin);
    p.released.store(true, Relaxed);
}

fn fake(gain: f32, mode: u8) -> (InsertCallbacks, Arc<AtomicBool>) {
    let released = Arc::new(AtomicBool::new(false));
    let ctx = Box::into_raw(Box::new(FakePlugin { gain, mode, released: released.clone() }));
    (InsertCallbacks { process: fake_process, release: Some(fake_release), ctx: ctx as *mut c_void }, released)
}

#[test]
fn fx_slots_as_inserts_and_faulty_ones_are_auto_bypassed() {
    let mut rig = Rig::new(1, 2, 2);
    set_channels(&rig.engine, 2);
    let (a, b) = (rig.ch(0), rig.ch(1));
    b.params.input_left.store(1, Relaxed);
    let engine = rig.engine.clone();

    let (half, released) = fake(0.5, 0);
    engine.set_fx_plugin(0, Some(half));
    engine.set_fx_plugin(1, Some(fake(1.0, 1).0)); // NaN
    engine.set_fx_plugin(2, Some(fake(1.0, 2).0)); // failing
    a.params.pre_insert.on.store(true, Relaxed);
    a.params.pre_insert.fx.store(0, Relaxed);
    rig.set_input(0, 0.5);
    rig.run(20);
    assert!(close(rig.out(0), 0.25 * CENTRE, 1e-5));

    // Post insert with a NaN plugin: auto-bypassed, signal kept.
    a.params.post_insert.on.store(true, Relaxed);
    a.params.post_insert.fx.store(1, Relaxed);
    rig.run(20);
    assert!(engine.fx(1).unwrap().is_faulted());
    assert!(close(rig.out(0), 0.25 * CENTRE, 1e-5));

    // Failing plugin on another strip.
    b.params.pre_insert.on.store(true, Relaxed);
    b.params.pre_insert.fx.store(2, Relaxed);
    rig.run(5);
    assert!(engine.fx(2).unwrap().is_faulted());
    assert_eq!(engine.num_faulted_fx(), 2);

    // The host rearranging the slot's plugins (a chain edited) keeps it
    // auto-bypassed; a new processor starts unfaulted.
    engine.replace_fx_plugin(1, fake(1.0, 0).0);
    rig.run(5);
    assert!(engine.fx(1).unwrap().is_faulted());
    engine.set_fx_plugin(1, Some(fake(1.0, 0).0));
    assert!(!engine.fx(1).unwrap().is_faulted());
    rig.run(5);
    assert!(!engine.fx(1).unwrap().is_faulted());

    // Two strips on one slot: never concurrent, reported.
    b.params.pre_insert.fx.store(0, Relaxed);
    rig.set_input(1, 0.5);
    rig.run(5);
    assert!(engine.fx(0).unwrap().is_double_assigned());
    assert_eq!(engine.fx_users(0).len(), 2);

    // Replacing a processor releases the old one once the audio thread is done with it.
    engine.set_fx_plugin(0, None);
    rig.run(2);
    engine.set_device_running(true);
    engine.collect();
    assert!(released.load(Relaxed));
}

#[test]
fn nan_input_never_reaches_buses_or_outputs() {
    let mut rig = Rig::new(1, 2, 2);
    set_channels(&rig.engine, 2);
    rig.ch(1).params.input_left.store(1, Relaxed);
    rig.set_input(0, f32::NAN);
    rig.set_input(1, 0.25);
    rig.run(4);
    assert!(rig.ch(0).meters.dsp_faults.load(Relaxed) > 0);
    assert!(close(rig.out(0), 0.25 * CENTRE, 1e-5));

    rig.set_input(0, 0.25);
    rig.run(40);
    assert!(rig.out(0).is_finite() && rig.out(0) > 0.4 * CENTRE);
}

#[test]
fn output_protection_clips_at_the_ceiling() {
    let mut rig = Rig::new(0, 1, 2);
    set_channels(&rig.engine, 1);
    rig.ch(0).params.fader_db.store(10.0);
    rig.set_input(0, 0.9);
    rig.run(20);
    assert!(close(rig.out(0), 1.0, 1e-6));
    assert!(rig.engine.controls.output_clip_events.load(Relaxed) > 0);
    rig.engine.controls.output_ceiling_db.store(-6.0206);
    rig.run(20);
    assert!(close(rig.out(0), 0.5, 1e-4));
    rig.engine.controls.output_protection.store(false, Relaxed);
    rig.run(20);
    assert!(rig.out(0) > 2.0);
}

#[test]
fn stress_structural_changes_while_audio_runs() {
    let mut rig = Rig::new(2, 8, 8);
    for i in 0..8 {
        rig.set_input(i, 0.01 * (i + 1) as f32);
    }
    set_channels(&rig.engine, 8);
    rig.engine.set_device_running(true); // garbage must now wait for callbacks

    let engine = rig.engine.clone();
    let running = Arc::new(AtomicBool::new(true));
    let blocks = Arc::new(AtomicUsize::new(0));
    let bad = Arc::new(AtomicBool::new(false));
    let inputs = rig.inputs.clone();

    let audio = {
        let (running, blocks, bad) = (running.clone(), blocks.clone(), bad.clone());
        std::thread::spawn(move || {
            let mut outputs = vec![vec![0.0f32; BLOCK]; 8];
            let ins: Vec<*const f32> = inputs.iter().map(|v| v.as_ptr()).collect();
            while running.load(Relaxed) {
                let outs: Vec<*mut f32> = outputs.iter_mut().map(|v| v.as_mut_ptr()).collect();
                unsafe { engine.process(&ins, &outs, BLOCK) };
                if outputs.iter().any(|o| !o[0].is_finite()) {
                    bad.store(true, Relaxed);
                }
                blocks.fetch_add(1, Relaxed);
            }
        })
    };

    let mut seed = 42u32;
    let mut rand = move |n: u32| {
        seed = seed.wrapping_mul(1664525).wrapping_add(1013904223);
        (seed >> 8) % n.max(1)
    };
    let end = std::time::Instant::now() + std::time::Duration::from_millis(1500);
    let mut ops = 0;
    while std::time::Instant::now() < end {
        let e = &rig.engine;
        match rand(6) {
            0 => set_channels(e, rand(64) as usize),
            1 => {
                e.add_bus(-1);
            }
            2 => {
                let buses = e.buses();
                e.remove_strip(Arc::as_ptr(&buses[rand(buses.len() as u32) as usize]));
            }
            3 => {
                let slot = rand(16) as usize;
                if rand(2) == 0 {
                    e.set_fx_plugin(slot, Some(fake(0.5, 0).0));
                } else {
                    e.set_fx_plugin(slot, None);
                }
            }
            4 => {
                if let Some(s) = e.channel_at(rand(e.num_channels().max(1) as u32) as usize) {
                    let p = &s.params;
                    p.pre_insert.on.store(rand(2) == 0, Relaxed);
                    p.pre_insert.fx.store(rand(17) as i32 - 1, Relaxed);
                    p.stereo.store(rand(2) == 0, Relaxed);
                    p.gate.on.store(rand(2) == 0, Relaxed);
                    p.dynamics.set_model(rand(DYN_MODELS.len() as u32) as usize);
                    p.dynamics.on.store(true, Relaxed);
                    // Keys from other channels (cycles and itself included) at any tap.
                    for slot in [&p.gate, &p.dynamics] {
                        slot.sc_source.store(rand(66) as i32 - 1, Relaxed);
                        slot.sc_tap.store(rand(8) as i32, Relaxed);
                    }
                    p.eq.set_model(rand(7) as usize, false);
                    p.process_order.store(rand(24) as i32, Relaxed);
                    if let Some(send) = s.sends.get_or_create(rand(24) as usize) {
                        send.enabled.store(rand(2) == 0, Relaxed);
                        send.mode.store(rand(3) as i32, Relaxed);
                    }
                    p.matrix[rand(8) as usize].on.store(rand(2) == 0, Relaxed);
                }
            }
            _ => e.reset(),
        }
        e.collect();
        ops += 1;
    }

    running.store(false, Relaxed);
    audio.join().unwrap();
    rig.engine.set_device_running(false);
    rig.engine.collect();

    println!("{ops} structural operations during {} audio blocks", blocks.load(Relaxed));
    assert!(!bad.load(Relaxed));
    assert!(blocks.load(Relaxed) > 100);
    assert_eq!(rig.engine.garbage().num_pending(), 0);
}

#[test]
fn builtin_fx_devices_run_in_slots() {
    use livedaw_dsp::wing::fx::model_index as fx;
    use livedaw_engine::params::FX_NONE;
    let mut rig = Rig::new(1, 1, 2);
    set_channels(&rig.engine, 1);
    let a = rig.ch(0);
    let engine = rig.engine.clone();

    // A flat graphic EQ with +6 dB trim on a pre insert doubles the signal.
    let geq = fx("GEQ").unwrap();
    assert!(engine.set_fx_model(9, geq, true));
    assert!(engine.fx(9).unwrap().has_device());
    a.params.pre_insert.on.store(true, Relaxed);
    a.params.pre_insert.fx.store(9, Relaxed);
    rig.set_input(0, 0.25);
    rig.run(40);
    let flat = rig.out(0);
    assert!(close(flat, 0.25 * CENTRE, 1e-3), "{flat}");
    engine.controls.fx[9].param("TRIM").unwrap().store(6.0206);
    rig.run(40);
    assert!(close(rig.out(0), 2.0 * flat, 2e-3), "{}", rig.out(0));

    // FX MIX 0 is dry; bypass is dry.
    engine.controls.fx[9].mix.store(0.0);
    rig.run(10);
    assert!(close(rig.out(0), flat, 1e-3));
    engine.controls.fx[9].mix.store(100.0);
    engine.controls.fx[9].bypass.store(true, Relaxed);
    rig.run(10);
    assert!(close(rig.out(0), flat, 1e-3));
    engine.controls.fx[9].bypass.store(false, Relaxed);

    // A hall reverb rings on after the input stops (premium: slots 1-8 only).
    let hall = fx("HALL").unwrap();
    assert!(!engine.set_fx_model(9, hall, true));
    assert!(engine.set_fx_model(0, hall, true));
    a.params.pre_insert.fx.store(0, Relaxed);
    rig.run(20);
    rig.set_input(0, 0.0);
    rig.run(30);
    let tail: f32 = (0..20)
        .map(|_| {
            rig.run(1);
            rig.out(0).abs()
        })
        .fold(0.0, f32::max);
    assert!(tail > 1e-5, "no reverb tail: {tail}");

    // A model set through the shared parameters gets its device on collect.
    engine.controls.fx[1].set_model(fx("CHORUS").unwrap(), true);
    assert!(!engine.fx(1).unwrap().has_device());
    engine.collect();
    assert!(engine.fx(1).unwrap().has_device());

    // NONE removes the device.
    assert!(engine.set_fx_model(0, FX_NONE, true));
    assert!(!engine.fx(0).unwrap().has_device());
}

// ---- WING processing order, taps and console settings ------------------------

/// DC gain of a strip's EQ as the engine runs it.
fn eq_dc_gain(s: &Strip) -> f32 {
    use livedaw_dsp::wing::{eq::design, EQ_MODELS_BUS, EQ_MODELS_CH, MAX_EQ_PARAMS};
    let table = if s.kind().is_output() { EQ_MODELS_BUS } else { EQ_MODELS_CH };
    let spec = &table[s.params.eq.model_index(s.kind().is_output())];
    let mut v = [0.0; MAX_EQ_PARAMS];
    s.params.eq.values_snapshot(&mut v);
    let d = design(spec, &v[..spec.params.len()], 48000.0, s.params.eq.mix.load() / 100.0);
    10f32.powf(d.response_db(1.0, 48000.0) as f32 / 20.0)
}

#[test]
fn bus_runs_dyn_ins1_fader_eq_ins2_with_the_tap_after_ins1() {
    let mut rig = Rig::new(1, 1, 6);
    set_channels(&rig.engine, 1);
    let engine = rig.engine.clone();
    let ch = rig.ch(0);
    ch.params.mains[0].on.store(false, Relaxed);
    rig.set_input(0, 0.5);
    engine.set_fx_plugin(0, Some(fake(0.5, 0).0));
    engine.set_fx_plugin(1, Some(fake(0.25, 0).0));

    let fx_bus = rig.strip(StripKind::Bus, 12);
    let bus1 = rig.strip(StripKind::Bus, 0);
    ch.sends.get_or_create(12).unwrap().enabled.store(true, Relaxed);
    let p = &fx_bus.params;
    p.pre_insert.on.store(true, Relaxed);
    p.pre_insert.fx.store(0, Relaxed);
    p.post_insert.on.store(true, Relaxed);
    p.post_insert.fx.store(1, Relaxed);
    p.eq.param("lg", true).unwrap().store(6.0);
    p.tap.store(0, Relaxed); // ignored: a bus taps after INS 1
    p.fader_db.store(-100.0);
    p.output_left.store(4, Relaxed);
    fx_bus.sends.get_or_create(0).unwrap().enabled.store(true, Relaxed); // PRE
    bus1.params.output_left.store(2, Relaxed);
    rig.run(40);

    // The tap carries INS 1 but neither the fader, the EQ nor INS 2.
    assert!(close(rig.out(2), 0.5 * CENTRE * 0.5, 1e-4), "{}", rig.out(2));
    assert!(rig.out(4).abs() < 1e-6);
    assert!(close(fx_bus.meters.pfl_peak[0].load(), 0.25 * CENTRE, 1e-4));

    // The output runs the EQ and INS 2 after the fader.
    p.fader_db.store(0.0);
    rig.run(40);
    let eq = eq_dc_gain(&fx_bus);
    assert!(eq > 1.9, "{eq}");
    assert!(close(rig.out(4), 0.25 * CENTRE * eq * 0.25, 1e-3), "{} vs {}", rig.out(4), 0.25 * CENTRE * eq * 0.25);
}

#[test]
fn aux_has_no_post_insert_and_taps_after_ins1() {
    let mut rig = Rig::new(1, 2, 4);
    let engine = rig.engine.clone();
    let aux = rig.strip(StripKind::Aux, 0);
    aux.params.input_left.store(0, Relaxed);
    rig.set_input(0, 0.5);
    engine.set_fx_plugin(0, Some(fake(0.5, 0).0));
    engine.set_fx_plugin(1, Some(fake(0.25, 0).0));
    let p = &aux.params;
    p.pre_insert.on.store(true, Relaxed);
    p.pre_insert.fx.store(0, Relaxed);
    p.post_insert.on.store(true, Relaxed); // the aux has no INS 2: ignored
    p.post_insert.fx.store(1, Relaxed);
    p.tap.store(0, Relaxed); // ignored: aux taps after INS 1, pre-fader
    p.fader_db.store(-6.0206);
    aux.sends.get_or_create(2).unwrap().enabled.store(true, Relaxed);
    rig.strip(StripKind::Bus, 2).params.output_left.store(2, Relaxed);
    rig.run(20);
    assert!(close(rig.out(0), 0.5 * 0.5 * 0.5 * CENTRE, 1e-4), "{}", rig.out(0));
    assert!(close(rig.out(2), 0.5 * 0.5 * CENTRE, 1e-4), "{}", rig.out(2));
    assert!(!aux.uses_fx(1) && aux.uses_fx(0));
    assert_eq!(engine.fx_users(1).len(), 0);

    // Aux can't join automix either.
    p.post_insert.mode.store(1, Relaxed);
    rig.run(20);
    assert!(close(rig.out(0), 0.5 * 0.5 * 0.5 * CENTRE, 1e-4));
}

#[test]
fn channel_delay_is_post_fader() {
    let mut rig = Rig::new(0, 1, 4);
    set_channels(&rig.engine, 1);
    let ch = rig.ch(0);
    ch.params.input.delay_on.store(true, Relaxed);
    ch.params.input.delay_mode.store(3, Relaxed); // samples
    ch.params.input.delay.store(64.0);
    ch.sends.get_or_create(1).unwrap().enabled.store(true, Relaxed); // TAP at PRE FDR
    rig.strip(StripKind::Bus, 1).params.output_left.store(2, Relaxed);
    rig.run(10); // ramps settle in silence
    rig.inputs[0][10] = 1.0;
    rig.run(1);
    let first = |ch: usize| rig.outputs[ch].iter().position(|x| x.abs() > 0.1).unwrap();
    assert_eq!(first(0), 74, "the main output is delayed");
    assert_eq!(first(2), 10, "the pre-fader tap is not");
}

#[test]
fn tap_sends_follow_the_channel_mute_unless_ignored() {
    let mut rig = Rig::new(1, 1, 8);
    set_channels(&rig.engine, 1);
    let ch = rig.ch(0);
    rig.set_input(0, 0.5);
    let send = ch.sends.get_or_create(2).unwrap();
    send.enabled.store(true, Relaxed);
    rig.strip(StripKind::Bus, 2).params.output_left.store(2, Relaxed);
    ch.params.matrix[0].on.store(true, Relaxed);
    ch.params.matrix[0].pre.store(true, Relaxed);
    rig.strip(StripKind::Matrix, 0).params.output_left.store(4, Relaxed);
    ch.params.mains[1].on.store(true, Relaxed);
    ch.params.mains[1].pre.store(true, Relaxed);
    rig.strip(StripKind::Main, 1).params.output_left.store(6, Relaxed);
    rig.run(20);
    assert!(close(rig.out(2), 0.5 * CENTRE, 1e-5));
    assert!(close(rig.out(4), 0.5 * CENTRE, 1e-5));
    assert!(close(rig.out(6), 0.5 * CENTRE, 1e-5));

    // Muted (own mute, DCA mute or mute group): TAP sends are cut too.
    for how in 0..3 {
        match how {
            0 => ch.params.mute.store(true, Relaxed),
            1 => {
                ch.params.dca_mask.store(1, Relaxed);
                rig.engine.controls.dcas[0].mute.store(true, Relaxed);
            }
            _ => {
                ch.params.mute_group_mask.store(1, Relaxed);
                rig.engine.controls.mute_groups.store(1, Relaxed);
            }
        }
        rig.run(20);
        assert!(rig.out(2).abs() < 1e-6 && rig.out(4).abs() < 1e-6 && rig.out(6).abs() < 1e-6, "mute {how}");
        ch.params.mute.store(false, Relaxed);
        rig.engine.controls.dcas[0].mute.store(false, Relaxed);
        rig.engine.controls.mute_groups.store(0, Relaxed);
    }

    // IGNORE CHANNEL MUTE keeps bus and matrix TAP sends (not the main send).
    ch.params.mute.store(true, Relaxed);
    send.ignore_mute.store(true, Relaxed);
    ch.params.matrix[0].ignore_mute.store(true, Relaxed);
    rig.run(20);
    assert!(close(rig.out(2), 0.5 * CENTRE, 1e-5));
    assert!(close(rig.out(4), 0.5 * CENTRE, 1e-5));
    assert!(rig.out(6).abs() < 1e-6);

    // POST sends follow the fader and mute regardless.
    send.mode.store(1, Relaxed);
    rig.run(20);
    assert!(rig.out(2).abs() < 1e-6);
}

#[test]
fn bus_pre_sends_follow_the_bus_mute() {
    let mut rig = Rig::new(1, 1, 4);
    set_channels(&rig.engine, 1);
    let ch = rig.ch(0);
    ch.params.mains[0].on.store(false, Relaxed);
    rig.set_input(0, 0.5);
    let fx_bus = rig.strip(StripKind::Bus, 12);
    ch.sends.get_or_create(12).unwrap().enabled.store(true, Relaxed);
    let send = fx_bus.sends.get_or_create(0).unwrap();
    send.enabled.store(true, Relaxed);
    send.ignore_mute.store(true, Relaxed); // only input and aux channels have it
    rig.strip(StripKind::Bus, 0).params.output_left.store(2, Relaxed);
    rig.run(20);
    assert!(close(rig.out(2), 0.5 * CENTRE, 1e-5));
    fx_bus.params.mute.store(true, Relaxed);
    rig.run(20);
    assert!(rig.out(2).abs() < 1e-6);
}

#[test]
fn group_sends_ignore_the_send_level_and_reversed_pan_link() {
    let mut rig = Rig::new(1, 1, 4);
    set_channels(&rig.engine, 1);
    let ch = rig.ch(0);
    ch.params.mains[0].on.store(false, Relaxed);
    rig.set_input(0, 0.5);
    let send = ch.sends.get_or_create(2).unwrap();
    send.enabled.store(true, Relaxed);
    let bus = rig.strip(StripKind::Bus, 2);
    bus.params.output_left.store(2, Relaxed);
    bus.params.output_right.store(3, Relaxed);

    // LINK REVERSED: the send pan mirrors the channel pan.
    ch.params.pan.store(-100.0);
    send.pan_reversed.store(true, Relaxed);
    rig.run(20);
    assert!(close(rig.out(2), 0.0, 1e-5) && close(rig.out(3), 0.5 * EDGE, 1e-5), "{} {}", rig.out(2), rig.out(3));
    send.pan_reversed.store(false, Relaxed);
    rig.run(20);
    assert!(close(rig.out(2), 0.5 * EDGE, 1e-5) && close(rig.out(3), 0.0, 1e-5));

    // GROUP: post-fader, post-pan, no send level.
    send.mode.store(2, Relaxed);
    send.level_db.store(-100.0);
    ch.params.fader_db.store(-6.0206);
    rig.run(20);
    assert!(close(rig.out(2), 0.25 * EDGE, 1e-4), "{}", rig.out(2));
}

#[test]
fn matrix_send_pan_link() {
    let mut rig = Rig::new(1, 1, 6);
    set_channels(&rig.engine, 1);
    let ch = rig.ch(0);
    ch.params.mains[0].on.store(false, Relaxed);
    rig.set_input(0, 0.5);
    let send = &ch.params.matrix[1];
    send.on.store(true, Relaxed);
    let mtx = rig.strip(StripKind::Matrix, 1);
    mtx.params.output_left.store(2, Relaxed);
    mtx.params.output_right.store(3, Relaxed);
    assert!(send.pan_link.load(Relaxed), "linked by default");

    // Linked: the send follows the channel's pan, POST and TAP alike; its own pan doesn't count.
    ch.params.pan.store(-100.0);
    send.pan.store(100.0);
    for pre in [false, true] {
        send.pre.store(pre, Relaxed);
        rig.run(20);
        assert!(close(rig.out(2), 0.5 * EDGE, 1e-5) && close(rig.out(3), 0.0, 1e-5), "pre {pre}: {} {}", rig.out(2), rig.out(3));
    }

    // LINK REVERSED mirrors the channel's pan.
    send.pan_reversed.store(true, Relaxed);
    for pre in [false, true] {
        send.pre.store(pre, Relaxed);
        rig.run(20);
        assert!(close(rig.out(2), 0.0, 1e-5) && close(rig.out(3), 0.5 * EDGE, 1e-5), "reversed, pre {pre}");
    }

    // Unlinked: the send's own pan, whatever the channel's.
    send.pan_link.store(false, Relaxed);
    send.pan.store(0.0);
    for pre in [false, true] {
        send.pre.store(pre, Relaxed);
        rig.run(20);
        assert!(close(rig.out(2), 0.5 * CENTRE, 1e-5) && close(rig.out(3), 0.5 * CENTRE, 1e-5), "own pan, pre {pre}");
    }
    send.pan.store(100.0);
    rig.run(20);
    assert!(close(rig.out(2), 0.0, 1e-5) && close(rig.out(3), 0.5 * EDGE, 1e-5));

    // A bus has no PAN LINK: its panned output, balanced by the send's pan.
    send.on.store(false, Relaxed);
    ch.sends.get_or_create(4).unwrap().enabled.store(true, Relaxed);
    let bus = rig.strip(StripKind::Bus, 4);
    let bus_send = &bus.params.matrix[1];
    bus_send.on.store(true, Relaxed);
    bus_send.pan_link.store(false, Relaxed);
    bus.params.pan.store(-100.0);
    rig.run(20);
    assert!(close(rig.out(2), 0.5 * EDGE, 1e-4) && close(rig.out(3), 0.0, 1e-5), "{} {}", rig.out(2), rig.out(3));
}

#[test]
fn automix_enables_input_channels_only_and_follows_the_fader() {
    let mut rig = Rig::new(1, 2, 4);
    set_channels(&rig.engine, 2);
    rig.ch(1).params.input_left.store(1, Relaxed);
    rig.set_input(0, 0.5);
    rig.set_input(1, 0.5);
    for i in 0..2 {
        let ins = &rig.ch(i).params.post_insert;
        ins.on.store(true, Relaxed);
        ins.mode.store(1, Relaxed); // AUTO_X
    }
    rig.run(50);
    let shared = 0.5 * CENTRE * 2.0 * SHARE;
    assert!(close(rig.out(0), shared, 1e-3), "{}", rig.out(0));
    assert!(close(rig.ch(0).meters.automix_db.load(), -3.0103, 0.05));

    // Automix sits in INS 2: the POST FDR tap is before it, POST PROC after it.
    let a = rig.ch(0);
    a.sends.get_or_create(5).unwrap().enabled.store(true, Relaxed);
    rig.strip(StripKind::Bus, 5).params.output_left.store(2, Relaxed);
    a.params.tap.store(6, Relaxed); // POST FDR
    rig.run(50);
    assert!(close(rig.out(2), 0.5 * CENTRE, 1e-3), "{}", rig.out(2));
    a.params.tap.store(7, Relaxed); // POST PROC
    rig.run(50);
    assert!(close(rig.out(2), 0.5 * CENTRE * SHARE, 1e-3), "{}", rig.out(2));
    a.params.tap.store(5, Relaxed);

    // X ENABLE off: both glide back to unity.
    rig.engine.controls.automix_enable[0].store(false, Relaxed);
    rig.run(50);
    assert!(close(rig.out(0), 2.0 * 0.5 * CENTRE, 1e-4), "{}", rig.out(0));
    rig.engine.controls.automix_enable[0].store(true, Relaxed);
    rig.run(50);
    assert!(close(rig.out(0), shared, 1e-3));

    // Pulling one fader down hands its share to the other channel.
    rig.ch(1).params.fader_db.store(-100.0);
    rig.run(200);
    assert!(close(rig.out(0), 0.5 * CENTRE, 1e-3), "{}", rig.out(0));
}

#[test]
fn trim_and_pre_fader_meters() {
    let mut rig = Rig::new(0, 1, 2);
    set_channels(&rig.engine, 1);
    let ch = rig.ch(0);
    rig.set_input(0, 0.25);
    ch.params.input.trim_db.store(6.0206);
    ch.params.fader_db.store(-100.0);
    rig.run(20);
    let m = &ch.meters;
    for v in [&m.input_peak[0], &m.trim_peak[0], &m.pfl_peak[0], &m.output_peak[0]] {
        v.store(0.0);
    }
    rig.run(2);
    assert!(close(m.input_peak[0].load(), 0.25, 1e-5));
    assert!(close(m.trim_peak[0].load(), 0.5, 1e-4));
    assert!(close(m.pfl_peak[0].load(), 0.5, 1e-4));
    assert!(m.output_peak[0].load() < 1e-6);
}

#[test]
fn monitor_dim_mute_mono_and_swap() {
    let mut rig = Rig::new(0, 1, 4);
    set_channels(&rig.engine, 1);
    let engine = rig.engine.clone();
    let c = &engine.controls;
    c.monitor_output_left.store(2, Relaxed);
    c.monitor_output_right.store(3, Relaxed);
    rig.ch(0).params.pan.store(-100.0);
    rig.set_input(0, 0.5);
    rig.run(20);
    let hard = 0.5 * EDGE;
    assert!(close(rig.out(2), hard, 1e-5) && close(rig.out(3), 0.0, 1e-5));

    c.monitor_dim.store(true, Relaxed);
    rig.run(20);
    assert!(close(rig.out(2), hard * 0.1, 1e-5), "{}", rig.out(2));
    c.monitor_dim_db.store(6.0206);
    rig.run(20);
    assert!(close(rig.out(2), hard * 0.5, 1e-4));
    c.monitor_mute.store(true, Relaxed);
    rig.run(20);
    assert!(rig.out(2).abs() < 1e-7);
    c.monitor_mute.store(false, Relaxed);
    c.monitor_dim.store(false, Relaxed);
    assert_eq!(c.monitor_level_db.load(), 0.0, "DIM never touches the level");

    c.monitor_mono.store(true, Relaxed);
    rig.run(20);
    assert!(close(rig.out(2), hard * 0.5, 1e-5) && close(rig.out(3), hard * 0.5, 1e-5));
    c.monitor_mono.store(false, Relaxed);
    c.monitor_swap.store(true, Relaxed);
    rig.run(20);
    assert!(close(rig.out(2), 0.0, 1e-5) && close(rig.out(3), hard, 1e-5));
    // Main 1 itself is unaffected.
    assert!(close(rig.out(0), hard, 1e-5));
}

#[test]
fn solo_pfl_or_afl_per_strip_type() {
    let mut rig = Rig::new(0, 1, 4);
    set_channels(&rig.engine, 1);
    let engine = rig.engine.clone();
    let c = &engine.controls;
    c.monitor_output_left.store(2, Relaxed);
    let ch = rig.ch(0);
    rig.set_input(0, 0.5);
    ch.sends.get_or_create(3).unwrap().enabled.store(true, Relaxed);
    let bus = rig.strip(StripKind::Bus, 3);
    bus.params.fader_db.store(-100.0);
    bus.params.solo.store(true, Relaxed);

    // BUS SOLO defaults to AFL: the bus fader is down, nothing to hear.
    rig.run(20);
    assert!(rig.out(2).abs() < 1e-6);
    c.bus_solo_afl.store(false, Relaxed);
    rig.run(20);
    assert!(close(rig.out(2), 0.5 * CENTRE, 1e-5), "{}", rig.out(2));

    // CHANNEL SOLO covers aux channels too.
    bus.params.solo.store(false, Relaxed);
    let aux = rig.strip(StripKind::Aux, 0);
    aux.params.input_left.store(0, Relaxed);
    aux.params.fader_db.store(-100.0);
    aux.params.solo.store(true, Relaxed);
    rig.run(20);
    assert!(close(rig.out(2), 0.5, 1e-5), "PFL: {}", rig.out(2));
    c.solo_afl.store(true, Relaxed);
    rig.run(20);
    assert!(rig.out(2).abs() < 1e-6);
}

#[test]
fn dca_mutegroups_main_link_and_startup_main_mute() {
    let mut rig = Rig::new(1, 1, 6);
    set_channels(&rig.engine, 1);
    let engine = rig.engine.clone();
    let c = &engine.controls;
    let ch = rig.ch(0);
    rig.set_input(0, 0.5);
    ch.params.dca_mask.store(1, Relaxed);
    c.dcas[0].mute.store(true, Relaxed);
    rig.run(20);
    assert!(rig.out(0).abs() < 1e-6);
    c.dca_mute_groups.store(false, Relaxed);
    rig.run(20);
    assert!(close(rig.out(0), 0.5 * CENTRE, 1e-5), "the DCA mute no longer mutes its strips");
    c.dcas[0].mute.store(false, Relaxed);

    // MAIN LINK 1>2: Main 1's fader also acts on Main 2, not on Main 3.
    for i in 1..3 {
        ch.params.mains[i].on.store(true, Relaxed);
        rig.strip(StripKind::Main, i).params.output_left.store(2 * i as i32, Relaxed);
    }
    let main1 = rig.strip(StripKind::Main, 0);
    main1.params.fader_db.store(-6.0206);
    rig.run(20);
    assert!(close(rig.out(2), 0.5 * CENTRE, 1e-5));
    c.main_link.store(1, Relaxed);
    rig.run(20);
    assert!(close(rig.out(0), 0.25 * CENTRE, 1e-4));
    assert!(close(rig.out(2), 0.25 * CENTRE, 1e-4), "{}", rig.out(2));
    assert!(close(rig.out(4), 0.5 * CENTRE, 1e-5));
    c.main_link.store(2, Relaxed);
    rig.run(20);
    assert!(close(rig.out(4), 0.25 * CENTRE, 1e-4));

    // STARTUP MAIN MUTE
    assert!(!engine.apply_startup_main_mute());
    assert!(!main1.params.mute.load(Relaxed));
    c.startup_main_mute.store(true, Relaxed);
    assert!(engine.apply_startup_main_mute());
    assert!((0..NUM_MAINS).all(|i| rig.strip(StripKind::Main, i).params.mute.load(Relaxed)));
}

/// WING 3.0.6: a POST (AFL) solo of an input, aux or DCA follows the pan.
#[test]
fn post_solo_follows_the_pan() {
    let mut rig = Rig::new(0, 1, 4);
    set_channels(&rig.engine, 1);
    let engine = rig.engine.clone();
    let c = &engine.controls;
    c.monitor_output_left.store(2, Relaxed);
    c.monitor_output_right.store(3, Relaxed);
    c.solo_afl.store(true, Relaxed);
    rig.set_input(0, 0.5);
    let ch = rig.ch(0);
    let aux = rig.strip(StripKind::Aux, 0);
    aux.params.input_left.store(0, Relaxed);
    for (strip, pan) in [(&ch, -100.0), (&aux, 100.0)] {
        strip.params.pan.store(pan);
        strip.params.solo.store(true, Relaxed);
        rig.run(20);
        let (near, far) = if pan < 0.0 { (rig.out(2), rig.out(3)) } else { (rig.out(3), rig.out(2)) };
        assert!(close(near, 0.5 * EDGE, 1e-4) && far.abs() < 1e-5, "{near} {far}");
        strip.params.solo.store(false, Relaxed);
    }

    // A DCA solo solos its members after their pan too.
    ch.params.dca_mask.store(1, Relaxed);
    c.dcas[0].solo.store(true, Relaxed);
    rig.run(20);
    assert!(close(rig.out(2), 0.5 * EDGE, 1e-4) && rig.out(3).abs() < 1e-5, "{} {}", rig.out(2), rig.out(3));
    // PFL ignores it.
    c.solo_afl.store(false, Relaxed);
    rig.run(20);
    assert!(close(rig.out(2), rig.out(3), 1e-5));
}
