//! Routing beyond the strips' own patch (`routing.rs`): sources straight to
//! outputs, buses / mains / matrices as channel sources, USER SIGNALs and
//! USER PATCHes, bus-to-bus sends between any buses (loops included) and a
//! matrix's DIRECT IN. Audio is checked sample by sample where the latency
//! matters; the audio thread is checked not to allocate.

mod common;
use common::{set_channels, unity_console};
use livedaw_engine::params::StripKind;
use livedaw_engine::routing::{matrix_direct, strip_source_input};
use livedaw_engine::sources::{source_mode, user_side, NUM_USER_SIGNALS, OSC_SOURCE_BASE, USR_SOURCE_BASE, USR_SOURCE_INDEX};
use livedaw_engine::{Engine, Strip};
use std::cell::Cell;
use std::sync::atomic::{AtomicUsize, Ordering::Relaxed};
use std::sync::Arc;

const BLOCK: usize = 64;

// ---- an allocation counter for the audio thread --------------------------------------

thread_local! {
    static IN_AUDIO: Cell<bool> = const { Cell::new(false) };
}
static AUDIO_ALLOCS: AtomicUsize = AtomicUsize::new(0);

struct Counting;

fn note() {
    if IN_AUDIO.try_with(|c| c.get()).unwrap_or(false) {
        AUDIO_ALLOCS.fetch_add(1, Relaxed);
    }
}

unsafe impl std::alloc::GlobalAlloc for Counting {
    unsafe fn alloc(&self, l: std::alloc::Layout) -> *mut u8 {
        note();
        unsafe { std::alloc::System.alloc(l) }
    }
    unsafe fn dealloc(&self, p: *mut u8, l: std::alloc::Layout) {
        note();
        unsafe { std::alloc::System.dealloc(p, l) }
    }
    unsafe fn alloc_zeroed(&self, l: std::alloc::Layout) -> *mut u8 {
        note();
        unsafe { std::alloc::System.alloc_zeroed(l) }
    }
    unsafe fn realloc(&self, p: *mut u8, l: std::alloc::Layout, n: usize) -> *mut u8 {
        note();
        unsafe { std::alloc::System.realloc(p, l, n) }
    }
}

#[global_allocator]
static ALLOC: Counting = Counting;

// ---- the rig ---------------------------------------------------------------------------

/// Drives an engine block by block with a ramp on every input (input `i`
/// carries `(i + 1) * 0.01 * t` for sample t, so delays show).
struct Rig {
    engine: Arc<Engine>,
    workers: Vec<std::thread::JoinHandle<()>>,
    inputs: Vec<Vec<f32>>,
    outputs: Vec<Vec<f32>>,
    /// Every output's samples so far.
    history: Vec<Vec<f32>>,
    t: usize,
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
        // Nothing on the mains' outputs unless a test asks.
        let main = engine.strip_at(StripKind::Main, 0).unwrap();
        main.params.output_left.store(-1, Relaxed);
        main.params.output_right.store(-1, Relaxed);
        Self {
            engine,
            workers,
            inputs: vec![vec![0.0; BLOCK]; num_ins],
            outputs: vec![vec![0.0; BLOCK]; num_outs],
            history: vec![Vec::new(); num_outs],
            t: 0,
        }
    }

    /// The input signal of port `i` at sample `t`.
    fn input(i: usize, t: usize) -> f32 {
        (i + 1) as f32 * 0.01 * ((t % 1000) as f32 / 1000.0)
    }

    fn run(&mut self, blocks: usize) {
        for _ in 0..blocks {
            for (i, buf) in self.inputs.iter_mut().enumerate() {
                for (k, x) in buf.iter_mut().enumerate() {
                    *x = Self::input(i, self.t + k);
                }
            }
            let ins: Vec<*const f32> = self.inputs.iter().map(|v| v.as_ptr()).collect();
            let outs: Vec<*mut f32> = self.outputs.iter_mut().map(|v| v.as_mut_ptr()).collect();
            IN_AUDIO.with(|c| c.set(true));
            unsafe { self.engine.process(&ins, &outs, BLOCK) };
            IN_AUDIO.with(|c| c.set(false));
            for (h, o) in self.history.iter_mut().zip(&self.outputs) {
                h.extend_from_slice(o);
            }
            self.t += BLOCK;
        }
        self.engine.collect();
    }

    /// Output `o`'s last block equals `f(t)` for its samples.
    fn last_block_matches(&self, o: usize, f: impl Fn(usize) -> f32, tol: f32) -> bool {
        let h = &self.history[o];
        let start = self.t - BLOCK;
        (0..BLOCK).all(|k| {
            let ok = (h[start + k] - f(start + k)).abs() <= tol;
            if !ok {
                eprintln!("out {o} sample {}: {} != {}", start + k, h[start + k], f(start + k));
            }
            ok
        })
    }

    fn ch(&self, i: usize) -> Arc<Strip> {
        self.engine.channel_at(i).unwrap()
    }

    fn strip(&self, kind: StripKind, i: usize) -> Arc<Strip> {
        self.engine.strip_at(kind, i).unwrap()
    }

    fn bus(&self, slot: i32) -> Arc<Strip> {
        self.engine.bus_in_slot(slot).unwrap()
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

fn input(i: usize, t: usize) -> f32 {
    Rig::input(i, t)
}

/// A channel at unity, sending nowhere.
fn quiet(s: &Strip) {
    s.params.fader_db.store(0.0);
    s.params.mains[0].on.store(false, Relaxed);
}

// ---- 9: sources straight to outputs ------------------------------------------------------

/// Manual 5.4 OUTPUTS ("route any Source to any digital or analog output")
/// and chapter 2 ("sent directly from any input to any output"): an input
/// port, after its source processing, plays on an output in the same block.
#[test]
fn a_source_plays_on_an_output_without_a_channel() {
    let mut rig = Rig::new(0, 4, 8);
    let e = &rig.engine;
    assert!(e.route_output_source(5, 2));
    e.sources().set_gain(2, 5.0); // LOCAL IN takes 2.5 dB steps
    e.sources().get(2).unwrap().invert.store(true, Relaxed);
    rig.run(4);
    let g = -livedaw_dsp::util::db_to_gain(5.0);
    assert!(rig.last_block_matches(5, |t| g * input(2, t), 1e-5));

    // An output has one source: routing Bus 1 there replaces the input.
    let bus = rig.bus(0);
    assert!(rig.engine.route_output_source(5, strip_source_input(StripKind::Bus, 0, 1)));
    assert_eq!(rig.engine.output_patch().get(5), -1);
    assert_eq!(bus.params.output_right.load(Relaxed), 5);
    // ... and a source replaces a strip.
    rig.engine.route_output_source(5, OSC_SOURCE_BASE);
    assert_eq!(bus.params.output_right.load(Relaxed), -1);
    rig.run(2);
    assert!(rig.history[5][rig.t - BLOCK..].iter().any(|x| x.abs() > 0.1), "the oscillator plays");
    rig.engine.clear_output(5);
    rig.run(2);
    assert!(rig.history[5][rig.t - BLOCK..].iter().all(|x| *x == 0.0));
}

// ---- 10: buses, mains and matrices as channel sources --------------------------------------

/// Manual 2.6 ("All Buses, Mains and Matrices can be selected as the
/// Source for all Input and Aux channels") and 6.6 (a latency, 38 samples
/// on the WING): a channel patched to a bus hears it one device block later.
#[test]
fn a_bus_main_or_matrix_feeds_a_channel_one_block_late() {
    for workers in [0, 2] {
        let mut rig = Rig::new(workers, 2, 8);
        set_channels(&rig.engine, 2);
        let (src, dst) = (rig.ch(0), rig.ch(1));
        quiet(&src);
        quiet(&dst);
        // CH 1 -> Bus 3 (TAP, mono at unity on both sides).
        let send = src.sends.get_or_create(2).unwrap();
        send.enabled.store(true, Relaxed);
        send.level_db.store(0.0);
        // CH 2 takes Bus 3 L and plays it on its direct out (output 6).
        dst.params.input_left.store(strip_source_input(StripKind::Bus, 2, 0), Relaxed);
        dst.params.output_left.store(6, Relaxed);
        rig.run(6);
        assert!(rig.last_block_matches(6, |t| input(0, t - BLOCK), 1e-5), "workers {workers}");

        // A stereo channel on Main 1 L/R (Main 1 takes CH 1).
        src.params.mains[0].on.store(true, Relaxed);
        dst.params.stereo.store(true, Relaxed);
        dst.params.input_left.store(strip_source_input(StripKind::Main, 0, 0), Relaxed);
        dst.params.input_right.store(-1, Relaxed); // the next signal: Main 1 R
        dst.params.output_right.store(7, Relaxed);
        rig.run(6);
        assert!(rig.last_block_matches(6, |t| input(0, t - BLOCK), 1e-5));
        assert!(rig.last_block_matches(7, |t| input(0, t - BLOCK), 1e-5));

        // A matrix: Main 1 -> Matrix 2 -> CH 2.
        rig.strip(StripKind::Main, 0).params.matrix[1].on.store(true, Relaxed);
        rig.strip(StripKind::Main, 0).params.matrix[1].level_db.store(0.0);
        dst.params.input_left.store(strip_source_input(StripKind::Matrix, 1, 0), Relaxed);
        rig.run(6);
        assert!(rig.last_block_matches(6, |t| input(0, t - BLOCK), 1e-5));
    }
}

/// A channel that sends to the bus it listens to is a feedback loop through
/// the one-block return, as on the WING: the engine runs (no cycle in its
/// order) and the signal recirculates once per block.
#[test]
fn a_feedback_loop_through_a_bus_source_recirculates_a_block_at_a_time() {
    let mut rig = Rig::new(0, 1, 8);
    set_channels(&rig.engine, 2);
    let (a, b) = (rig.ch(0), rig.ch(1));
    quiet(&a);
    quiet(&b);
    for (s, db) in [(&a, 0.0), (&b, -6.0)] {
        let send = s.sends.get_or_create(0).unwrap();
        send.enabled.store(true, Relaxed);
        send.level_db.store(db);
    }
    // CH 2 listens to Bus 1, which it feeds at -6 dB.
    b.params.input_left.store(strip_source_input(StripKind::Bus, 0, 0), Relaxed);
    rig.bus(0).params.output_left.store(3, Relaxed);
    rig.run(200);
    let g = livedaw_dsp::util::db_to_gain(-6.0);
    // bus(t) = in(t) + g * bus(t - BLOCK): finite, and the echo is there.
    let h = &rig.history[3];
    let t = rig.t - 1;
    assert!(h.iter().all(|x| x.is_finite()));
    assert!((h[t] - (input(0, t) + g * h[t - BLOCK])).abs() < 1e-4, "{} vs {}", h[t], input(0, t) + g * h[t - BLOCK]);
}

// ---- 11: user signals ---------------------------------------------------------------------

/// Manual 2.6, User Signals: a mono copy of a strip, PRE (its tap point) or
/// POST, L / R / L+R of a stereo one; on an output in the same block, into
/// a channel one block late.
#[test]
fn user_signals_copy_a_strip_to_outputs_and_channels() {
    let mut rig = Rig::new(0, 2, 8);
    set_channels(&rig.engine, 3);
    let e = rig.engine.clone();
    let ch1 = rig.ch(0);
    quiet(&ch1);
    ch1.params.fader_db.store(-6.0);
    let us = &e.sources().user_signals()[0];
    us.set_strip(Some((StripKind::Channel, 1)));
    e.route_output_source(2, USR_SOURCE_BASE);
    rig.run(4);
    // PRE: the tap point, before the fader.
    assert!(rig.last_block_matches(2, |t| input(0, t), 1e-5));
    // POST: after the fader.
    us.post.store(true, Relaxed);
    rig.run(4);
    let g = livedaw_dsp::util::db_to_gain(-6.0);
    assert!(rig.last_block_matches(2, |t| g * input(0, t), 1e-5));

    // A stereo strip: Bus 1 fed by CH 1 (L) and CH 2 (R), L / R / L+R.
    let ch2 = rig.ch(1);
    quiet(&ch2);
    ch2.params.input_left.store(1, Relaxed);
    ch1.params.fader_db.store(0.0);
    for (s, pan) in [(&ch1, -100.0), (&ch2, 100.0)] {
        let send = s.sends.get_or_create(0).unwrap();
        send.enabled.store(true, Relaxed);
        send.level_db.store(0.0);
        send.pan_link.store(false, Relaxed);
        send.pan.store(pan);
    }
    let edge = std::f32::consts::SQRT_2; // a mono send panned hard (WING law)
    us.set_strip(Some((StripKind::Bus, 1)));
    for (side, f) in [
        (user_side::LEFT, Box::new(|t| edge * input(0, t)) as Box<dyn Fn(usize) -> f32>),
        (user_side::RIGHT, Box::new(|t| edge * input(1, t))),
        (user_side::BOTH, Box::new(|t| 0.5 * edge * (input(0, t) + input(1, t)))),
    ] {
        us.side.store(side, Relaxed);
        rig.run(3);
        assert!(rig.last_block_matches(2, &f, 1e-5), "side {side}");
    }

    // Into a channel (CH 3 on USR 1): one block late; muted and inverted as a source.
    let ch3 = rig.ch(2);
    quiet(&ch3);
    ch3.params.input_left.store(USR_SOURCE_BASE, Relaxed);
    ch3.params.output_left.store(4, Relaxed);
    e.sources().get(USR_SOURCE_INDEX).unwrap().invert.store(true, Relaxed);
    rig.run(4);
    assert!(rig.last_block_matches(4, |t| -0.5 * edge * (input(0, t - BLOCK) + input(1, t - BLOCK)), 1e-5));
    e.sources().get(USR_SOURCE_INDEX).unwrap().mute.store(true, Relaxed);
    rig.run(3);
    assert!(rig.last_block_matches(4, |_| 0.0, 1e-6));
    // Back to OFF: silent, not stale.
    us.set_strip(None);
    e.sources().get(USR_SOURCE_INDEX).unwrap().mute.store(false, Relaxed);
    rig.run(2);
    assert!(rig.last_block_matches(2, |_| 0.0, 0.0) && rig.last_block_matches(4, |_| 0.0, 0.0));
}

/// Two adjacent user signals as one STEREO source.
#[test]
fn adjacent_user_signals_pair_as_stereo() {
    let mut rig = Rig::new(0, 2, 8);
    set_channels(&rig.engine, 3);
    let e = rig.engine.clone();
    let (ch1, ch2, ch3) = (rig.ch(0), rig.ch(1), rig.ch(2));
    quiet(&ch1);
    quiet(&ch2);
    quiet(&ch3);
    ch2.params.input_left.store(1, Relaxed);
    let u = e.sources().user_signals();
    u[2].set_strip(Some((StripKind::Channel, 2)));
    u[3].set_strip(Some((StripKind::Channel, 1)));
    e.set_source_mode(USR_SOURCE_INDEX + 2, source_mode::STEREO);
    // A stereo channel on USR 3/4: L = CH 2, R = CH 1.
    ch3.params.stereo.store(true, Relaxed);
    ch3.params.input_left.store(USR_SOURCE_BASE + 2, Relaxed);
    ch3.params.input_right.store(USR_SOURCE_BASE + 3, Relaxed);
    ch3.params.output_left.store(5, Relaxed);
    ch3.params.output_right.store(6, Relaxed);
    rig.run(5);
    assert!(rig.last_block_matches(5, |t| input(1, t - BLOCK), 1e-5));
    assert!(rig.last_block_matches(6, |t| input(0, t - BLOCK), 1e-5));
    assert_eq!(NUM_USER_SIGNALS, 24);
}

// ---- 12: user patches ---------------------------------------------------------------------

/// Manual 2.6, User Patches: copies of external sources, so two ports from
/// anywhere make a stereo (or mid/side) source, each with its own polarity.
#[test]
fn user_patches_pair_any_two_ports() {
    let mut rig = Rig::new(0, 4, 8);
    set_channels(&rig.engine, 1);
    let e = rig.engine.clone();
    let ch = rig.ch(0);
    quiet(&ch);
    // USR 25 = port 4, USR 26 = port 1, as a stereo pair; USR 26 inverted.
    e.sources().set_user_patch(0, 3);
    e.sources().set_user_patch(1, 0);
    e.sources().set_user_patch(2, OSC_SOURCE_BASE); // not external: OFF
    assert_eq!(e.sources().user_patches()[2].source.load(Relaxed), -1);
    let first = USR_SOURCE_INDEX + NUM_USER_SIGNALS;
    e.set_source_mode(first, source_mode::STEREO);
    e.sources().get(first + 1).unwrap().invert.store(true, Relaxed);
    ch.params.stereo.store(true, Relaxed);
    ch.params.input_left.store(USR_SOURCE_BASE + 24, Relaxed);
    ch.params.input_right.store(USR_SOURCE_BASE + 25, Relaxed);
    ch.params.output_left.store(5, Relaxed);
    ch.params.output_right.store(6, Relaxed);
    rig.run(4);
    // Same block: the patches are processed with the sources.
    assert!(rig.last_block_matches(5, |t| input(3, t), 1e-5));
    assert!(rig.last_block_matches(6, |t| input(0, t), 1e-5), "a pair runs on USR 25's polarity");
    // MONO: each its own; the patch also plays on an output directly.
    e.set_source_mode(first, source_mode::MONO);
    e.sources().get(first + 1).unwrap().invert.store(true, Relaxed);
    ch.params.stereo.store(true, Relaxed);
    ch.params.input_right.store(USR_SOURCE_BASE + 25, Relaxed);
    e.route_output_source(7, USR_SOURCE_BASE + 25);
    rig.run(3);
    assert!(rig.last_block_matches(6, |t| -input(0, t), 1e-5));
    assert!(rig.last_block_matches(7, |t| -input(0, t), 1e-5));
}

// ---- 15: bus to bus ---------------------------------------------------------------------

fn bus_send(from: &Strip, to: i32, db: f32) {
    let s = from.sends.get_or_create(to as usize).unwrap();
    s.enabled.store(true, Relaxed);
    s.level_db.store(db);
}

/// Manual 5.10 (BUS → BUS "from Bus 1-16 to BUS 1-16"): any bus feeds any
/// other, lower or higher; a chain arrives in the same block.
#[test]
fn any_bus_feeds_any_other_bus_in_the_same_block() {
    for workers in [0, 3] {
        let mut rig = Rig::new(workers, 1, 8);
        set_channels(&rig.engine, 1);
        let ch = rig.ch(0);
        quiet(&ch);
        bus_send(&ch, 1, 0.0); // CH 1 -> Bus 2 (mono at unity, L and R)
                               // Bus 2 -> Bus 5 -> Bus 1 -> Bus 16 (POST, centred: unity per side).
        bus_send(&rig.bus(1), 4, 0.0);
        bus_send(&rig.bus(4), 0, -6.0);
        bus_send(&rig.bus(0), 15, 0.0);
        let b16 = rig.bus(15);
        b16.params.mains[0].on.store(false, Relaxed);
        b16.params.output_left.store(2, Relaxed);
        rig.run(4);
        let g = livedaw_dsp::util::db_to_gain(-6.0);
        assert!(rig.last_block_matches(2, |t| g * input(0, t), 1e-5), "workers {workers}");
    }
}

/// A loop of buses (A -> B -> A) never stalls the engine: the send closing
/// it reads its source's previous block (manual 6.6 has 8 samples there).
#[test]
fn a_bus_loop_is_cut_by_a_one_block_return() {
    let mut rig = Rig::new(2, 1, 8);
    set_channels(&rig.engine, 1);
    let ch = rig.ch(0);
    quiet(&ch);
    bus_send(&ch, 2, 0.0); // CH 1 -> Bus 3
    let (b3, b7) = (rig.bus(2), rig.bus(6));
    bus_send(&b3, 6, 0.0); // Bus 3 -> Bus 7
    bus_send(&b7, 2, -12.0); // Bus 7 -> Bus 3 closes the loop
    b7.sends.find(2).unwrap().mode.store(livedaw_engine::sends::send_mode::POST, Relaxed);
    b7.params.output_left.store(3, Relaxed);
    rig.run(100);
    let h = &rig.history[3];
    assert!(h.iter().all(|x| x.is_finite()));
    // Bus 3 runs first (the lowest bus of the loop): b7(t) = in(t) + g * b7(t - BLOCK).
    let g = livedaw_dsp::util::db_to_gain(-12.0);
    let t = rig.t - 7;
    assert!((h[t] - (input(0, t) + g * h[t - BLOCK])).abs() < 1e-4);
    // A PRE send closing the loop reads the tap (pre-fader): the fader of Bus 7 doesn't change it.
    b7.sends.find(2).unwrap().mode.store(livedaw_engine::sends::send_mode::PRE, Relaxed);
    b7.params.output_left.store(-1, Relaxed);
    b3.params.output_left.store(4, Relaxed);
    b7.params.fader_db.store(-90.0);
    rig.run(60);
    let h = &rig.history[4];
    let t = rig.t - 3;
    assert!((h[t] - (input(0, t) + g * h[t - BLOCK])).abs() < 1e-4, "{} vs {}", h[t], input(0, t) + g * h[t - BLOCK]);
}

// ---- 14: matrix DIRECT IN ---------------------------------------------------------------

/// Manual 6.4: a matrix's DIRECT IN takes the PHONES, the SPEAKERS or the
/// solo bus (a block late: the monitor runs after the matrices); AES is
/// silent here; OFF adds nothing.
#[test]
fn matrix_direct_in_takes_the_monitor_paths() {
    let mut rig = Rig::new(0, 1, 8);
    set_channels(&rig.engine, 1);
    let ch = rig.ch(0);
    ch.params.fader_db.store(0.0); // CH 1 -> Main 1 (centre)
    let c = &rig.engine.controls;
    c.monitor_level_db.store(0.0);
    c.phones.level_db.store(-6.0);
    let mtx = rig.strip(StripKind::Matrix, 3);
    mtx.params.output_left.store(2, Relaxed);
    mtx.params.direct_on.store(true, Relaxed);
    mtx.params.direct_level_db.store(0.0);
    for (choice, gain) in [
        (matrix_direct::SPEAKERS, 1.0),
        (matrix_direct::PHONES, livedaw_dsp::util::db_to_gain(-6.0)),
        (matrix_direct::AES, 0.0),
        (matrix_direct::OFF, 0.0),
    ] {
        mtx.params.direct_source.store(choice, Relaxed);
        rig.run(4);
        assert!(rig.last_block_matches(2, |t| gain * input(0, t - BLOCK), 1e-5), "choice {choice}");
    }
    // The solo bus: CH 1 soloed (PFL).
    mtx.params.direct_source.store(matrix_direct::SOLO_BUS, Relaxed);
    ch.params.solo.store(true, Relaxed);
    ch.params.fader_db.store(-20.0);
    rig.run(4);
    assert!(rig.last_block_matches(2, |t| input(0, t - BLOCK), 1e-5));
}

// ---- realtime safety --------------------------------------------------------------------

/// Every routing above is set up, switched and torn down while the audio
/// thread runs, without one allocation there.
#[test]
fn routing_changes_never_allocate_on_the_audio_thread() {
    let mut rig = Rig::new(0, 4, 16);
    set_channels(&rig.engine, 4);
    rig.run(4); // first blocks (lazy one-time setup elsewhere) before counting
    let e = rig.engine.clone();
    for ch in e.channels() {
        quiet(&ch);
        bus_send(&ch, 0, 0.0);
    }
    AUDIO_ALLOCS.store(0, Relaxed);
    for round in 0..3 {
        e.route_output_source(9, round);
        e.route_output_source(10, USR_SOURCE_BASE + round);
        e.sources().user_signals()[round as usize].set_strip(Some((StripKind::Bus, 1)));
        e.sources().set_user_patch(round as usize, 3);
        rig.ch(1).params.input_left.store(strip_source_input(StripKind::Bus, round as usize, 0), Relaxed);
        rig.ch(2).params.input_left.store(USR_SOURCE_BASE + round, Relaxed);
        bus_send(&rig.bus(round + 1), round, 0.0);
        bus_send(&rig.bus(round), round + 1, -20.0);
        rig.strip(StripKind::Matrix, 0).params.direct_on.store(true, Relaxed);
        rig.strip(StripKind::Matrix, 0).params.direct_source.store(2 + round, Relaxed);
        rig.run(3);
    }
    for u in e.sources().user_signals() {
        u.set_strip(None);
    }
    rig.run(2);
    // (`Rig::run` collects after its blocks, on this thread but outside the audio flag.)
    assert_eq!(AUDIO_ALLOCS.load(Relaxed), 0, "the audio thread allocated");
}
