//! KEY TAP: GATE and DYNAMICS keys taken at a tap point of another channel,
//! in the right order within a block (`keytap.rs`).

use livedaw_dsp::wing::{model_index, DYN_MODELS};
mod common;
use common::{set_channels, unity_console};
use livedaw_engine::params::StripKind;
use livedaw_engine::{Engine, Strip};
use std::sync::atomic::Ordering::Relaxed;
use std::sync::Arc;

const BLOCK: usize = 128;
/// A mono channel's gain on each side at pan centre (the WING's pan law: unity).
const CENTRE: f32 = 1.0;

/// Tap points (`TAP_POINTS` order).
const IN: i32 = 0;
const PFL: i32 = 5;
const AFL: i32 = 6;
const POST: i32 = 7;

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

    /// Keys channel `dst`'s gate (on) from channel `src` at `tap`, and plans the buffers.
    fn key_gate(&self, dst: usize, src: i32, tap: i32) {
        let gate = &self.ch(dst).params.gate;
        gate.on.store(true, Relaxed);
        gate.sc_source.store(src, Relaxed);
        gate.sc_tap.store(tap, Relaxed);
        self.engine.collect();
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

#[test]
fn the_key_follows_the_tap_of_the_source() {
    let mut rig = Rig::new(2, 2, 2);
    set_channels(&rig.engine, 2);
    // Channel 1 is the key source (fader down, off Main 1); channel 2 is gated by it.
    let src = rig.ch(0);
    src.params.mains[0].on.store(false, Relaxed);
    src.params.fader_db.store(-100.0);
    rig.inputs[0].fill(0.5);
    rig.inputs[1].fill(0.5);
    let open = 0.5 * CENTRE;

    // IN and PFL are before the fader: the gate opens.
    for tap in [IN, PFL] {
        rig.key_gate(1, 0, tap);
        rig.run(100);
        assert!((rig.out(0) - open).abs() < 1e-3, "tap {tap}: {}", rig.out(0));
    }
    // AFL and POST follow the source's fader: closed.
    for tap in [AFL, POST] {
        rig.key_gate(1, 0, tap);
        rig.run(200);
        assert!(rig.out(0).abs() < 0.01, "tap {tap}: {}", rig.out(0));
    }
    assert!(rig.ch(0).keys.has(POST as usize) && !rig.ch(0).keys.has(IN as usize), "only the used tap has a buffer");
    // The fader comes up: POST opens it again.
    src.params.fader_db.store(0.0);
    rig.run(100);
    assert!((rig.out(0) - open).abs() < 1e-3, "{}", rig.out(0));
    // Muted: POST and AFL are silent, IN still keys.
    src.params.mute.store(true, Relaxed);
    rig.run(200);
    assert!(rig.out(0).abs() < 0.01, "{}", rig.out(0));
    rig.key_gate(1, 0, IN);
    rig.run(100);
    assert!((rig.out(0) - open).abs() < 1e-3, "{}", rig.out(0));

    // Back to SELF: the buffers are retired.
    rig.ch(1).params.gate.sc_source.store(-1, Relaxed);
    rig.engine.collect();
    assert!(!rig.ch(0).keys.has(IN as usize));
}

#[test]
fn the_in_tap_is_after_the_input_trim() {
    let mut rig = Rig::new(0, 2, 2);
    set_channels(&rig.engine, 2);
    let src = rig.ch(0);
    src.params.mains[0].on.store(false, Relaxed);
    rig.inputs[0].fill(0.5);
    rig.inputs[1].fill(0.5);
    rig.key_gate(1, 0, IN);
    rig.run(50);
    assert!(rig.out(0) > 0.3);
    // -60 dB of trim takes the key below the gate's -40 dB threshold.
    src.params.input.trim_db.store(-60.0);
    rig.run(200);
    assert!(rig.out(0).abs() < 0.01, "{}", rig.out(0));
}

#[test]
fn a_key_from_a_later_channel_reads_this_block() {
    let mut rig = Rig::new(2, 4, 2);
    set_channels(&rig.engine, 4);
    for i in 0..3 {
        rig.inputs[i].fill(0.5);
    }
    // Channel 1 keys from channel 3 (processed later in console order), and
    // channel 2 from channel 1 (a chain): each runs after its source.
    rig.key_gate(0, 2, POST);
    rig.key_gate(1, 0, POST);
    let key_db = |rig: &Rig, i: usize| rig.ch(i).meters.gate_key_db.load();

    rig.ch(2).params.fader_db.store(-100.0);
    rig.run(50);
    assert!(key_db(&rig, 0) < -90.0, "{}", key_db(&rig, 0));
    rig.ch(2).params.fader_db.store(0.0);
    rig.run(1);
    assert!(key_db(&rig, 0) > -30.0, "the key lags a block: {}", key_db(&rig, 0));

    rig.ch(0).params.fader_db.store(-100.0);
    rig.run(50);
    assert!(key_db(&rig, 1) < -90.0, "{}", key_db(&rig, 1));
    rig.ch(0).params.fader_db.store(0.0);
    rig.run(1);
    assert!(key_db(&rig, 1) > -30.0, "the chain lags a block: {}", key_db(&rig, 1));
}

#[test]
fn key_cycles_read_the_previous_block() {
    let mut rig = Rig::new(2, 4, 2);
    set_channels(&rig.engine, 4);
    // Channels 1 and 2 key each other; channel 3 keys itself (IN).
    rig.key_gate(0, 1, POST);
    rig.key_gate(1, 0, POST);
    rig.key_gate(2, 2, IN);
    rig.inputs[0].fill(0.5);
    rig.inputs[2].fill(0.5);
    rig.run(20);
    assert!(rig.ch(0).meters.gate_key_db.load() < -99.0, "{}", rig.ch(0).meters.gate_key_db.load());
    // Channel 2 starts: channel 1's key sees it one block later.
    rig.inputs[1].fill(0.5);
    rig.run(1);
    assert!(rig.ch(0).meters.gate_key_db.load() < -99.0, "{}", rig.ch(0).meters.gate_key_db.load());
    rig.run(1);
    assert!(rig.ch(0).meters.gate_key_db.load() > -30.0, "{}", rig.ch(0).meters.gate_key_db.load());
    rig.run(100);
    assert!(rig.ch(2).meters.gate_key_db.load() > -30.0, "{}", rig.ch(2).meters.gate_key_db.load());
    assert!(rig.outputs.iter().flatten().all(|x| x.is_finite()));
}

#[test]
fn buses_and_aux_key_from_channels() {
    let mut rig = Rig::new(2, 2, 2);
    set_channels(&rig.engine, 2);
    let bus = rig.engine.strip_at(StripKind::Bus, 0).unwrap();
    let aux = rig.engine.strip_at(StripKind::Aux, 0).unwrap();
    for s in [&bus, &aux] {
        let d = &s.params.dynamics;
        d.set_model(model_index(DYN_MODELS, "GATE").unwrap()); // its key meter reads the plain key
        d.on.store(true, Relaxed);
        d.sc_source.store(1, Relaxed);
        d.sc_tap.store(POST, Relaxed);
    }
    aux.params.input_left.store(0, Relaxed); // patched (silent), so it isn't idle
    rig.engine.collect();
    rig.run(20);
    assert!(bus.meters.dyn_key_db.load() < -99.0, "{}", bus.meters.dyn_key_db.load());
    assert!(aux.meters.dyn_key_db.load() < -99.0, "{}", aux.meters.dyn_key_db.load());
    // A bus runs after every channel, the aux in a wave after channel 2:
    // both take this block's POST tap.
    rig.inputs[1].fill(0.5);
    rig.run(1);
    assert!(bus.meters.dyn_key_db.load() > -30.0, "{}", bus.meters.dyn_key_db.load());
    assert!(aux.meters.dyn_key_db.load() > -30.0, "{}", aux.meters.dyn_key_db.load());
}

#[test]
fn an_idle_source_keys_silence() {
    let mut rig = Rig::new(1, 2, 2);
    set_channels(&rig.engine, 2);
    rig.inputs[0].fill(0.5);
    rig.inputs[1].fill(0.5);
    rig.key_gate(1, 0, PFL);
    rig.run(50);
    assert!(rig.out(0) > 0.3);
    // Unpatched (idle fast path): its key tap is silent, the gate closes.
    rig.ch(0).params.input_left.store(-1, Relaxed);
    rig.run(200);
    assert!(rig.ch(0).is_idle());
    assert!(rig.out(0).abs() < 0.01, "{}", rig.out(0));
}
