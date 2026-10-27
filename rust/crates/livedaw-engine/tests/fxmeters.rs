//! The FX panels' live readouts: an FX slot's input and output peaks
//! (`FxRack::take_io_peaks`, the meters on its panel) and its device's live
//! values (`FxRack::live`: LFO positions, envelope, needle, band levels, note).

mod common;
use common::{set_channels, unity_console};
use livedaw_dsp::wing::fx::FxLive;
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

/// Runs `blocks` blocks of `signal` (a function of the sample index) into input 1.
fn run(engine: &Engine, from: usize, blocks: usize, signal: impl Fn(usize) -> f32) {
    let mut input = vec![0.0f32; BLOCK];
    let mut outs = [vec![0.0f32; BLOCK], vec![0.0f32; BLOCK]];
    for b in 0..blocks {
        for (i, x) in input.iter_mut().enumerate() {
            *x = signal((from + b) * BLOCK + i);
        }
        let ins = [input.as_ptr()];
        let o: Vec<*mut f32> = outs.iter_mut().map(|v| v.as_mut_ptr()).collect();
        unsafe { engine.process(&ins, &o, BLOCK) };
    }
}

fn sine(amp: f32) -> impl Fn(usize) -> f32 {
    move |i| amp * (std::f32::consts::TAU * 440.0 * i as f32 / SR).sin()
}

/// Channel 1 through FX slot 1 running `token`.
fn through(engine: &Engine, token: &str) {
    assert!(engine.set_fx_model(0, model_index(FX_MODELS, token).unwrap(), true));
    let ch = engine.channel_at(0).unwrap();
    ch.params.pre_insert.on.store(true, Relaxed);
    ch.params.pre_insert.fx.store(0, Relaxed);
}

#[test]
fn a_slot_reports_what_goes_in_and_out() {
    let engine = engine();
    through(&engine, "HALL");
    let rack = engine.fx(0).unwrap();
    run(&engine, 0, 40, sine(0.5));
    let p = rack.take_io_peaks();
    // A mono channel: the same input on both sides, the reverb's output (mixed at FX MIX) out.
    assert!((p[0] - 0.5).abs() < 0.01 && (p[1] - 0.5).abs() < 0.01, "input peaks {p:?}");
    assert!(p[2] > 0.01 && p[3] > 0.01, "output peaks {p:?}");
    // Taking them resets them; silence reads 0 in, the tail still out.
    assert_eq!(rack.take_io_peaks()[0], 0.0);
    run(&engine, 40, 4, |_| 0.0);
    let q = rack.take_io_peaks();
    assert!(q[0] == 0.0 && q[2] > 0.0, "silence in, tail out: {q:?}");
    // Bypassed, nothing runs: no readings.
    engine.controls.fx[0].bypass.store(true, Relaxed);
    run(&engine, 44, 4, sine(0.5));
    assert_eq!(rack.take_io_peaks(), [0.0; 4]);
}

#[test]
fn the_device_reports_its_lfo_envelope_and_needle() {
    let engine = engine();
    through(&engine, "PANNER");
    let m = model_index(FX_MODELS, "PANNER").unwrap();
    let p = &engine.controls.fx[0];
    let set = |id: &str, v: f32| p.values[FX_MODELS[m].param_index(id).unwrap()].store(v);
    set("spd", 2.0);
    set("phase", 180.0);
    set("depth", 100.0);
    let rack = engine.fx(0).unwrap();
    // The LFO turns at SPEED: 2 Hz is 1/24 of a cycle per 256-sample block... measured over half a second.
    run(&engine, 0, 2, sine(0.5));
    let start = rack.live(FxLive::LfoPhase);
    let blocks = (0.5 * SR) as usize / BLOCK;
    let mut needle = (f32::MAX, f32::MIN);
    let mut turned = 0.0;
    let mut last = start;
    for b in 0..blocks {
        run(&engine, 2 + b, 1, sine(0.5));
        let ph = rack.live(FxLive::LfoPhase);
        turned += (ph - last).rem_euclid(1.0);
        last = ph;
        let n = rack.live(FxLive::Needle);
        needle = (needle.0.min(n), needle.1.max(n));
    }
    let secs = (blocks * BLOCK) as f32 / SR;
    assert!((turned / secs - 2.0).abs() < 0.05, "LFO at {} Hz", turned / secs);
    // PHASE 180: the right LFO half a cycle on; the needle swings L to R.
    let off = (rack.live(FxLive::LfoPhase2) - rack.live(FxLive::LfoPhase)).rem_euclid(1.0);
    assert!((off - 0.5).abs() < 0.01, "right LFO {off} cycles on");
    assert!(needle.0 < -0.8 && needle.1 > 0.8, "needle {needle:?}");
    // The envelope follows the input.
    assert!(rack.live(FxLive::Envelope) > 0.3);
    run(&engine, 200, 200, |_| 0.0);
    assert!(rack.live(FxLive::Envelope) < 0.01);
    // A device without these reads 0; switching device clears them.
    assert!(engine.set_fx_model(0, model_index(FX_MODELS, "HALL").unwrap(), true));
    assert_eq!(engine.fx(0).unwrap().live(FxLive::LfoPhase), 0.0);
}
