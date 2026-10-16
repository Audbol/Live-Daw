//! Engine behaviour tests (ported from the C++ EngineTests/StabilityTests).

use livedaw_engine::{Engine, InsertCallbacks, Strip, StripKind};
use std::ffi::c_void;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering::Relaxed};
use std::sync::Arc;

const BLOCK: usize = 128;
const CENTRE: f32 = std::f32::consts::FRAC_1_SQRT_2;

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

    fn input(&self, i: usize) -> Arc<Strip> {
        self.engine.input_at(i).unwrap()
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
fn input_reaches_master_through_pan_law() {
    let mut rig = Rig::new(2, 4, 8);
    rig.engine.set_num_inputs(2);
    rig.set_input(0, 0.5);
    rig.run(20);
    assert!(close(rig.out(0), 0.5 * CENTRE, 1e-5));
    assert!(close(rig.out(1), 0.5 * CENTRE, 1e-5));

    rig.input(0).params.pan.store(-1.0);
    rig.run(20);
    assert!(close(rig.out(0), 0.5, 1e-5) && close(rig.out(1), 0.0, 1e-5));
}

#[test]
fn mute_fader_and_main_assign() {
    let mut rig = Rig::new(2, 2, 2);
    rig.engine.set_num_inputs(1);
    let ch = rig.input(0);
    rig.set_input(0, 0.5);
    ch.params.fader_db.store(-6.0206);
    rig.run(20);
    assert!(close(rig.out(0), 0.25 * CENTRE, 1e-4));
    ch.params.mute.store(true, Relaxed);
    rig.run(20);
    assert!(close(rig.out(0), 0.0, 1e-6));
    ch.params.mute.store(false, Relaxed);
    ch.params.main_assign.store(false, Relaxed);
    rig.run(20);
    assert!(close(rig.out(0), 0.0, 1e-6));
}

#[test]
fn stereo_channel_uses_both_inputs_and_can_switch_live() {
    let mut rig = Rig::new(1, 2, 2);
    rig.engine.set_num_inputs(1);
    let ch = rig.input(0);
    rig.set_input(0, 0.25);
    rig.set_input(1, -0.5);
    rig.run(20);
    assert_eq!(ch.num_channels(), 1);

    ch.params.stereo.store(true, Relaxed); // right input defaults to the next channel
    rig.run(20);
    assert_eq!(ch.num_channels(), 2);
    assert!(close(rig.out(0), 0.25, 1e-5) && close(rig.out(1), -0.5, 1e-5));
}

#[test]
fn pre_fader_aux_send_to_hardware_output() {
    let mut rig = Rig::new(2, 2, 6);
    rig.engine.set_num_inputs(2);
    let aux = rig.engine.add_bus(StripKind::Aux, -1).unwrap();
    aux.params.output_left.store(4, Relaxed);
    let ch = rig.input(1);
    ch.params.fader_db.store(-100.0); // fader down: a pre-fader send stays live
    let send = ch.sends.get_or_create(aux.id() as usize).unwrap();
    send.enabled.store(true, Relaxed);
    send.pre_fader.store(true, Relaxed);
    rig.set_input(1, 0.5);
    rig.run(20);
    assert!(close(rig.out(4), 0.5, 1e-5));
    assert!(close(rig.out(0), 0.0, 1e-6));

    send.pre_fader.store(false, Relaxed);
    rig.run(20);
    assert!(close(rig.out(4), 0.0, 1e-6));
}

#[test]
fn group_bus_routes_to_master() {
    let mut rig = Rig::new(2, 1, 2);
    rig.engine.set_num_inputs(1);
    let group = rig.engine.add_bus(StripKind::Group, -1).unwrap();
    let ch = rig.input(0);
    ch.params.main_assign.store(false, Relaxed);
    let send = ch.sends.get_or_create(group.id() as usize).unwrap();
    send.enabled.store(true, Relaxed);
    send.pre_fader.store(false, Relaxed);
    rig.set_input(0, 0.5);
    rig.run(20);
    assert!(close(rig.out(0), 0.5 * CENTRE, 1e-5));
    group.params.mute.store(true, Relaxed);
    rig.run(20);
    assert!(close(rig.out(0), 0.0, 1e-6));
}

#[test]
fn pfl_solo_replaces_master_on_monitor_outputs() {
    let mut rig = Rig::new(2, 2, 4);
    rig.engine.set_num_inputs(2);
    rig.input(1).params.input_left.store(1, Relaxed);
    rig.engine.controls.monitor_output_left.store(2, Relaxed);
    rig.engine.controls.monitor_output_right.store(3, Relaxed);
    rig.set_input(0, 0.5);
    rig.set_input(1, 0.25);
    rig.run(20);
    assert!(close(rig.out(2), 0.75 * CENTRE, 1e-5));

    rig.input(1).params.solo.store(true, Relaxed);
    rig.input(1).params.fader_db.store(-100.0);
    rig.run(20);
    assert!(rig.engine.controls.any_solo_active.load(Relaxed));
    assert!(close(rig.out(2), 0.25, 1e-5)); // PFL ignores the fader
    assert!(close(rig.out(0), 0.5 * CENTRE, 1e-5)); // master unaffected
}

#[test]
fn many_channels_in_parallel_and_shrinking_live() {
    let mut rig = Rig::new(3, 256, 2);
    rig.engine.set_num_inputs(256);
    for i in 0..256 {
        rig.set_input(i, 0.001);
    }
    rig.run(20);
    assert!(close(rig.out(0), 256.0 * 0.001 * CENTRE, 1e-4));
    rig.engine.set_num_inputs(16);
    rig.run(20);
    assert!(close(rig.out(0), 16.0 * 0.001 * CENTRE, 1e-5));
}

#[test]
fn channels_beyond_the_interface_are_unpatched() {
    let mut rig = Rig::new(0, 2, 2);
    rig.engine.set_num_inputs(3);
    assert_eq!(rig.input(2).params.input_left.load(Relaxed), -1);
    rig.set_input(0, 0.1);
    rig.set_input(1, 0.1);
    rig.run(20);
    assert!(close(rig.out(0), 0.2 * CENTRE, 1e-5));
}

#[test]
fn unpatched_channels_idle_and_resume_cleanly() {
    let mut rig = Rig::new(Engine::default_num_workers(), 8, 2);
    rig.engine.set_num_inputs(4096);
    for i in 0..8 {
        rig.set_input(i, 0.01);
    }
    rig.run(10);
    assert!(rig.input(100).is_idle());
    assert!(!rig.input(3).is_idle());

    let start = std::time::Instant::now();
    rig.run(200);
    let ms = start.elapsed().as_secs_f64() * 1000.0 / 200.0;
    println!("4096 channels, 8 patched: {ms:.3} ms per {:.2} ms block", 1000.0 * BLOCK as f64 / 48000.0);

    for i in 0..8 {
        rig.input(i).params.mute.store(true, Relaxed);
    }
    rig.input(100).params.input_left.store(0, Relaxed);
    rig.run(20);
    assert!(close(rig.out(0), 0.01 * CENTRE, 1e-5));
}

#[test]
fn bus_count_is_not_capped_at_32() {
    let mut rig = Rig::new(2, 1, 4);
    rig.engine.set_num_inputs(1);
    let mut last = None;
    for i in 0..100 {
        last = rig.engine.add_bus(if i % 2 == 0 { StripKind::Aux } else { StripKind::Group }, -1);
    }
    let last = last.unwrap();
    assert_eq!(rig.engine.num_buses(), 100);
    last.params.stereo.store(false, Relaxed);
    last.params.main_assign.store(false, Relaxed);
    last.params.output_left.store(3, Relaxed);
    let ch = rig.input(0);
    let send = ch.sends.get_or_create(last.id() as usize).unwrap();
    send.enabled.store(true, Relaxed);
    rig.set_input(0, 0.5);
    rig.run(20);
    assert!(close(rig.out(3), 0.5, 1e-5));

    let slot = last.id();
    assert!(rig.engine.remove_strip(Arc::as_ptr(&last)));
    let reused = rig.engine.add_bus(StripKind::Aux, -1).unwrap();
    assert_eq!(reused.id(), slot);
    assert!(!rig.input(0).sends.find(slot as usize).unwrap().enabled.load(Relaxed));
}

// ---- plugins through the C ABI ---------------------------------------------

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
fn plugins_run_and_faulty_ones_are_auto_bypassed() {
    let mut rig = Rig::new(1, 1, 2);
    rig.engine.set_num_inputs(1);
    let ch = rig.input(0);
    let engine = rig.engine.clone();
    let g = engine.garbage();

    let (nan, _) = fake(1.0, 1);
    let (failing, _) = fake(1.0, 2);
    let (half, released) = fake(0.5, 0);
    ch.inserts.set(0, Some(nan), g);
    ch.inserts.set(1, Some(failing), g);
    ch.inserts.set(2, Some(half), g);
    rig.set_input(0, 0.5);
    rig.run(20);

    assert!(ch.inserts.is_faulted(0) && ch.inserts.is_bypassed(0));
    assert!(ch.inserts.is_faulted(1));
    assert!(!ch.inserts.is_faulted(2));
    assert!(close(rig.out(0), 0.25 * CENTRE, 1e-5)); // the healthy plugin still runs
    assert_eq!(rig.engine.num_faulted_plugins(), 2);

    // Replacing a plugin releases the old one once the audio thread is done with it.
    ch.inserts.set(2, None, g);
    rig.run(2);
    rig.engine.set_device_running(true);
    rig.engine.collect();
    assert!(released.load(Relaxed));
}

#[test]
fn nan_input_never_reaches_buses_or_outputs() {
    let mut rig = Rig::new(1, 2, 2);
    rig.engine.set_num_inputs(2);
    rig.input(1).params.input_left.store(1, Relaxed);
    rig.input(0).params.eq.bands[1].gain_db.store(6.0);
    rig.set_input(0, f32::NAN);
    rig.set_input(1, 0.25);
    rig.run(4);
    assert!(rig.input(0).meters.dsp_faults.load(Relaxed) > 0);
    assert!(close(rig.out(0), 0.25 * CENTRE, 1e-5));

    rig.set_input(0, 0.25);
    rig.run(40);
    assert!(rig.out(0).is_finite() && rig.out(0) > 0.4 * CENTRE);
}

#[test]
fn output_protection_clips_at_the_ceiling() {
    let mut rig = Rig::new(0, 1, 2);
    rig.engine.set_num_inputs(1);
    rig.input(0).params.fader_db.store(10.0);
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
    rig.engine.set_num_inputs(8);
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
        match rand(5) {
            0 => e.set_num_inputs(rand(64) as usize),
            1 => {
                e.add_bus(if rand(2) == 0 { StripKind::Aux } else { StripKind::Group }, -1);
            }
            2 => {
                let buses = e.buses();
                if !buses.is_empty() {
                    e.remove_strip(Arc::as_ptr(&buses[rand(buses.len() as u32) as usize]));
                }
            }
            3 => {
                if let Some(s) = e.input_at(rand(e.num_inputs().max(1) as u32) as usize) {
                    let slot = rand(8) as usize;
                    if rand(2) == 0 {
                        s.inserts.set(slot, Some(fake(0.5, 0).0), e.garbage());
                    } else {
                        s.inserts.set(slot, None, e.garbage());
                    }
                    s.params.stereo.store(rand(2) == 0, Relaxed);
                    if let Some(send) = s.sends.get_or_create(rand(128) as usize) {
                        send.enabled.store(rand(2) == 0, Relaxed);
                    }
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
