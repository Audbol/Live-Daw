//! The FX slots' meters for the device screens: the slot's input and output
//! levels (`FxRack::level`) and its device's readouts (`FxRack::readout`),
//! and a device's own latency (the PRECISION LIMITER's look-ahead) in the
//! slot's latency, the delay compensation and FX MIX's dry path.

mod common;
use common::{set_channels, unity_console};
use livedaw_dsp::wing::{model_index, FX_MODELS};
use livedaw_engine::Engine;
use std::sync::atomic::Ordering::Relaxed;
use std::sync::Arc;

const BLOCK: usize = 256;
const SR: f32 = 48000.0;

fn engine() -> Arc<Engine> {
    let engine = Arc::new(Engine::new(0));
    unity_console(&engine);
    unsafe { engine.prepare(SR as f64, BLOCK, &[0], &[0, 1]) };
    set_channels(&engine, 1);
    engine
}

/// FX `slot` running `token` on channel 1's PRE insert.
fn insert(engine: &Engine, slot: usize, token: &str) {
    assert!(engine.set_fx_model(slot, model_index(FX_MODELS, token).unwrap(), true));
    let ch = engine.channel_at(0).unwrap();
    ch.params.pre_insert.on.store(true, Relaxed);
    ch.params.pre_insert.fx.store(slot as i32, Relaxed);
}

fn set(engine: &Engine, slot: usize, id: &str, v: f32) {
    let m = engine.controls.fx[slot].model.load(Relaxed) as usize;
    let i = FX_MODELS[m].param_index(id).unwrap();
    engine.controls.fx[slot].values[i].store(v);
}

/// Runs `blocks` blocks of `signal` into input 1; returns Main 1 left of the last block.
fn run(engine: &Engine, blocks: usize, signal: impl Fn(usize) -> f32) -> Vec<f32> {
    let mut input = vec![0.0f32; BLOCK];
    let mut outs = [vec![0.0f32; BLOCK], vec![0.0f32; BLOCK]];
    for b in 0..blocks {
        for (i, x) in input.iter_mut().enumerate() {
            *x = signal(b * BLOCK + i);
        }
        let ins = [input.as_ptr()];
        let o: Vec<*mut f32> = outs.iter_mut().map(|v| v.as_mut_ptr()).collect();
        unsafe { engine.process(&ins, &o, BLOCK) };
    }
    outs[0].clone()
}

fn sine(freq: f32, amp: f32) -> impl Fn(usize) -> f32 {
    move |i| amp * (std::f32::consts::TAU * freq * i as f32 / SR).sin()
}

#[test]
fn combinator_meters_follow_the_signal() {
    let engine = engine();
    insert(&engine, 9, "C5-CMB");
    set(&engine, 9, "thr", -30.0);
    set(&engine, 9, "sbcon", 0.0);
    run(&engine, 100, sine(1000.0, 0.5));
    let fx = engine.fx(9).unwrap();
    // Band 3 (1 kHz) compresses; the input level is the tone's.
    assert!(fx.readout(2) < -3.0, "band 3: {}", fx.readout(2));
    assert!(fx.readout(0) > -0.5 && fx.readout(4) > -0.5);
    assert!((fx.level(0) - 0.5).abs() < 0.02, "in {}", fx.level(0));
    assert!(fx.level(2) < fx.level(0), "out {} under in {}", fx.level(2), fx.level(0));
    // Bypassed: idle readouts, the levels show the signal passing.
    engine.controls.fx[9].bypass.store(true, Relaxed);
    run(&engine, 4, sine(1000.0, 0.5));
    assert_eq!(fx.readout(2), 0.0);
    assert!((fx.level(2) - fx.level(0)).abs() < 1e-6);
    // Silence: the levels fall at 60 dB/s (half a second: about -30 dB).
    run(&engine, (0.5 * SR) as usize / BLOCK, |_| 0.0);
    let db = 20.0 * fx.level(0).log10();
    assert!((-36.0..-26.0).contains(&db), "{db}");
}

#[test]
fn limiter_look_ahead_is_its_latency() {
    let engine = engine();
    insert(&engine, 9, "LIMITER");
    let fx = engine.fx(9).unwrap();
    let p = &engine.controls.fx[9];
    assert_eq!(fx.latency(p), 48);
    assert_eq!(fx.effective_latency(p), 48);
    p.bypass.store(true, Relaxed);
    assert_eq!(fx.effective_latency(p), 0);
    p.bypass.store(false, Relaxed);
    // FX MIX 50 % on a quiet 500 Hz tone (no limiting): the dry half is
    // delayed with the wet half, so they add up instead of cancelling.
    set(&engine, 9, "again", 0.0);
    set(&engine, 9, "gout", 0.0);
    p.mix.store(50.0);
    engine.collect();
    let out = run(&engine, 40, sine(500.0, 0.1));
    let peak = out.iter().fold(0.0f32, |a, x| a.max(x.abs()));
    p.bypass.store(true, Relaxed);
    let dry = run(&engine, 40, sine(500.0, 0.1));
    p.bypass.store(false, Relaxed);
    let dry_peak = dry.iter().fold(0.0f32, |a, x| a.max(x.abs()));
    assert!((peak / dry_peak - 1.0).abs() < 0.02, "{peak} vs {dry_peak}");
    // The GR readout shows a hot signal's reduction.
    set(&engine, 9, "gin", 18.0);
    p.mix.store(100.0);
    run(&engine, 40, sine(500.0, 0.5));
    assert!(fx.readout(0) < -10.0, "{}", fx.readout(0));
    assert_eq!(fx.readout(0), fx.meter());
}
